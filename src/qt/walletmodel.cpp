// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/walletmodel.h>

#include <qt/addresstablemodel.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/paymentserver.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/transactiontablemodel.h>

#include <common/args.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/interface_ui.h>
#include <node/types.h>
#include <psbt.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/types.h>
#include <wallet/wallet.h>

#include <chrono>
#include <atomic>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <QDebug>
#include <QApplication>
#include <QThread>
#include <QMessageBox>
#include <QPointer>
#include <QSet>
#include <QTimer>

using wallet::CCoinControl;
using wallet::CRecipient;
using wallet::DEFAULT_DISABLE_WALLET;

static std::atomic_bool g_wallet_enabled{!DEFAULT_DISABLE_WALLET};

class WalletModelNotificationState
{
public:
    enum class Kind { OTHER, BALANCE, STATUS };
    explicit WalletModelNotificationState(WalletModel* model) : target(model) {}

    std::mutex mutex;
    WalletModel* target;
    bool balance_pending{false};
    bool status_pending{false};
    struct AddressUpdate {
        QString label;
        bool is_mine;
        wallet::AddressPurpose purpose;
        ChangeType status;
    };
    std::map<QString, AddressUpdate> addresses;
    bool addresses_pending{false};
    struct ProgressUpdate {
        QString title;
        int progress;
    };
    std::deque<ProgressUpdate> progress;
    std::optional<ProgressUpdate> last_progress;
    bool progress_pending{false};
};

// Wallet rescans report every 100 blocks, not only when the visible percentage
// changes. Merge redundant/pending progress before Qt posting, but preserve the
// start/end of each operation and yield between batches of distinct operations.
static void PostWalletProgressNotifications(const std::shared_ptr<WalletModelNotificationState>& state)
{
    // Called with the notification gate held, including when rescheduling.
    auto* target = state->target;
    const bool invoked = QMetaObject::invokeMethod(target, [state, target] {
        std::array<std::optional<WalletModelNotificationState::ProgressUpdate>, 64> updates;
        {
            std::lock_guard lock{state->mutex};
            if (state->target != target) return;
            for (auto& update : updates) {
                if (state->progress.empty()) break;
                update = std::move(state->progress.front());
                state->progress.pop_front();
            }
        }
        const QPointer<WalletModel> guard{target};
        for (const auto& update : updates) {
            if (!update) break;
            Q_EMIT target->showProgress(update->title, update->progress);
            if (!guard) return;
            // QProgressDialog::setValue may process events. Do not deliver
            // remaining values after a reentrant stop closes this gate.
            std::lock_guard lock{state->mutex};
            if (state->target != target) return;
        }
        std::lock_guard lock{state->mutex};
        if (state->target != target) return;
        if (state->progress.empty()) state->progress_pending = false;
        else PostWalletProgressNotifications(state);
    }, Qt::QueuedConnection);
    assert(invoked);
}

static void QueueWalletProgressNotification(const std::shared_ptr<WalletModelNotificationState>& state,
                                             QString title, int progress)
{
    WalletModelNotificationState::ProgressUpdate update{std::move(title), progress};
    std::lock_guard lock{state->mutex};
    if (!state->target) return;
    if (state->last_progress && state->last_progress->title == update.title && state->last_progress->progress == progress) return;
    state->last_progress = update;
    if (!state->progress.empty()) {
        auto& last = state->progress.back();
        const bool intermediate = progress > 0 && progress < 100 && last.progress > 0 && last.progress < 100;
        if (last.title == update.title && intermediate) {
            last = std::move(update);
            return;
        }
    }
    state->progress.push_back(std::move(update));
    if (!std::exchange(state->progress_pending, true)) PostWalletProgressNotifications(state);
}

// Address imports can emit thousands of changes while the GUI is busy. Only
// the final state of each address is relevant to the address-book cache, and
// each delivery must yield before processing the next bounded batch.
static void PostAddressBookNotifications(const std::shared_ptr<WalletModelNotificationState>& state)
{
    // Called with the notification gate held, including when rescheduling.
    auto* target = state->target;
    const bool invoked = QMetaObject::invokeMethod(target, [state, target] {
        std::map<QString, WalletModelNotificationState::AddressUpdate> updates;
        {
            std::lock_guard lock{state->mutex};
            if (state->target != target) return;
            while (!state->addresses.empty() && updates.size() < 64) {
                updates.insert(state->addresses.extract(state->addresses.begin()));
            }
        }
        const QPointer<WalletModel> guard{target};
        for (const auto& [address, update] : updates) {
            target->updateAddressBook(address, update.label, update.is_mine, update.purpose, update.status);
            if (!guard) return;
        }
        std::lock_guard lock{state->mutex};
        if (state->target != target) return;
        if (state->addresses.empty()) state->addresses_pending = false;
        else PostAddressBookNotifications(state);
    }, Qt::QueuedConnection);
    assert(invoked);
}

