// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGWALLETMODEL_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGWALLETMODEL_H

#include <qt/bitcoinunits.h>
#include <qt/addresstablemodel.h>
#include <qt/coincontroldialog.h>
#include <qt/csvmodelwriter.h>
#include <qt/optionsmodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionfilterproxy.h>
#include <qt/walletmodel.h>
#include <interfaces/node.h>
#include <wallet/coincontrol.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include <QApplication>
#include <QLabel>
#include <QPointer>
#include <QFile>
#include <QScopedValueRollback>
#include <QScopeGuard>
#include <QTest>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>

// Use the existing funded GUI fixture. Backend locks must not stall model
// construction, coin-control drawing, or destruction with unfinished queries.
inline void CheckNonblockingCoinControlAndStartup(WalletModel& model, wallet::CWallet& wallet, const PlatformStyle* style)
{
    std::vector<COutPoint> outputs;
    CAmount selected_amount{0};
    for (const auto& [destination, coins] : model.wallet().listCoins()) {
        for (const auto& [outpoint, coin] : coins) {
            if (outputs.size() == 2) break;
            outputs.push_back(outpoint);
            selected_amount += coin.txout.nValue;
        }
        if (outputs.size() == 2) break;
    }
    QCOMPARE(outputs.size(), size_t{2});
    wallet::CCoinControl control;
    control.Select(outputs[0]);
    QScopedValueRollback<QList<CAmount>> amounts{CoinControlDialog::payAmounts, {1 * COIN}};
    QScopedValueRollback<bool> subtract{CoinControlDialog::fSubtractFeeFromAmount, false};
    auto owned_sender = std::make_unique<SendCoinsDialog>(style);
    owned_sender->setModel(&model);

    std::promise<void> locked;
    auto ready{locked.get_future()};
    std::promise<void> release;
    auto released{release.get_future()};
    auto blocker = std::async(std::launch::async, [&] {
        LOCK(wallet.cs_wallet);
        locked.set_value();
        return released.wait_for(std::chrono::seconds{3}) == std::future_status::timeout;
    });
    ready.wait();
    int heartbeats{0};
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
    heartbeat.start(10);

    auto coins = std::make_unique<CoinControlDialog>(control, &model, style);
    coins->show();
    const bool rendered{!coins->grab().isNull()};
    // Supersede the pending calculation without queuing unbounded work. Its
    // result must use two selected inputs, never overwrite them with one.
    control.Select(outputs[1]);
    CoinControlDialog::payAmounts = {2 * COIN};
    const bool invoked{QMetaObject::invokeMethod(coins.get(), "coinControlUpdateLabels", Qt::DirectConnection)};
    auto initial = std::make_unique<TransactionTableModel>(style, &model);
    auto disposable = std::make_unique<CoinControlDialog>(control, &model, style);
    const bool owned_dialog_opened = QMetaObject::invokeMethod(owned_sender.get(), "coinControlButtonClicked", Qt::DirectConnection);
    QPointer<CoinControlDialog> owned_coins = owned_sender->findChild<CoinControlDialog*>();
    if (!owned_coins) owned_coins = qobject_cast<CoinControlDialog*>(QApplication::activeModalWidget());
    const bool owned_dialog_parented = owned_coins && owned_coins->parentWidget() == owned_sender.get();
    // Count delivered event-loop turns, not elapsed wall time: timer overruns
    // are coalesced when a CI runner is briefly descheduled.
    const bool responsive = QTest::qWaitFor([&] { return heartbeats >= 3; }, 1000);
    const int rows_while_locked{initial->rowCount({})};
    initial.reset();
    disposable.reset();
    // Wallet-view shutdown destroys the sending page while its coin query is
    // still pending. The child dialog and timers must not outlive that page's
    // CCoinControl reference or defer their preferences snapshot past flush.
    owned_sender.reset();
    const bool owned_dialog_destroyed = owned_coins.isNull();
    if (owned_coins) delete owned_coins.data(); // Keep a failing regression isolated.
    release.set_value();
    const bool lock_timed_out{blocker.get()};

    QVERIFY(!lock_timed_out);
    QVERIFY(invoked);
    QVERIFY(owned_dialog_opened);
    QVERIFY(owned_dialog_parented);
    QVERIFY(owned_dialog_destroyed);
    QVERIFY(rendered);
    QVERIFY(responsive);
    QCOMPARE(rows_while_locked, 0);
    auto* quantity = coins->findChild<QLabel*>("labelCoinControlQuantity");
    auto* amount = coins->findChild<QLabel*>("labelCoinControlAmount");
    QVERIFY(quantity);
    QVERIFY(amount);
    QTRY_COMPARE(quantity->text(), QString{"2"});
    QTRY_COMPARE(amount->text(), BitcoinUnits::formatWithUnit(model.getOptionsModel()->getDisplayUnit(), selected_amount));
    auto* tree = coins->findChild<QTreeWidget*>("treeWidget");
    QVERIFY(tree);
    QTRY_VERIFY(tree->topLevelItemCount() > 0);
    coins.reset();
    auto drained = model.requestWalletData([](interfaces::Wallet&) { return true; });
    QTRY_VERIFY(drained.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
    QVERIFY(drained.get());
}

// Label searches must react to address cache changes without a new block or
// transaction-status notification. No backend mutation is needed for this.
inline void CheckTransactionLabelInvalidation(WalletModel& model)
{
    auto* addresses = model.getAddressTableModel();
    auto& history = *model.getTransactionTableModel();
    QTRY_VERIFY(history.rowCount({}) > 0);
    QString address;
    for (int row{0}; row < history.rowCount({}); ++row) {
        address = history.index(row, 0).data(TransactionTableModel::AddressRole).toString();
        if (!address.isEmpty()) break;
    }
    QVERIFY(!address.isEmpty());
    const auto old_label = addresses->labelForAddress(address);
    const auto old_purpose = addresses->purposeForAddress(address);
    const auto purpose = old_purpose.value_or(wallet::AddressPurpose::RECEIVE);
    const auto restore = qScopeGuard([&] {
        addresses->updateEntry(address, old_label, true, purpose, old_purpose ? CT_UPDATED : CT_DELETED);
    });
    TransactionFilterProxy filter;
    filter.setSourceModel(&history);
    filter.setDynamicSortFilter(true);
    const QString label{"qt-label-cache-invalidation-test"};
    filter.setSearchString(label);
    QCOMPARE(filter.rowCount({}), 0);
    bool label_role_changed{false};
    QObject::connect(&history, &QAbstractItemModel::dataChanged, &filter,
        [&](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
            if (roles.contains(TransactionTableModel::LabelRole)) label_role_changed = true;
        });
    addresses->updateEntry(address, label, true, purpose, CT_UPDATED);
    QTRY_VERIFY(label_role_changed);
    QTRY_VERIFY(filter.rowCount({}) > 0);
    QCOMPARE(addresses->labelForAddress(address), label);
    // Both a renamed cache entry and a deletion must invalidate active filters.
    addresses->updateEntry(address, old_label, true, purpose, CT_UPDATED);
    QTRY_COMPARE(filter.rowCount({}), 0);
    addresses->updateEntry(address, label, true, purpose, CT_UPDATED);
    QTRY_VERIFY(filter.rowCount({}) > 0);
    addresses->updateEntry(address, {}, true, purpose, CT_DELETED);
    QTRY_COMPARE(filter.rowCount({}), 0);
}

// Core transaction/progress notifications are drained in bounded batches.
// New live events must not overtake buffered rescan deletes for the same txid.
inline void CheckTransactionNotificationOrdering(WalletModel& model, wallet::CWallet& wallet, const PlatformStyle* style)
{
    TransactionTableModel history{style, &model};
    QTRY_VERIFY(history.rowCount({}) > 0);
    const int initial_rows = history.rowCount({});
    const auto hash_text = history.index(0, 0).data(TransactionTableModel::TxHashRole).toString();
    const auto hash = Txid::FromHex(hash_text.toStdString()).value();
    history.updateTransaction(hash_text, CT_DELETED, true);
    QVERIFY(history.rowCount({}) < initial_rows);

    auto producer = std::async(std::launch::async, [&wallet, hash] {
        wallet.ShowProgress("test rescan", 0);
        wallet.NotifyTransactionChanged(hash, CT_NEW);
        for (int i{0}; i < 128; ++i) wallet.NotifyTransactionChanged(hash, CT_UPDATED);
        wallet.NotifyTransactionChanged(hash, CT_DELETED);
        wallet.ShowProgress("test rescan", 100);
        wallet.NotifyTransactionChanged(hash, CT_NEW);
    });
    producer.get();
    QTRY_COMPARE(history.rowCount({}), initial_rows);
    // A later buffered delete must not undo the final live re-add.
    QTest::qWait(200);
    QCOMPARE(history.rowCount({}), initial_rows);
    QVERIFY(!history.processingQueuedTransactions());
}

// Disconnect must close the shared callback gate before the model can be
// deleted. Core signal disconnection alone does not join entered callbacks.
inline void CheckWalletNotificationLifetime(WalletModel& source, const PlatformStyle* style)
{
    auto wallet = std::make_shared<wallet::CWallet>(&source.wallet().wallet()->chain(),
        "notification lifetime fixture", wallet::CreateMockableWalletDatabase());
    wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
    const auto notify = [wallet] {
        wallet->NotifyStatusChanged(wallet.get());
        wallet->NotifyCanGetAddressesChanged();
        wallet->NotifyTransactionChanged(Txid{}, CT_UPDATED);
        wallet->NotifyAddressBookChanged(PKHash{}, "notification", false, wallet::AddressPurpose::SEND, CT_UPDATED);
        wallet->ShowProgress("notification lifetime", 100);
        wallet->NotifyUnload();
    };
    for (int round{0}; round < 8; ++round) {
        auto model = std::make_unique<WalletModel>(interfaces::MakeWallet(*source.node().walletLoader().context(), wallet),
            source.clientModel(), style);
        int delivered{0};
        QObject::connect(model.get(), &WalletModel::unload, model.get(), [&] { ++delivered; });
        QObject::connect(model.get(), &WalletModel::showProgress, model.get(), [&] { ++delivered; });
        notify();
        // Even same-thread unload must be queued, avoiding gate reentrancy.
        QCOMPARE(delivered, 0);
        std::promise<void> started;
        auto ready = started.get_future();
        std::thread producer([&] {
            started.set_value();
            for (int i{0}; i < 512; ++i) notify();
        });
        ready.wait();
        // No event processing precedes quiescence. Pending notifications and
        // those racing teardown must not be delivered by its responsive wait.
        WalletModel::destroy(model.release());
        producer.join();
        notify();
        QCoreApplication::processEvents();
        QCOMPARE(delivered, 0);
    }
}

inline void CheckBoundedCSVSnapshot()
{
    class SnapshotModel final : public QAbstractTableModel {
    public:
        int reads{0}, generation{0};
        bool changed_once{false}, unstable{false}, gui_only{true};
        int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 4096; }
        int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 1; }
        QVariant data(const QModelIndex& item, int role) const override
        {
            if (!item.isValid() || role != Qt::EditRole) return {};
            auto& self = *const_cast<SnapshotModel*>(this);
            ++self.reads;
            self.gui_only &= QThread::currentThread() == thread();
            if ((unstable && item.row() == 0) || (!changed_once && self.reads == 64)) {
                self.changed_once = true;
                ++self.generation;
                Q_EMIT self.dataChanged(index(0, 0), index(rowCount() - 1, 0));
            }
            return QString("%1:%2").arg(generation).arg(item.row());
        }
    } model;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("snapshot.csv");
    CSVModelWriter writer{path};
    writer.setModel(&model);
    writer.addColumn("value", 0);
    bool yielded_during_capture{false};
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        if (model.reads > 0 && model.reads < model.rowCount()) yielded_during_capture = true;
    });
    heartbeat.start(0);
    const bool written = writer.write();
    heartbeat.stop();
    QVERIFY(written);
    QVERIFY(model.gui_only);
    QVERIFY(yielded_during_capture);
    QVERIFY(model.reads > model.rowCount()); // The changed first attempt was retried.
    QFile file{path};
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto contents = file.readAll();
    file.close();
    QVERIFY(contents.startsWith("\"value\"\n"));
    for (int row{0}; row < model.rowCount(); ++row) {
        QVERIFY(contents.contains(QString("\"1:%1\"\n").arg(row).toUtf8()));
    }
    QVERIFY(!contents.contains("\"0:"));

    // Constant mutation exhausts the bounded retries without truncating an
    // existing destination or spinning forever in the GUI snapshot phase.
    model.unstable = true;
    const int before = model.reads;
    QVERIFY(!writer.write());
    QVERIFY(model.reads - before <= 4 * 128);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(file.readAll(), contents);
}

