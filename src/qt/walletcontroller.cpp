// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/walletcontroller.h>

#include <qt/askpassphrasedialog.h>
#include <qt/clientmodel.h>
#include <qt/createwalletdialog.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/walletmodel.h>

#include <external_signer.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <util/string.h>
#include <util/threadnames.h>
#include <util/translation.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include <QApplication>
#include <QCheckBox>
#include <QDebug>
#include <QEvent>
#include <QMessageBox>
#include <QMetaObject>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <QWindow>

using util::Join;
using wallet::WALLET_FLAG_BLANK_WALLET;
using wallet::WALLET_FLAG_DESCRIPTORS;
using wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS;
using wallet::WALLET_FLAG_EXTERNAL_SIGNER;

/** Loader callbacks own this gate, never a raw controller pointer. Closing it
 * cancels queued registrations and wakes loaders without requiring a GUI
 * event to run. In particular, a loader holding wallets_mutex cannot deadlock
 * a synchronous destructor waiting to disconnect the loader handler.
 */
class WalletControllerLoadState : public std::enable_shared_from_this<WalletControllerLoadState>
{
public:
    explicit WalletControllerLoadState(WalletController* controller) : target(controller) {}

    WalletModel* getOrCreate(std::unique_ptr<interfaces::Wallet> wallet)
    {
        struct Request {
            std::unique_ptr<interfaces::Wallet> wallet;
            WalletModel* result{nullptr};
            bool done{false};
        };
        std::unique_lock lock(mutex);
        auto* controller = target;
        if (!controller) return nullptr;
        ++active;
        lock.unlock();
        const auto finished = qScopeGuard([this] {
            std::lock_guard lock(mutex);
            --active;
            idle.notify_all();
        });
        if (QThread::currentThread() == controller->thread()) {
            return controller->registerWalletOnGUI(std::move(wallet));
        }

        auto request = std::make_shared<Request>();
        request->wallet = std::move(wallet);
        lock.lock();
        if (target == controller) {
            // Enqueue atomically with checking the target gate. The request
            // owns its input; no callback captures a waiting stack reference.
            const bool queued = QMetaObject::invokeMethod(controller, [state = shared_from_this(), controller, request] {
                std::unique_ptr<interfaces::Wallet> input;
                {
                    std::lock_guard lock(state->mutex);
                    if (state->target != controller) return;
                    input = std::move(request->wallet);
                }
                auto* result = controller->registerWalletOnGUI(std::move(input));
                std::lock_guard lock(state->mutex);
                request->result = result;
                request->done = true;
                state->idle.notify_all();
            }, Qt::QueuedConnection);
            assert(queued);
            idle.wait(lock, [&] { return request->done || target != controller; });
        }
        auto* result = request->done ? request->result : nullptr;
        // Release an unregistered backend interface on the loader, not when
        // Qt later removes the cancelled callback from the GUI event queue.
        auto unused_wallet = std::move(request->wallet);
        lock.unlock();
        return result;
    }

    void close()
    {
        std::lock_guard lock(mutex);
        target = nullptr;
        idle.notify_all();
    }

    void wait()
    {
        std::unique_lock lock(mutex);
        idle.wait(lock, [this] { return active == 0; });
    }

    bool busy() const
    {
        std::lock_guard lock(mutex);
        return active != 0;
    }

private:
    mutable std::mutex mutex;
    std::condition_variable idle;
    WalletController* target;
    unsigned int active{0};
};

WalletController::WalletController(ClientModel& client_model, const PlatformStyle* platform_style, QObject* parent)
    : QObject(parent)
    , m_activity_thread(new QThread(this))
    , m_activity_worker(new QObject)
    , m_client_model(client_model)
    , m_node(client_model.node())
    , m_platform_style(platform_style)
    , m_options_model(client_model.getOptionsModel())
    , m_load_state(std::make_shared<WalletControllerLoadState>(this))
{
    m_activity_worker->moveToThread(m_activity_thread);
    connect(m_activity_thread, &QThread::finished, m_activity_worker, &QObject::deleteLater);
    m_activity_thread->start();
    QTimer::singleShot(0, m_activity_worker, []() {
        util::ThreadRename("qt-walletctrl");
    });
    connect(this, &WalletController::walletAdded, this, [this](WalletModel* model) {
        if (!m_stopping) m_loaded_wallet_names.insert(model->getWalletName().toStdString());
    });
    m_wallet_dir_timer = new QTimer(this);
    m_wallet_dir_timer->setInterval(100);
    connect(m_wallet_dir_timer, &QTimer::timeout, this, &WalletController::pollWalletDir);
    listWalletDir();
}