static void QueueAddressBookNotification(const std::shared_ptr<WalletModelNotificationState>& state,
                                        QString address, QString label, bool is_mine,
                                        wallet::AddressPurpose purpose, ChangeType status)
{
    std::lock_guard lock{state->mutex};
    if (!state->target) return;
    state->addresses.insert_or_assign(std::move(address), WalletModelNotificationState::AddressUpdate{
        std::move(label), is_mine, purpose, status});
    if (std::exchange(state->addresses_pending, true)) return;
    PostAddressBookNotifications(state);
}

template <typename Fn>
static void QueueWalletNotification(const std::shared_ptr<WalletModelNotificationState>& state, Fn callback,
                                    WalletModelNotificationState::Kind kind = WalletModelNotificationState::Kind::OTHER)
{
    std::lock_guard lock{state->mutex};
    auto* target = state->target;
    if (!target) return;
    bool* pending = kind == WalletModelNotificationState::Kind::BALANCE ? &state->balance_pending :
                    kind == WalletModelNotificationState::Kind::STATUS ? &state->status_pending : nullptr;
    if (pending && std::exchange(*pending, true)) return;
    // Target validation and enqueue must be atomic with closing the gate.
    // Always queue, even on the GUI thread: direct delivery could reenter
    // stopWorker() and try to lock this same mutex while it is held.
    const bool invoked = QMetaObject::invokeMethod(target, [state, target, callback = std::move(callback), kind] {
        {
            std::lock_guard delivery_lock{state->mutex};
            if (state->target != target) return;
            if (kind == WalletModelNotificationState::Kind::BALANCE) state->balance_pending = false;
            if (kind == WalletModelNotificationState::Kind::STATUS) state->status_pending = false;
        }
        // Delivery and model destruction share the GUI thread. Release the
        // gate before any signal or model work which might enter an event loop.
        callback(*target);
    }, Qt::QueuedConnection);
    assert(invoked);
}

WalletModel::WalletModel(std::unique_ptr<interfaces::Wallet> wallet, ClientModel& client_model, const PlatformStyle *platformStyle, QObject *parent) :
    QObject(parent),
    m_wallet(std::move(wallet)),
    m_notifications(std::make_shared<WalletModelNotificationState>(this)),
    m_client_model(&client_model),
    m_node(client_model.node()),
    optionsModel(client_model.getOptionsModel()),
    timer(new QTimer(this))
{
    m_refresh_worker.Start(1);
    // Subscribe before taking the address snapshot so a backend mutation
    // during model construction cannot be permanently missed by the cache.
    // Address notifications are queued and applied after construction.
    subscribeToCoreSignals();
    addressTableModel = new AddressTableModel(this);
    transactionTableModel = new TransactionTableModel(platformStyle, this);
    recentRequestsTableModel = new RecentRequestsTableModel(this);

    // This cache is also needed before balance polling starts and by hidden
    // receive pages. Its timer only runs while a refresh is pending.
    m_can_get_addresses_timer = new QTimer(this);
    m_can_get_addresses_timer->setInterval(MODEL_UPDATE_DELAY);
    connect(m_can_get_addresses_timer, &QTimer::timeout, this, &WalletModel::pollCanGetAddresses);
    // Start polling after model construction and view registration finish.
    // Queueing also respects affinity for non-controller/test owners.
    const bool invoked{QMetaObject::invokeMethod(this, [this] {
        requestCanGetAddressesUpdate();
        pollCanGetAddresses();
    }, Qt::QueuedConnection)};
    assert(invoked);
}

WalletModel::~WalletModel()
{
    for (auto* refresh_timer : findChildren<QTimer*>()) refresh_timer->stop();
    unsubscribeFromCoreSignals();
    // Fallback for non-GUI/test owners. Production calls stopWorker while the
    // model and its controller are fully alive; never reenter Qt in a destructor.
    m_refresh_worker.Stop();
}