inline void CheckExplicitTransactionRequests(WalletModel& model, wallet::CWallet& wallet)
{
    auto& history = *model.getTransactionTableModel();
    QTRY_VERIFY(history.rowCount({}) > 0);
    const int initial_rows = history.rowCount({});
    const auto selected = history.index(0, 0);
    const auto hash = selected.data(TransactionTableModel::TxHashRole).toString();
    std::promise<void> locked, release;
    auto ready = locked.get_future();
    auto released = release.get_future();
    auto holder = std::async(std::launch::async, [&] {
        LOCK(wallet.cs_wallet);
        locked.set_value();
        return released.wait_for(std::chrono::seconds{3}) == std::future_status::timeout;
    });
    ready.wait();
    bool reentered{false};
    QObject callback_context;
    QTimer::singleShot(0, &callback_context, [&] { reentered = true; });
    for (int role{TransactionTableModel::TypeRole}; role <= TransactionTableModel::RawDecorationRole; ++role) {
        selected.data(role);
    }
    auto description = history.requestTxDescription(selected);
    auto hex = history.requestTxHex(selected);
    const bool description_waiting = description.wait_for(std::chrono::seconds{0}) != std::future_status::ready;
    const bool hex_waiting = hex.wait_for(std::chrono::seconds{0}) != std::future_status::ready;
    const bool data_reentered = reentered;
    // Invalidate the selected row before the copied worker requests execute.
    history.updateTransaction(hash, CT_DELETED, true);
    release.set_value();
    const bool timed_out = holder.get();
    QTRY_VERIFY(description.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
    QTRY_VERIFY(hex.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
    const auto text = description.get();
    const auto encoded = hex.get();
    history.updateTransaction(hash, CT_NEW, true);
    QTRY_COMPARE(history.rowCount({}), initial_rows);
    QVERIFY(!timed_out);
    QVERIFY(!data_reentered);
    QVERIFY(description_waiting);
    QVERIFY(hex_waiting);
    QVERIFY(text.contains(hash));
    QVERIFY(!encoded.isEmpty());
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGWALLETMODEL_H