void WalletController::subscribeToCoreSignals()
{
    // Registration takes wallets_mutex, which can be occupied by an RPC load.
    // LoadWalletsActivity keeps us alive and performs this before getWallets(),
    // so a load between registration and that snapshot cannot be missed.
    assert(QThread::currentThread() == m_activity_thread);
    if (m_stopping) return;
    assert(!m_handler_load_wallet);
    m_handler_load_wallet = m_node.walletLoader().handleLoadWallet([state = m_load_state](std::unique_ptr<interfaces::Wallet> wallet) {
        state->getOrCreate(std::move(wallet));
    });
}

// Not using the default destructor because not all member types definitions are
// available in the header, just forward declared.
WalletController::~WalletController()
{
    // Fallback only: never pump events while a QObject is being destroyed.
    // The registration gate cancels loaders which would otherwise wait on GUI.
    if (!m_stop_complete) stopImpl(/*responsive=*/false);
}

void WalletController::stop()
{
    if (m_stopping) return;
    GUIUtil::BackendOperationGuard operation;
    stopImpl(/*responsive=*/true);
}

void WalletController::stopImpl(bool responsive)
{
    m_stopping = true;
    m_load_state->close();
    m_wallet_dir_timer->stop();
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    std::vector<WalletModel*> wallets;
    {
        QMutexLocker locker(&m_mutex);
        wallets.swap(m_wallets);
    }
    // Release existing models before joining activities: a migration can be
    // waiting for its old wallet references to disappear. Finish callbacks
    // observe m_stopping and never publish these obsolete model pointers.
    for (WalletModel* model : wallets) {
        Q_EMIT walletRemoved(model);
        if (responsive) WalletModel::destroy(model);
        else delete model;
    }
    m_wallets_pending_removal.clear();
    m_loaded_wallet_names.clear();
    // Shutdown callers wait for active user operations first. Quit also
    // cancels activity timers which have not begun executing yet.
    m_activity_thread->quit();
    const auto drain = [this] {
        m_activity_thread->wait();
        // Subscription itself runs on the activity thread, so join it before
        // touching the handler. Disconnect can block on wallets_mutex.
        if (m_handler_load_wallet) m_handler_load_wallet->disconnect();
        m_handler_load_wallet.reset();
        m_load_state->wait();
    };
    if (responsive) GUIUtil::WaitForBackendTask(std::async(std::launch::async, drain));
    else drain();
    for (auto* activity : findChildren<WalletControllerActivity*>(QString{}, Qt::FindDirectChildrenOnly)) delete activity;

    m_stop_complete = true;
}

std::map<std::string, std::pair<bool, std::string>> WalletController::listWalletDir()
{
    if (!m_stopping && !m_wallet_dir_query.valid()) {
        m_wallet_dir_query = m_client_model.requestNodeData([](interfaces::Node& node) {
            // The filesystem listing and its ordered menu snapshot both
            // belong to the worker; the GUI only takes the completed map.
            std::map<std::string, std::string> snapshot;
            for (auto& [name, format] : node.walletLoader().listWalletDir()) {
                snapshot.emplace(std::move(name), std::move(format));
            }
            return snapshot;
        });
        m_wallet_dir_timer->start();
    }
    std::map<std::string, std::pair<bool, std::string>> wallets;
    for (const auto& [name, format] : m_cached_wallet_dir) {
        wallets[name] = std::make_pair(m_loaded_wallet_names.count(name) != 0, format);
    }
    return wallets;
}