void WalletModel::stopWorker()
{
    if (m_stopping) return;
    m_stopping = true;
    unsubscribeFromCoreSignals();
    for (auto* table : findChildren<TransactionTableModel*>()) table->interrupt();
    for (auto* refresh_timer : findChildren<QTimer*>()) refresh_timer->stop();
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    for (auto* child : findChildren<QObject*>()) QCoreApplication::removePostedEvents(child, QEvent::MetaCall);
    // Backend objects (including the chain) still exist here. Never leave
    // detached queries holding a wallet after node shutdown starts.
    m_refresh_worker.Interrupt();
    if (qApp && QThread::currentThread() == qApp->thread()) {
        GUIUtil::WaitForBackendTask(std::async(std::launch::async, [this] { m_refresh_worker.Stop(); }));
    } else {
        m_refresh_worker.Stop();
    }
}

void WalletModel::destroy(WalletModel* model)
{
    if (!model) return;
    GUIUtil::BackendOperationGuard operation;
    model->stopWorker();
    // Destroy GUI children without pumping events, then release the backend.
    // The last CWallet reference can stop claims and close its database.
    auto wallet = std::move(model->m_wallet);
    delete model;
    GUIUtil::WaitForBackendTask(std::async(std::launch::async,
        [wallet = std::move(wallet)]() mutable { wallet.reset(); }));
}

void WalletModel::startPollBalance()
{
    if (m_stopping) return;
    // Update the cached balance right away, so every view can make use of it,
    // so them don't need to waste resources recalculating it.
    pollBalanceChanged();

    // This timer will be fired repeatedly to update the balance
    // Since the QTimer::timeout is a private signal, it cannot be used
    // in the GUIUtil::ExceptionSafeConnect directly.
    connect(timer, &QTimer::timeout, this, &WalletModel::timerTimeout);
    GUIUtil::ExceptionSafeConnect(this, &WalletModel::timerTimeout, this, &WalletModel::pollBalanceChanged);
    timer->start(MODEL_UPDATE_DELAY);
}

void WalletModel::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (!m_client_model) timer->stop();
}

void WalletModel::updateStatus()
{
    requestCanGetAddressesUpdate();
}

void WalletModel::pollBalanceChanged()
{
    if (m_stopping) return;
    if (m_balance_query.valid()) {
        if (m_balance_query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
        try {
            const auto snapshot = m_balance_query.get();
            if (snapshot) {
                m_cached_last_update_tip = snapshot->block_hash;
                checkBalanceChanged(snapshot->balances);
                if (transactionTableModel) transactionTableModel->updateConfirmations();
            } else {
                fForceCheckBalanceChanged = true;
            }
        } catch (const std::exception& error) {
            fForceCheckBalanceChanged = true;
            qWarning() << "Wallet balance refresh failed:" << error.what();
        }
    }

    // Avoid recomputing wallet balances unless a TransactionChanged or
    // BlockTip notification was received.
    if (!fForceCheckBalanceChanged && m_cached_last_update_tip == getLastBlockProcessed()) return;

    // TRY_LOCK alone is insufficient: once acquired, computing a large
    // wallet's balance can still be expensive. Do the entire query off-thread.
    m_balance_query = requestWalletData([](interfaces::Wallet& wallet) -> std::optional<BalanceSnapshot> {
        BalanceSnapshot snapshot;
        if (!wallet.tryGetBalances(snapshot.balances, snapshot.block_hash)) return std::nullopt;
        return snapshot;
    });
    // Clear at dispatch, not completion, so notifications arriving while the
    // query runs are preserved and cause another refresh (including same-tip
    // mempool changes).
    fForceCheckBalanceChanged = false;
}

void WalletModel::checkBalanceChanged(const interfaces::WalletBalances& new_balances)
{
    if (new_balances.balanceChanged(m_cached_balances)) {
        m_cached_balances = new_balances;
        Q_EMIT balanceChanged(new_balances);
    }
}

interfaces::WalletBalances WalletModel::getCachedBalance() const
{
    return m_cached_balances;
}

void WalletModel::requestCanGetAddressesUpdate()
{
    if (m_stopping) return;
    m_can_get_addresses_dirty = true;
    if (!m_can_get_addresses_timer->isActive()) m_can_get_addresses_timer->start();
}

void WalletModel::pollCanGetAddresses()
{
    if (m_stopping) return;
    std::optional<WalletStatusSnapshot> ready;
    if (m_can_get_addresses_query.valid()) {
        if (m_can_get_addresses_query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
        try {
            const auto snapshot{m_can_get_addresses_query.get()};
            // A notification received during this query requires a fresh
            // snapshot; do not briefly publish an already invalidated result.
            if (!m_can_get_addresses_dirty) ready = snapshot;
        } catch (const std::exception& error) {
            m_can_get_addresses_dirty = true;
            qWarning() << "Wallet receiving capability refresh failed:" << error.what();
        }
    }

    if (m_can_get_addresses_dirty) {
        try {
            m_can_get_addresses_query = requestWalletData(&WalletModel::readWalletStatus);
            m_can_get_addresses_dirty = false;
        } catch (const std::exception& error) {
            qWarning() << "Wallet receiving capability refresh failed:" << error.what();
        }
    } else {
        m_can_get_addresses_timer->stop();
    }
    if (ready) applyWalletStatus(*ready);
}

WalletModel::WalletStatusSnapshot WalletModel::readWalletStatus(interfaces::Wallet& wallet)
{
    const auto encryption = !wallet.isCrypted() ? (wallet.privateKeysDisabled() ? NoKeys : Unencrypted) :
                            wallet.isLocked() ? Locked : Unlocked;
    return {wallet.canGetAddresses(), encryption, wallet.hdEnabled()};
}

void WalletModel::applyWalletStatus(const WalletStatusSnapshot& snapshot)
{
    const bool capability_changed = m_cached_can_get_addresses != snapshot.can_get_addresses;
    const bool security_changed = cachedEncryptionStatus != snapshot.encryption || m_cached_hd_enabled != snapshot.hd_enabled;
    m_cached_can_get_addresses = snapshot.can_get_addresses;
    cachedEncryptionStatus = snapshot.encryption;
    m_cached_hd_enabled = snapshot.hd_enabled;
    if (capability_changed) Q_EMIT canGetAddressesChanged();
    if (security_changed) Q_EMIT encryptionStatusChanged();
}

void WalletModel::refreshWalletStatusForAction()
{
    const auto snapshot = GUIUtil::WaitForBackendTask(requestWalletData(&WalletModel::readWalletStatus));
    // Invalidate any older periodic snapshot before publishing action results.
    requestCanGetAddressesUpdate();
    applyWalletStatus(snapshot);
}

void WalletModel::updateTransaction()
{
    if (m_stopping) return;
    // Balance and number of transactions might have changed
    fForceCheckBalanceChanged = true;
}

void WalletModel::updateAddressBook(const QString &address, const QString &label,
        bool isMine, wallet::AddressPurpose purpose, int status)
{
    if (m_stopping) return;
    // A refreshed model can coexist with older models still used by address
    // book dialogs. Keep every surviving view's lock-free lookup cache current.
    for (auto* model : findChildren<AddressTableModel*>(QString{}, Qt::FindDirectChildrenOnly)) {
        model->updateEntry(address, label, isMine, purpose, status);
    }
}

bool WalletModel::validateAddress(const QString& address) const
{
    return IsValidDestinationString(address.toStdString());
}

WalletModel::SendCoinsReturn WalletModel::prepareTransaction(WalletModelTransaction &transaction, const CCoinControl& coinControl)
{
    GUIUtil::BackendOperationGuard operation;
    transaction.setWtx(nullptr);
    const auto recipients = transaction.getRecipients();
    if (recipients.empty()) return OK;

    struct Prepared {
        StatusCode status{OK};
        QString error;
        std::unique_ptr<WalletModelTransaction> transaction;
    };
    try {
        // All transaction-sized work belongs to the worker, including address
        // decoding, selected-coin balance, fee redistribution and vsize. The
        // GUI receives only a completed value object and never serializes it.
        auto prepared = GUIUtil::WaitForBackendTask(requestWalletData([recipients, coinControl, cached_balance = getCachedBalance().balance](interfaces::Wallet& wallet) {
            CAmount total{0};
            bool subtract_fee{false};
            QSet<QString> addresses;
            std::vector<CRecipient> outputs;
            outputs.reserve(recipients.size());
            for (const auto& recipient : recipients) {
                auto destination = DecodeDestination(recipient.address.toStdString());
                if (!IsValidDestination(destination)) return Prepared{InvalidAddress, {}, {}};
                if (recipient.amount <= 0 || !MoneyRange(recipient.amount) || recipient.amount > MAX_MONEY - total) {
                    return Prepared{InvalidAmount, {}, {}};
                }
                addresses.insert(recipient.address);
                total += recipient.amount;
                subtract_fee |= recipient.fSubtractFeeFromAmount;
                outputs.emplace_back(CRecipient{std::move(destination), recipient.amount, recipient.fSubtractFeeFromAmount});
            }
            if (addresses.size() != recipients.size()) return Prepared{DuplicateAddress, {}, {}};
            const CAmount balance = coinControl.HasSelected() ? wallet.getAvailableBalance(coinControl) : cached_balance;
            if (total > balance) return Prepared{AmountExceedsBalance, {}, {}};

            const auto created = wallet.createTransaction(outputs, coinControl, /*sign=*/!wallet.privateKeysDisabled(), /*change_pos=*/std::nullopt);
            if (!created) return Prepared{TransactionCreationFailed, QString::fromStdString(util::ErrorString(created).translated), {}};
            auto ready = std::make_unique<WalletModelTransaction>(recipients);
            ready->setWtx(created->tx);
            ready->setTransactionFee(created->fee);
            if (subtract_fee && ready->getWtx()) ready->reassignAmounts(static_cast<int>(created->change_pos.value_or(-1)));
            // Defense in depth: createTransaction already enforces this fee limit.
            const auto status = created->fee > wallet.getDefaultMaxTxFee() ? AbsurdFee : OK;
            return Prepared{status, {}, std::move(ready)};
        }));
        if (!prepared.error.isEmpty()) {
            Q_EMIT message(tr("Send Coins"), prepared.error, CClientUIInterface::MSG_ERROR);
        }
        if (prepared.transaction) transaction = std::move(*prepared.transaction);
        return prepared.status;
    } catch (const std::runtime_error& err) {
        // Something unexpected happened, instruct user to report this bug.
        Q_EMIT message(tr("Send Coins"), QString::fromStdString(err.what()),
                       CClientUIInterface::MSG_ERROR);
        return TransactionCreationFailed;
    }
}

void WalletModel::sendCoins(WalletModelTransaction& transaction)
{
    GUIUtil::BackendOperationGuard operation;
    const auto recipients = transaction.getRecipients();
    const auto completed = GUIUtil::WaitForBackendTask(requestWalletData([recipients, new_tx = transaction.getWtx()](interfaces::Wallet& wallet) {
        std::vector<std::string> messages;
        for (const SendCoinsRecipient &rcp : recipients)
        {
            if (!rcp.message.isEmpty()) { // Message from normal connectcoin: URI.
                messages.emplace_back(rcp.message.toStdString());
            }
        }

        wallet.commitTransaction(new_tx, messages);

        DataStream ssTx;
        ssTx << TX_WITH_WITNESS(*new_tx);
        QByteArray transaction_array((const char*)ssTx.data(), ssTx.size());
        for (const SendCoinsRecipient &rcp : recipients) {
            std::string strAddress = rcp.address.toStdString();
            CTxDestination dest = DecodeDestination(strAddress);
            std::string strLabel = rcp.label.toStdString();
            {
                // Check if we have a new address or an updated label
                std::string name;
                if (!wallet.getAddress(
                     dest, &name, /*purpose=*/nullptr))
                {
                    wallet.setAddressBook(dest, strLabel, wallet::AddressPurpose::SEND);
                }
                else if (name != strLabel)
                {
                    wallet.setAddressBook(dest, strLabel, {}); // {} means don't change purpose
                }
            }
        }
        return std::pair{transaction_array, wallet.getBalances()};
    }));
    for (const auto& rcp : recipients) Q_EMIT coinsSent(this, rcp, completed.first);
    checkBalanceChanged(completed.second);
}

OptionsModel* WalletModel::getOptionsModel() const
{
    return optionsModel;
}

AddressTableModel* WalletModel::getAddressTableModel() const
{
    return addressTableModel;
}

TransactionTableModel* WalletModel::getTransactionTableModel() const
{
    return transactionTableModel;
}

RecentRequestsTableModel* WalletModel::getRecentRequestsTableModel() const
{
    return recentRequestsTableModel;
}

WalletModel::EncryptionStatus WalletModel::getEncryptionStatus() const
{
    return cachedEncryptionStatus;
}

bool WalletModel::setWalletEncrypted(const SecureString& passphrase)
{
    GUIUtil::BackendOperationGuard operation;
    const bool result = GUIUtil::WaitForBackendTask(requestWalletData([passphrase](interfaces::Wallet& wallet) { return wallet.encryptWallet(passphrase); }));
    refreshWalletStatusForAction();
    return result;
}

bool WalletModel::setWalletLocked(bool locked, const SecureString &passPhrase)
{
    GUIUtil::BackendOperationGuard operation;
    const bool result = GUIUtil::WaitForBackendTask(requestWalletData([locked, passPhrase](interfaces::Wallet& wallet) {
        return locked ? wallet.lock() : wallet.unlock(passPhrase);
    }));
    refreshWalletStatusForAction();
    return result;
}

bool WalletModel::changePassphrase(const SecureString &oldPass, const SecureString &newPass)
{
    GUIUtil::BackendOperationGuard operation;
    const bool result = GUIUtil::WaitForBackendTask(requestWalletData([oldPass, newPass](interfaces::Wallet& wallet) {
        wallet.lock();
        return wallet.changeWalletPassphrase(oldPass, newPass);
    }));
    refreshWalletStatusForAction();
    return result;
}

void WalletModel::subscribeToCoreSignals()
{
    // btcsignals::disconnect() cancels future callbacks but does not join an
    // already-entered one. Such callbacks may only access their shared gate.
    m_handler_unload = m_wallet->handleUnload([state = m_notifications] {
        QueueWalletNotification(state, [](WalletModel& model) { Q_EMIT model.unload(); });
    });
    const auto status_changed = [state = m_notifications] {
        QueueWalletNotification(state, [](WalletModel& model) { model.updateStatus(); }, WalletModelNotificationState::Kind::STATUS);
    };
    m_handler_status_changed = m_wallet->handleStatusChanged(status_changed);
    m_handler_address_book_changed = m_wallet->handleAddressBookChanged([state = m_notifications](
            const CTxDestination& address, const std::string& label, bool is_mine, wallet::AddressPurpose purpose, ChangeType status) {
        QueueAddressBookNotification(state, QString::fromStdString(EncodeDestination(address)),
                                     QString::fromStdString(label), is_mine, purpose, status);
    });
    m_handler_transaction_changed = m_wallet->handleTransactionChanged([state = m_notifications](const Txid&, ChangeType) {
        QueueWalletNotification(state, [](WalletModel& model) { model.updateTransaction(); }, WalletModelNotificationState::Kind::BALANCE);
    });
    m_handler_show_progress = m_wallet->handleShowProgress([state = m_notifications](const std::string& title, int progress) {
        QueueWalletProgressNotification(state, QString::fromStdString(title), progress);
    });
    m_handler_can_get_addrs_changed = m_wallet->handleCanGetAddressesChanged(status_changed);
}

void WalletModel::unsubscribeFromCoreSignals()
{
    {
        std::lock_guard lock{m_notifications->mutex};
        m_notifications->target = nullptr;
    }
    // Disconnect signals from wallet
    m_handler_unload->disconnect();
    m_handler_status_changed->disconnect();
    m_handler_address_book_changed->disconnect();
    m_handler_transaction_changed->disconnect();
    m_handler_show_progress->disconnect();
    m_handler_can_get_addrs_changed->disconnect();
}

// WalletModel::UnlockContext implementation
WalletModel::UnlockContext WalletModel::requestUnlock()
{
    GUIUtil::BackendOperationGuard operation;
    refreshWalletStatusForAction();
    // Bugs in earlier versions may have resulted in wallets with private keys disabled to become "encrypted"
    // (encryption keys are present, but not actually doing anything).
    // To avoid issues with such wallets, check if the wallet has private keys disabled, and if so, return a context
    // that indicates the wallet is not encrypted.
    if (m_wallet->privateKeysDisabled()) {
        return UnlockContext(this, /*valid=*/true, /*relock=*/false);
    }
    bool was_locked = getEncryptionStatus() == Locked;
    if(was_locked)
    {
        // Request UI to unlock wallet
        Q_EMIT requireUnlock();
        // Unlock handlers can call the backend directly (for example a
        // hardware integration). Its queued status signal may not have
        // refreshed the cache yet; decide validity/relocking from fresh state.
        refreshWalletStatusForAction();
    }
    // If wallet is still locked, unlock was failed or cancelled, mark context as invalid
    bool valid = getEncryptionStatus() != Locked;

    return UnlockContext(this, valid, was_locked);
}

WalletModel::UnlockContext::UnlockContext(WalletModel *_wallet, bool _valid, bool _relock):
        wallet(_wallet),
        valid(_valid),
        relock(_relock)
{
}

WalletModel::UnlockContext::~UnlockContext()
{
    if(valid && relock)
    {
        wallet->setWalletLocked(true);
    }
}

bool WalletModel::bumpFee(Txid hash, Txid& new_hash)
{
    GUIUtil::BackendOperationGuard operation;
    CCoinControl coin_control;
    struct BumpDraft {
        bool success{false};
        std::vector<bilingual_str> errors;
        CAmount old_fee{0}, new_fee{0};
        CMutableTransaction tx;
    };
    auto draft = GUIUtil::WaitForBackendTask(requestWalletData([hash, coin_control](interfaces::Wallet& wallet) {
        BumpDraft result;
        result.success = wallet.createBumpTransaction(hash, coin_control, result.errors, result.old_fee, result.new_fee, result.tx);
        return result;
    }));
    auto& errors = draft.errors;
    const auto old_fee = draft.old_fee;
    const auto new_fee = draft.new_fee;
    auto& mtx = draft.tx;
    if (!draft.success) {
        QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Increasing transaction fee failed") + "<br />(" +
            (errors.size() ? QString::fromStdString(errors[0].translated) : "") +")");
        return false;
    }

    // allow a user based fee verification
    /*: Asks a user if they would like to manually increase the fee of a transaction that has already been created. */
    QString questionString = tr("Do you want to increase the fee?");
    questionString.append("<br />");
    questionString.append("<table style=\"text-align: left;\">");
    questionString.append("<tr><td>");
    questionString.append(tr("Current fee:"));
    questionString.append("</td><td>");
    questionString.append(BitcoinUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(), old_fee));
    questionString.append("</td></tr><tr><td>");
    questionString.append(tr("Increase:"));
    questionString.append("</td><td>");
    questionString.append(BitcoinUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(), new_fee - old_fee));
    questionString.append("</td></tr><tr><td>");
    questionString.append(tr("New fee:"));
    questionString.append("</td><td>");
    questionString.append(BitcoinUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(), new_fee));
    questionString.append("</td></tr></table>");

    // Display warning in the "Confirm fee bump" window if the "Coin Control Features" option is enabled
    if (getOptionsModel()->getCoinControlFeatures()) {
        questionString.append("<br><br>");
        questionString.append(tr("Warning: This may pay the additional fee by reducing change outputs or adding inputs, when necessary. It may add a new change output if one does not already exist. These changes may potentially leak privacy."));
    }

    const bool enable_send{!wallet().privateKeysDisabled() || wallet().hasExternalSigner()};
    const bool always_show_unsigned{getOptionsModel()->getEnablePSBTControls()};
    auto confirmationDialog = new SendConfirmationDialog(tr("Confirm fee bump"), questionString, "", "", SEND_CONFIRM_DELAY, enable_send, always_show_unsigned, nullptr);
    confirmationDialog->setAttribute(Qt::WA_DeleteOnClose);
    // TODO: Replace QDialog::exec() with safer QDialog::show().
    const auto retval = static_cast<QMessageBox::StandardButton>(confirmationDialog->exec());

    // cancel sign&broadcast if user doesn't want to bump the fee
    if (retval != QMessageBox::Yes && retval != QMessageBox::Save) {
        return false;
    }

    // Short-circuit if we are returning a bumped transaction PSBT to clipboard
    if (retval == QMessageBox::Save) {
        // "Create Unsigned" clicked
        const auto psbt = GUIUtil::WaitForBackendTask(requestWalletData([mtx = std::move(mtx)](interfaces::Wallet& wallet) -> std::optional<std::string> {
            PartiallySignedTransaction psbtx(mtx);
            bool complete = false;
            if (wallet.fillPSBT({.sign = false, .bip32_derivs = true}, nullptr, psbtx, complete) || complete) return std::nullopt;
            DataStream stream;
            stream << psbtx;
            return EncodeBase64(stream.str());
        }));
        if (!psbt) {
            QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Can't draft transaction."));
            return false;
        }
        // Serialize the PSBT
        GUIUtil::setClipboard(psbt->c_str());
        Q_EMIT message(tr("PSBT copied"), tr("Fee-bump PSBT copied to clipboard"), CClientUIInterface::MSG_INFORMATION | CClientUIInterface::MODAL);
        return true;
    }

    WalletModel::UnlockContext ctx(requestUnlock());
    if (!ctx.isValid()) {
        return false;
    }

    assert(!m_wallet->privateKeysDisabled() || wallet().hasExternalSigner());

    // sign bumped transaction
    auto signed_tx = GUIUtil::WaitForBackendTask(requestWalletData([mtx = std::move(mtx)](interfaces::Wallet& wallet) mutable {
        const bool success = wallet.signBumpTransaction(mtx);
        return std::pair{success, std::move(mtx)};
    }));
    if (!signed_tx.first) {
        QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Can't sign transaction."));
        return false;
    }
    // commit the bumped transaction
    struct BumpResult { bool success; Txid hash; std::vector<bilingual_str> errors; };
    auto result = GUIUtil::WaitForBackendTask(requestWalletData([hash, tx = std::move(signed_tx.second)](interfaces::Wallet& wallet) mutable {
        BumpResult result{};
        result.success = wallet.commitBumpTransaction(hash, std::move(tx), result.errors, result.hash);
        return result;
    }));
    if (!result.success) {
        QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Could not commit transaction") + "<br />(" +
            (result.errors.empty() ? QString{} : QString::fromStdString(result.errors[0].translated))+")");
        return false;
    }
    new_hash = result.hash;
    return true;
}