void WalletController::pollWalletDir()
{
    if (m_stopping) return;
    if (!m_wallet_dir_query.valid() || m_wallet_dir_query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
    m_wallet_dir_timer->stop();
    try {
        auto snapshot = m_wallet_dir_query.get();
        const bool changed = !m_has_wallet_dir_snapshot || snapshot != m_cached_wallet_dir;
        m_cached_wallet_dir = std::move(snapshot);
        m_has_wallet_dir_snapshot = true;
        if (changed) Q_EMIT walletDirectoryChanged();
    } catch (const std::exception& error) {
        qWarning() << "Wallet directory refresh failed:" << error.what();
    }
}

bool WalletController::hasActiveActivities() const
{
    if (m_load_state->busy()) return true;
    for (const auto* activity : findChildren<WalletControllerActivity*>()) {
        if (activity->hasActiveBackendWork()) return true;
    }
    return false;
}

void WalletController::removeWallet(WalletModel* wallet_model)
{
    // Once the wallet is successfully removed from the node, the model will emit the 'WalletModel::unload' signal.
    // This signal is already connected and will complete the removal of the view from the GUI.
    // Look at 'WalletController::getOrCreateWallet' for the signal connection.
    GUIUtil::WaitForBackendTask(wallet_model->requestWalletData([](interfaces::Wallet& wallet) { wallet.remove(); }));
}

void WalletController::closeWallet(WalletModel* wallet_model, QWidget* parent)
{
    if (m_stopping) return;
    GUIUtil::BackendOperationGuard operation;
    QMessageBox box(parent);
    box.setWindowTitle(tr("Close wallet"));
    box.setText(tr("Are you sure you wish to close the wallet <i>%1</i>?").arg(GUIUtil::HtmlEscape(wallet_model->getDisplayName())));
    box.setInformativeText(tr("Closing the wallet for too long can result in having to resync the entire chain if pruning is enabled."));
    box.setStandardButtons(QMessageBox::Yes|QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Yes);
    if (box.exec() != QMessageBox::Yes) return;

    removeWallet(wallet_model);
}

void WalletController::closeAllWallets(QWidget* parent)
{
    if (m_stopping) return;
    GUIUtil::BackendOperationGuard operation;
    QMessageBox::StandardButton button = QMessageBox::question(parent, tr("Close all wallets"),
        tr("Are you sure you wish to close all wallets?"),
        QMessageBox::Yes|QMessageBox::Cancel,
        QMessageBox::Yes);
    if (button != QMessageBox::Yes) return;

    std::vector<WalletModel*> wallets;
    {
        QMutexLocker locker(&m_mutex);
        wallets = m_wallets;
    }
    for (WalletModel* wallet_model : wallets) {
        removeWallet(wallet_model);
    }
}

WalletModel* WalletController::getOrCreateWallet(std::unique_ptr<interfaces::Wallet> wallet)
{
    return m_load_state->getOrCreate(std::move(wallet));
}

WalletModel* WalletController::registerWalletOnGUI(std::unique_ptr<interfaces::Wallet> wallet)
{
    assert(QThread::currentThread() == thread());
    GUIUtil::BackendOperationGuard operation;
    if (m_stopping) return nullptr;
    const std::string name = wallet->getWalletName();
    {
        QMutexLocker locker(&m_mutex);
        for (WalletModel* wallet_model : m_wallets) {
            if (wallet_model->getWalletName().toStdString() == name) return wallet_model;
        }
    }

    // Model constructors only enqueue backend snapshots. Construct on the GUI
    // after the cancellable request is accepted, never leave an unregistered
    // QObject on a loader which shutdown would need to delete cross-thread.
    WalletModel* wallet_model = new WalletModel(std::move(wallet), m_client_model, m_platform_style,
                                                this);
    {
        QMutexLocker locker(&m_mutex);
        m_wallets.push_back(wallet_model);
    }
    connect(wallet_model, &WalletModel::unload, this, [this, wallet_model] {
        if (!m_stopping && m_wallets_pending_removal.insert(wallet_model).second) removeWalletWhenReady(wallet_model);
    }, Qt::QueuedConnection);
    connect(wallet_model, &WalletModel::coinsSent, this, &WalletController::coinsSent);
    wallet_model->startPollBalance();
    Q_EMIT walletAdded(wallet_model);
    return wallet_model;
}

void WalletController::removeWalletWhenReady(WalletModel* wallet_model)
{
    if (m_stopping || m_wallets_pending_removal.count(wallet_model) == 0) return;
    // A modal backend wait still runs queued unload notifications. Do not
    // destroy either the wallet or its caller until that whole action returns.
    // Retain the existing protection for other synchronous modal dialogs.
    QWidget* active_dialog = QApplication::activeModalWidget();
    if (GUIUtil::HasActiveBackendOperation() ||
        (active_dialog && dynamic_cast<QProgressDialog*>(active_dialog) == nullptr)) {
        QTimer::singleShot(25, this, [this, wallet_model] { removeWalletWhenReady(wallet_model); });
        return;
    }
    removeAndDeleteWallet(wallet_model);
}

void WalletController::removeAndDeleteWallet(WalletModel* wallet_model)
{
    GUIUtil::BackendOperationGuard operation;
    m_loaded_wallet_names.erase(wallet_model->getWalletName().toStdString());
    // Unregister wallet model.
    {
        QMutexLocker locker(&m_mutex);
        m_wallets.erase(std::remove(m_wallets.begin(), m_wallets.end(), wallet_model));
    }
    Q_EMIT walletRemoved(wallet_model);
    wallet_model->stopWorker();
    m_wallets_pending_removal.erase(wallet_model);
    // Currently this can trigger the unload since the model can hold the last
    // CWallet shared pointer.
    WalletModel::destroy(wallet_model);
}

WalletControllerActivity::WalletControllerActivity(WalletController* wallet_controller, QWidget* parent_widget)
    : QObject(wallet_controller)
    , m_wallet_controller(wallet_controller)
    , m_parent_widget(parent_widget)
{
    connect(this, &WalletControllerActivity::finished, this, &QObject::deleteLater);
    connect(this, &WalletControllerActivity::finished, this, [this] {
        GUIUtil::BackendOperationGuard unwind;
        m_backend_active = false;
    });
}

WalletControllerActivity::~WalletControllerActivity()
{
    delete m_progress_dialog.data();
}

void WalletControllerActivity::showProgressDialog(const QString& title_text, const QString& label_text, bool show_minimized)
{
    // Unlike a synchronous action, migration must be allowed to unload old
    // wallet models while its worker is active. Defer shutdown separately.
    m_backend_active = true;
    auto progress_dialog = new QProgressDialog(m_parent_widget);
    m_progress_dialog = progress_dialog;
    progress_dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(this, &WalletControllerActivity::finished, progress_dialog, &QWidget::close);

    progress_dialog->setWindowTitle(title_text);
    progress_dialog->setLabelText(label_text);
    progress_dialog->setRange(0, 0);
    progress_dialog->setCancelButton(nullptr);
    progress_dialog->setWindowModality(Qt::ApplicationModal);
    GUIUtil::PolishProgressDialog(progress_dialog);
    // The setValue call forces QProgressDialog to start the internal duration estimation.
    // See details in https://bugreports.qt.io/browse/QTBUG-47042.
    progress_dialog->setValue(0);
    // When requested, launch dialog minimized
    if (show_minimized) progress_dialog->showMinimized();
}

CreateWalletActivity::CreateWalletActivity(WalletController* wallet_controller, QWidget* parent_widget)
    : WalletControllerActivity(wallet_controller, parent_widget)
{
    m_passphrase.reserve(MAX_PASSPHRASE_SIZE);
}

CreateWalletActivity::~CreateWalletActivity()
{
    delete m_create_wallet_dialog;
    delete m_passphrase_dialog;
}

void CreateWalletActivity::askPassphrase()
{
    if (m_wallet_controller->isStopping()) { Q_EMIT finished(); return; }
    m_passphrase_dialog = new AskPassphraseDialog(AskPassphraseDialog::Encrypt, m_parent_widget, &m_passphrase);
    m_passphrase_dialog->setWindowModality(Qt::ApplicationModal);
    m_passphrase_dialog->show();

    connect(m_passphrase_dialog, &QObject::destroyed, [this] {
        m_passphrase_dialog = nullptr;
    });
    connect(m_passphrase_dialog, &QDialog::accepted, [this] {
        createWallet();
    });
    connect(m_passphrase_dialog, &QDialog::rejected, [this] {
        Q_EMIT finished();
    });
}

void CreateWalletActivity::createWallet()
{
    if (m_wallet_controller->isStopping()) { Q_EMIT finished(); return; }
    showProgressDialog(
        //: Title of window indicating the progress of creation of a new wallet.
        tr("Create Wallet"),
        /*: Descriptive text of the create wallet progress window which indicates
            to the user which wallet is currently being created. */
        tr("Creating Wallet <b>%1</b>…").arg(m_create_wallet_dialog->walletName().toHtmlEscaped()));

    std::string name = m_create_wallet_dialog->walletName().toStdString();
    uint64_t flags = 0;
    // Enable descriptors by default.
    flags |= WALLET_FLAG_DESCRIPTORS;
    if (m_create_wallet_dialog->isDisablePrivateKeysChecked()) {
        flags |= WALLET_FLAG_DISABLE_PRIVATE_KEYS;
    }
    if (m_create_wallet_dialog->isMakeBlankWalletChecked()) {
        flags |= WALLET_FLAG_BLANK_WALLET;
    }
    if (m_create_wallet_dialog->isExternalSignerChecked()) {
        flags |= WALLET_FLAG_EXTERNAL_SIGNER;
    }

    QTimer::singleShot(500ms, worker(), [this, name, flags] {
        auto wallet{node().walletLoader().createWallet(name, m_passphrase, flags, m_warning_message)};

        if (wallet) {
            m_wallet_model = m_wallet_controller->getOrCreateWallet(std::move(*wallet));
        } else {
            m_error_message = util::ErrorString(wallet);
        }

        QTimer::singleShot(500ms, this, &CreateWalletActivity::finish);
    });
}

void CreateWalletActivity::finish()
{
    if (m_wallet_controller->isStopping()) { Q_EMIT finished(); return; }
    if (!m_error_message.empty()) {
        QMessageBox::critical(m_parent_widget, tr("Create wallet failed"), QString::fromStdString(m_error_message.translated));
    } else if (!m_warning_message.empty()) {
        QMessageBox::warning(m_parent_widget, tr("Create wallet warning"), QString::fromStdString(Join(m_warning_message, Untranslated("\n")).translated));
    }

    if (m_wallet_model) Q_EMIT created(m_wallet_model);

    Q_EMIT finished();
}

void CreateWalletActivity::create()
{
    if (m_wallet_controller->isStopping()) { Q_EMIT finished(); return; }
    GUIUtil::BackendOperationGuard operation;
    m_create_wallet_dialog = new CreateWalletDialog(m_parent_widget);

    std::vector<std::unique_ptr<interfaces::ExternalSigner>> signers;
    try {
        signers = GUIUtil::WaitForBackendTask(clientModel().requestNodeData([](interfaces::Node& node) {
            return node.listExternalSigners();
        }), m_parent_widget);
    } catch (const std::runtime_error& e) {
        QMessageBox::critical(nullptr, tr("Can't list signers"), e.what());
    }
    if (signers.size() > 1) {
        QMessageBox::critical(nullptr, tr("Too many external signers found"), QString::fromStdString("More than one external signer found. Please connect only one at a time."));
        signers.clear();
    }
    m_create_wallet_dialog->setSigners(signers);

    m_create_wallet_dialog->setWindowModality(Qt::ApplicationModal);
    m_create_wallet_dialog->show();

    connect(m_create_wallet_dialog, &QObject::destroyed, [this] {
        m_create_wallet_dialog = nullptr;
    });
    connect(m_create_wallet_dialog, &QDialog::rejected, [this] {
        Q_EMIT finished();
    });
    connect(m_create_wallet_dialog, &QDialog::accepted, [this] {
        if (m_create_wallet_dialog->isEncryptWalletChecked()) {
            askPassphrase();
        } else {
            createWallet();
        }
    });
}

OpenWalletActivity::OpenWalletActivity(WalletController* wallet_controller, QWidget* parent_widget)
    : WalletControllerActivity(wallet_controller, parent_widget)
{
}

void OpenWalletActivity::finish()
{
    if (m_wallet_controller->isStopping()) { Q_EMIT finished(); return; }
    if (!m_error_message.empty()) {
        QMessageBox::critical(m_parent_widget, tr("Open wallet failed"), QString::fromStdString(m_error_message.translated));
    } else if (!m_warning_message.empty()) {
        QMessageBox::warning(m_parent_widget, tr("Open wallet warning"), QString::fromStdString(Join(m_warning_message, Untranslated("\n")).translated));
    }

    if (m_wallet_model) Q_EMIT opened(m_wallet_model);

    Q_EMIT finished();
}

void OpenWalletActivity::open(const std::string& path)
{
    if (m_wallet_controller->isStopping()) { Q_EMIT finished(); return; }
    QString name = GUIUtil::WalletDisplayName(path);

    showProgressDialog(
        //: Title of window indicating the progress of opening of a wallet.
        tr("Open Wallet"),
        /*: Descriptive text of the open wallet progress window which indicates
            to the user which wallet is currently being opened. */
        tr("Opening Wallet <b>%1</b>…").arg(name.toHtmlEscaped()));

    QTimer::singleShot(0, worker(), [this, path] {
        auto wallet{node().walletLoader().loadWallet(path, m_warning_message)};

        if (wallet) {
            m_wallet_model = m_wallet_controller->getOrCreateWallet(std::move(*wallet));
        } else {
            m_error_message = util::ErrorString(wallet);
        }

        QTimer::singleShot(0, this, &OpenWalletActivity::finish);
    });
}

LoadWalletsActivity::LoadWalletsActivity(WalletController* wallet_controller, QWidget* parent_widget)
    : WalletControllerActivity(wallet_controller, parent_widget)
{
}

void LoadWalletsActivity::load(bool show_loading_minimized)
{
    if (m_wallet_controller->isStopping()) { Q_EMIT finished(); return; }
    showProgressDialog(
        //: Title of progress window which is displayed when wallets are being loaded.
        tr("Load Wallets"),
        /*: Descriptive text of the load wallets progress window which indicates to
            the user that wallets are currently being loaded.*/
        tr("Loading wallets…"),
        /*show_minimized=*/show_loading_minimized);

    QTimer::singleShot(0, worker(), [this] {
        m_wallet_controller->subscribeToCoreSignals();
        for (auto& wallet : node().walletLoader().getWallets()) {
            m_wallet_controller->getOrCreateWallet(std::move(wallet));
        }

        QTimer::singleShot(0, this, [this] { Q_EMIT finished(); });
    });
}

RestoreWalletActivity::RestoreWalletActivity(WalletController* wallet_controller, QWidget* parent_widget)
    : WalletControllerActivity(wallet_controller, parent_widget)
{
}

void RestoreWalletActivity::restore(const fs::path& backup_file, const std::string& wallet_name)
{
    if (m_wallet_controller->isStopping()) { Q_EMIT finished(); return; }
    QString name = QString::fromStdString(wallet_name);

    showProgressDialog(
        //: Title of progress window which is displayed when wallets are being restored.
        tr("Restore Wallet"),
        /*: Descriptive text of the restore wallets progress window which indicates to
            the user that wallets are currently being restored.*/
        tr("Restoring Wallet <b>%1</b>…").arg(name.toHtmlEscaped()));

    QTimer::singleShot(0, worker(), [this, backup_file, wallet_name] {
        auto wallet{node().walletLoader().restoreWallet(backup_file, wallet_name, m_warning_message, /*load_after_restore=*/true)};

        if (wallet) {
            m_wallet_model = m_wallet_controller->getOrCreateWallet(std::move(*wallet));
        } else {
            m_error_message = util::ErrorString(wallet);
        }

        QTimer::singleShot(0, this, &RestoreWalletActivity::finish);
    });
}

void RestoreWalletActivity::finish()
{
    if (m_wallet_controller->isStopping()) { Q_EMIT finished(); return; }
    if (!m_error_message.empty()) {
        //: Title of message box which is displayed when the wallet could not be restored.
        QMessageBox::critical(m_parent_widget, tr("Restore wallet failed"), QString::fromStdString(m_error_message.translated));
    } else if (!m_warning_message.empty()) {
        //: Title of message box which is displayed when the wallet is restored with some warning.
        QMessageBox::warning(m_parent_widget, tr("Restore wallet warning"), QString::fromStdString(Join(m_warning_message, Untranslated("\n")).translated));
    } else {
        //: Title of message box which is displayed when the wallet is successfully restored.
        QMessageBox::information(m_parent_widget, tr("Restore wallet message"), QString::fromStdString(Untranslated("Wallet restored successfully \n").translated));
    }

    if (m_wallet_model) Q_EMIT restored(m_wallet_model);

    Q_EMIT finished();
}

void MigrateWalletActivity::do_migrate(const std::string& name, bool load_wallet)
{
    if (m_wallet_controller->isStopping()) { Q_EMIT finished(); return; }
    GUIUtil::BackendOperationGuard operation;
    SecureString passphrase;
    const bool encrypted = GUIUtil::WaitForBackendTask(clientModel().requestNodeData(
        [name](interfaces::Node& node) { return node.walletLoader().isEncrypted(name); }), m_parent_widget);
    if (encrypted) {
        // Get the passphrase for the wallet
        AskPassphraseDialog dlg(AskPassphraseDialog::UnlockMigration, m_parent_widget, &passphrase);
        if (dlg.exec() == QDialog::Rejected) {
            Q_EMIT finished();
            return;
        }
    }

    showProgressDialog(tr("Migrate Wallet"), tr("Migrating Wallet <b>%1</b>…").arg(GUIUtil::HtmlEscape(name)));

    QTimer::singleShot(0, worker(), [this, name, passphrase, load_wallet] {
        auto res{node().walletLoader().migrateWallet(name, passphrase, load_wallet)};

        if (res) {
            m_success_message = tr("The wallet '%1' was migrated successfully.").arg(GUIUtil::HtmlEscape(GUIUtil::WalletDisplayName(name)));
            if (res->watchonly_wallet_name) {
                m_success_message += QChar(' ') + tr("Watchonly scripts have been migrated to a new wallet named '%1'.").arg(GUIUtil::HtmlEscape(GUIUtil::WalletDisplayName(res->watchonly_wallet_name.value())));
            }
            if (res->solvables_wallet_name) {
                m_success_message += QChar(' ') + tr("Solvable but not watched scripts have been migrated to a new wallet named '%1'.").arg(GUIUtil::HtmlEscape(GUIUtil::WalletDisplayName(res->solvables_wallet_name.value())));
            }
            if (load_wallet) {
                assert(res->wallet);
                m_wallet_model = m_wallet_controller->getOrCreateWallet(std::move(res->wallet));
            } else {
                m_success_message += QChar(' ') + tr("The wallet was not loaded after migration. You can open it from the \"File > Open wallet\" menu.");
            }
        } else {
            m_error_message = util::ErrorString(res);
        }

        QTimer::singleShot(0, this, &MigrateWalletActivity::finish);
    });
}

void MigrateWalletActivity::migrate(const std::string& name)
{
    if (m_wallet_controller->isStopping()) { Q_EMIT finished(); return; }
    GUIUtil::BackendOperationGuard operation;
    // Warn the user about migration
    QMessageBox box(m_parent_widget);
    box.setWindowTitle(tr("Migrate wallet"));
    box.setText(tr("Are you sure you wish to migrate the wallet <i>%1</i>?").arg(GUIUtil::HtmlEscape(GUIUtil::WalletDisplayName(name))));
    box.setInformativeText(tr("Migrating the wallet will convert this wallet to one or more descriptor wallets. A new wallet backup will need to be made.\n"
                "If this wallet contains any watchonly scripts, a new wallet will be created which contains those watchonly scripts.\n"
                "If this wallet contains any solvable but not watched scripts, a different and new wallet will be created which contains those scripts.\n\n"
                "The migration process will create a backup of the wallet before migrating. This backup file will be named "
                "<wallet name>-<timestamp>.legacy.bak and can be found in the directory for this wallet. In the event of "
                "an incorrect migration, the backup can be restored with the \"Restore Wallet\" functionality."));
    auto* load_wallet_checkbox = new QCheckBox(tr("Load wallet after migration"), &box);
    load_wallet_checkbox->setToolTip(tr("If the node is pruned and the wallet was created before the pruned height, the migration process may fail trying to load the migrated wallet."));
    load_wallet_checkbox->setChecked(true);
    box.setCheckBox(load_wallet_checkbox);
    box.setStandardButtons(QMessageBox::Yes|QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Yes);
    if (box.exec() != QMessageBox::Yes) return;

    do_migrate(name, load_wallet_checkbox->isChecked());
}

void MigrateWalletActivity::restore_and_migrate(const fs::path& path, const std::string& wallet_name)
{
    if (m_wallet_controller->isStopping()) { Q_EMIT finished(); return; }
    GUIUtil::BackendOperationGuard operation;
    // Warn the user about migration
    QMessageBox box(m_parent_widget);
    box.setWindowTitle(tr("Restore and Migrate wallet"));
    box.setText(tr("Are you sure you wish to restore the wallet file <i>%1</i> to <i>%2</i> and migrate it?").arg(GUIUtil::HtmlEscape(fs::PathToString(path)), GUIUtil::HtmlEscape(GUIUtil::WalletDisplayName(wallet_name))));
    box.setInformativeText(tr("Restoring the wallet will copy the backup file to the wallets directory and place it in the standard "
                "wallet directory layout. The original file will not be modified.\n\n"
                "Migrating the wallet will convert the restored wallet to one or more descriptor wallets. A new wallet backup will need to be made.\n"
                "If this wallet contains any watchonly scripts, a new wallet will be created which contains those watchonly scripts.\n"
                "If this wallet contains any solvable but not watched scripts, a different and new wallet will be created which contains those scripts.\n\n"
                "The migration process will create a backup of the wallet before migrating. This backup file will be named "
                "<wallet name>-<timestamp>.legacy.bak and can be found in the directory for this wallet. In the event of "
                "an incorrect migration, the backup can be restored with the \"Restore Wallet\" functionality."));
    box.setStandardButtons(QMessageBox::Yes|QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Yes);
    if (box.exec() != QMessageBox::Yes) return;

    showProgressDialog(
        //: Title of progress window which is displayed when wallets are being restored.
        tr("Restore Wallet"),
        /*: Descriptive text of the restore wallets progress window which indicates to
            the user that wallets are currently being restored.*/
        tr("Restoring Wallet <b>%1</b>…").arg(GUIUtil::HtmlEscape(GUIUtil::WalletDisplayName(wallet_name))));

    QTimer::singleShot(0, worker(), [this, path, wallet_name] {
        auto res{node().walletLoader().restoreWallet(path, wallet_name, m_warning_message, /*load_after_restore=*/false)};

        if (!res) {
            m_error_message = util::ErrorString(res);
            QTimer::singleShot(0, this, &MigrateWalletActivity::finish);
            return;
        }
        QTimer::singleShot(0, this, [this, wallet_name] {
            do_migrate(wallet_name, /*load_wallet=*/true);
        });
    });
}

void MigrateWalletActivity::finish()
{
    if (m_wallet_controller->isStopping()) { Q_EMIT finished(); return; }
    if (!m_error_message.empty()) {
        QMessageBox::critical(m_parent_widget, tr("Migration failed"), QString::fromStdString(m_error_message.translated));
    } else {
        QMessageBox::information(m_parent_widget, tr("Migration Successful"), m_success_message);
    }

    if (m_wallet_model) Q_EMIT migrated(m_wallet_model);

    Q_EMIT finished();
}