void WalletModel::displayAddress(std::string sAddress)
{
    GUIUtil::BackendOperationGuard operation;
    CTxDestination dest = DecodeDestination(sAddress);
    try {
        auto result = GUIUtil::WaitForBackendTask(requestWalletData([dest](interfaces::Wallet& wallet) { return std::make_unique<util::Result<void>>(wallet.displayAddress(dest)); }));
        if (!*result) {
            QMessageBox::warning(nullptr, tr("Signer error"), QString::fromStdString(util::ErrorString(*result).translated));
        }
    } catch (const std::runtime_error& e) {
        QMessageBox::critical(nullptr, tr("Can't display address"), e.what());
    }
}

bool WalletModel::isWalletEnabled()
{
    return g_wallet_enabled.load(std::memory_order_relaxed);
}

void WalletModel::refreshWalletEnabled()
{
    g_wallet_enabled.store(!gArgs.GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET), std::memory_order_relaxed);
}

QString WalletModel::getWalletName() const
{
    return QString::fromStdString(m_wallet->getWalletName());
}

QString WalletModel::getDisplayName() const
{
    return GUIUtil::WalletDisplayName(getWalletName());
}

bool WalletModel::isMultiwallet() const
{
    return GUIUtil::WaitForBackendTask(m_client_model->requestNodeData([](interfaces::Node& node) {
        return node.walletLoader().getWallets().size() > 1;
    }));
}

void WalletModel::refresh(bool pk_hash_only)
{
    // Each cache stays current through address-book notifications. Reopening
    // the signing selector must not retain another full cache and snapshot
    // query, or multiply the work of every subsequent address notification.
    for (auto* model : findChildren<AddressTableModel*>(QString{}, Qt::FindDirectChildrenOnly)) {
        if (model->isPkHashOnly() == pk_hash_only) {
            addressTableModel = model;
            return;
        }
    }
    addressTableModel = new AddressTableModel(this, pk_hash_only);
}

uint256 WalletModel::getLastBlockProcessed() const
{
    return m_client_model ? m_client_model->getBestBlockHash() : uint256{};
}

CAmount WalletModel::getAvailableBalance(const CCoinControl* control)
{
    // No selected coins, return the cached balance
    if (!control || !control->HasSelected()) {
        const interfaces::WalletBalances& balances = getCachedBalance();
        return balances.balance;
    }
    // Fetch balance from the wallet, taking into account the selected coins
    return GUIUtil::WaitForBackendTask(requestWalletData([control = *control](interfaces::Wallet& wallet) { return wallet.getAvailableBalance(control); }));
}
