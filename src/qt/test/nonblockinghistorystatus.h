// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGHISTORYSTATUS_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGHISTORYSTATUS_H

#include <interfaces/node.h>
#include <key_io.h>
#include <qt/guiutil.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/walletmodel.h>
#include <tinyformat.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <memory>
#include <utility>

#include <QSignalSpy>
#include <QPersistentModelIndex>
#include <QTableView>
#include <QTest>

inline void CheckHistoryStatusRoles(WalletModel& source, const PlatformStyle* style)
{
    const auto tip = GUIUtil::WaitForBackendTask(source.requestWalletData([](interfaces::Wallet& backend) {
        auto& wallet = *backend.wallet();
        LOCK(wallet.cs_wallet);
        return std::make_pair(wallet.GetLastBlockHeight(), wallet.GetLastBlockHash());
    }));
    QVERIFY(tip.first >= 10);
    auto wallet = std::make_shared<wallet::CWallet>(&source.wallet().wallet()->chain(),
        "history status roles", wallet::CreateMockableWalletDatabase());
    CKey key;
    key.MakeNewKey(true);
    QVERIFY(wallet::CreateDescriptor(*wallet, strprintf("rawtr(%s)", EncodeSecret(key)), true));
    constexpr int ROWS{32};
    Txid target;
    {
        LOCK(wallet->cs_wallet);
        wallet->SetLastBlockProcessed(tip.first, tip.second);
        for (int row{0}; row < ROWS; ++row) {
            CMutableTransaction transaction;
            transaction.vin.emplace_back(Txid::FromUint256(uint256::ONE), row);
            transaction.vout.emplace_back(COIN, XOnlyPubKey{key.GetPubKey()});
            QVERIFY(wallet->AddToWallet(MakeTransactionRef(transaction), wallet::TxStateConfirmed{tip.second, tip.first - 10, row}));
            target = transaction.GetHash();
        }
    }
    // Use the asynchronous-drain-aware deleter even when a QVERIFY exits the
    // helper early. Proxies/observers below are destroyed before their source.
    const auto destroy_model = [](WalletModel* model) { WalletModel::destroy(model); };
    std::unique_ptr<WalletModel, decltype(destroy_model)> model{
        new WalletModel(interfaces::MakeWallet(*source.node().walletLoader().context(), wallet),
                        source.clientModel(), style), destroy_model};
    auto& history = *model->getTransactionTableModel();
    class CountingProxy final : public TransactionFilterProxy {
    public:
        mutable int comparisons{0};
    protected:
        bool lessThan(const QModelIndex& left, const QModelIndex& right) const override
        {
            ++comparisons;
            return TransactionFilterProxy::lessThan(left, right);
        }
    } sorted;
    sorted.setSourceModel(&history);
    sorted.setDynamicSortFilter(true);
    sorted.setSortRole(Qt::EditRole);
    sorted.setShowInactive(false);
    sorted.sort(TransactionTableModel::Date, Qt::DescendingOrder);
    QTRY_COMPARE(sorted.rowCount(), ROWS);
    const auto all_confirmed = [&] {
        for (int row{0}; row < history.rowCount({}); ++row) {
            if (history.index(row, 0).data(TransactionTableModel::StatusRole).toInt() != TransactionStatus::Confirmed) return false;
        }
        return true;
    };
    QTRY_VERIFY(all_confirmed());
    QVERIFY(sorted.comparisons > 0);
    sorted.comparisons = 0;
    QSignalSpy changes{&history, &QAbstractItemModel::dataChanged};
    QSignalSpy confirmation_repaints{&history, &TransactionTableModel::confirmationsChanged};
    const auto old_tooltip = history.indexesForTransaction(target).front().data(Qt::ToolTipRole).toString();

    // Advancing only depth changes tooltip/icon information, not the immutable
    // date/status sort key. It must not wake the proxy's comparison machinery.
    GUIUtil::WaitForBackendTask(model->requestWalletData([tip, target](interfaces::Wallet& backend) {
        auto& wallet = *backend.wallet();
        LOCK(wallet.cs_wallet);
        wallet.SetLastBlockProcessed(tip.first + 1, tip.second);
        wallet.NotifyTransactionChanged(target, CT_UPDATED);
    }));
    QTRY_VERIFY(history.indexesForTransaction(target).front().data(Qt::ToolTipRole).toString() != old_tooltip);
    QCOMPARE(changes.size(), 0);
    QCOMPARE(confirmation_repaints.size(), 1);
    QCOMPARE(sorted.comparisons, 0);

    // A real state/sort-key change must retain filtering semantics, including
    // bringing a hidden conflict back when the wallet reports its recovery.
    changes.clear();
    GUIUtil::WaitForBackendTask(model->requestWalletData([tip, target](interfaces::Wallet& backend) {
        auto& wallet = *backend.wallet();
        LOCK(wallet.cs_wallet);
        wallet.mapWallet.at(target).m_state = wallet::TxStateBlockConflicted{tip.second, tip.first};
        wallet.NotifyTransactionChanged(target, CT_UPDATED);
    }));
    QTRY_COMPARE(sorted.rowCount(), ROWS - 1);
    QVERIFY(!changes.empty());
    auto transition_roles = changes.front().at(2).value<QList<int>>();
    QVERIFY(transition_roles.contains(Qt::EditRole));
    QVERIFY(transition_roles.contains(Qt::DisplayRole));
    QVERIFY(transition_roles.contains(TransactionTableModel::StatusRole));
    changes.clear();
    GUIUtil::WaitForBackendTask(model->requestWalletData([tip, target](interfaces::Wallet& backend) {
        auto& wallet = *backend.wallet();
        LOCK(wallet.cs_wallet);
        wallet.mapWallet.at(target).m_state = wallet::TxStateConfirmed{tip.second, tip.first - 10, 0};
        wallet.NotifyTransactionChanged(target, CT_UPDATED);
    }));
    QTRY_COMPARE(sorted.rowCount(), ROWS);
    QVERIFY(!changes.empty());
    QVERIFY(changes.front().at(2).value<QList<int>>().contains(Qt::EditRole));
    sorted.setSourceModel(nullptr);
}

inline void CheckBoundedHistoryOutputStatuses(WalletModel& source, const PlatformStyle* style)
{
    const auto tip = GUIUtil::WaitForBackendTask(source.requestWalletData([](interfaces::Wallet& backend) {
        auto& wallet = *backend.wallet();
        LOCK(wallet.cs_wallet);
        return std::make_pair(wallet.GetLastBlockHeight(), wallet.GetLastBlockHash());
    }));
    QVERIFY(tip.first >= 10);
    auto wallet = std::make_shared<wallet::CWallet>(&source.wallet().wallet()->chain(),
        "history output status bounds", wallet::CreateMockableWalletDatabase());
    CKey key;
    key.MakeNewKey(true);
    QVERIFY(wallet::CreateDescriptor(*wallet, strprintf("rawtr(%s)", EncodeSecret(key)), true));
    constexpr int OUTPUTS{320};
    CMutableTransaction transaction;
    transaction.vin.emplace_back(Txid::FromUint256(uint256::ONE), 0);
    for (int output{0}; output < OUTPUTS; ++output) transaction.vout.emplace_back(COIN + output, XOnlyPubKey{key.GetPubKey()});
    const auto hash = transaction.GetHash();
    const auto hash_text = QString::fromStdString(hash.GetHex());
    {
        LOCK(wallet->cs_wallet);
        wallet->SetLastBlockProcessed(tip.first, tip.second);
        QVERIFY(wallet->AddToWallet(MakeTransactionRef(transaction), wallet::TxStateConfirmed{tip.second, tip.first - 10, 0}));
    }
    const auto destroy_model = [](WalletModel* model) { WalletModel::destroy(model); };
    std::unique_ptr<WalletModel, decltype(destroy_model)> model{
        new WalletModel(interfaces::MakeWallet(*source.node().walletLoader().context(), wallet),
                        source.clientModel(), style), destroy_model};
    auto& history = *model->getTransactionTableModel();
    QTRY_COMPARE(history.rowCount({}), OUTPUTS);
    QTRY_COMPARE(model->getLastBlockProcessed(), tip.second);
    const auto rows_with_status = [&](TransactionStatus::Status status) {
        int count{0};
        for (const auto& item : history.indexesForTransaction(hash)) {
            if (item.data(TransactionTableModel::StatusRole).toInt() == status) ++count;
        }
        return count;
    };
    // Trigger initial status loading without painting or sorting all rows.
    (void)history.index(0, 0);
    QTRY_COMPARE(rows_with_status(TransactionStatus::Confirmed), OUTPUTS);

    QObject observer;
    int notified_rows{0};
    int first_turn_rows{-1};
    bool bounded{true};
    bool interleave_update{false};
    bool interleaved{false};
    QObject::connect(&history, &QAbstractItemModel::dataChanged, &observer,
        [&](const QModelIndex& first, const QModelIndex& last, const QList<int>& roles) {
            if (!roles.contains(TransactionTableModel::StatusRole)) return;
            const int count = last.row() - first.row() + 1;
            bounded &= count <= 128;
            notified_rows += count;
            if (first_turn_rows != -1) return;
            first_turn_rows = -2;
            QMetaObject::invokeMethod(&observer, [&] {
                first_turn_rows = notified_rows;
                if (!interleave_update) return;
                // A newer notification between chunks invalidates the older
                // snapshot and starts the newer status from its first output.
                {
                    LOCK(wallet->cs_wallet);
                    wallet->mapWallet.at(hash).m_state = wallet::TxStateConfirmed{tip.second, tip.first - 10, 0};
                }
                history.updateTransaction(hash_text, CT_UPDATED, true);
                interleaved = true;
            }, Qt::QueuedConnection);
        });
    GUIUtil::WaitForBackendTask(model->requestWalletData([tip, hash](interfaces::Wallet& backend) {
        auto& wallet = *backend.wallet();
        LOCK(wallet.cs_wallet);
        wallet.mapWallet.at(hash).m_state = wallet::TxStateBlockConflicted{tip.second, tip.first};
        wallet.NotifyTransactionChanged(hash, CT_UPDATED);
    }));
    QTRY_COMPARE(rows_with_status(TransactionStatus::Conflicted), OUTPUTS);
    QCOMPARE(first_turn_rows, 128);
    QCOMPARE(notified_rows, OUTPUTS);
    QVERIFY(bounded);

    GUIUtil::WaitForBackendTask(model->requestWalletData([tip, hash](interfaces::Wallet& backend) {
        auto& wallet = *backend.wallet();
        LOCK(wallet.cs_wallet);
        wallet.mapWallet.at(hash).m_state = wallet::TxStateConfirmed{tip.second, tip.first - 10, 0};
        wallet.NotifyTransactionChanged(hash, CT_UPDATED);
    }));
    QTRY_COMPARE(rows_with_status(TransactionStatus::Confirmed), OUTPUTS);
    QVERIFY(bounded);

    notified_rows = 0;
    first_turn_rows = -1;
    interleave_update = true;
    GUIUtil::WaitForBackendTask(model->requestWalletData([tip, hash](interfaces::Wallet& backend) {
        auto& wallet = *backend.wallet();
        LOCK(wallet.cs_wallet);
        wallet.mapWallet.at(hash).m_state = wallet::TxStateBlockConflicted{tip.second, tip.first};
        wallet.NotifyTransactionChanged(hash, CT_UPDATED);
    }));
    QTRY_VERIFY(interleaved);
    QTRY_COMPARE(rows_with_status(TransactionStatus::Confirmed), OUTPUTS);
    QCOMPARE(first_turn_rows, 128);
    QCOMPARE(notified_rows, 256); // 128 stale conflict rows, then their recovery.
    QVERIFY(bounded);

    // A retained index must resolve the live row, not a pointer into QList
    // storage. Growing the history may move that storage without moving the
    // retained row (when the new transaction sorts after it).
    const QPersistentModelIndex retained{history.index(OUTPUTS / 2, 0)};
    const auto retained_amount = retained.data(TransactionTableModel::AmountRole);
    QVERIFY(retained.internalPointer() == nullptr);
    CMutableTransaction extra;
    extra.vin.emplace_back(Txid::FromUint256(uint256::ONE), 1);
    for (int output{0}; output < OUTPUTS * 2; ++output) extra.vout.emplace_back(COIN + output / 2, XOnlyPubKey{key.GetPubKey()});
    GUIUtil::WaitForBackendTask(model->requestWalletData([extra](interfaces::Wallet& backend) {
        auto& wallet = *backend.wallet();
        LOCK(wallet.cs_wallet);
        wallet.AddToWallet(MakeTransactionRef(extra), wallet::TxStateInactive{});
    }));
    QTRY_COMPARE(history.rowCount({}), OUTPUTS * 3);
    QVERIFY(retained.isValid());
    QCOMPARE(retained.data(TransactionTableModel::TxHashRole).toString(), hash_text);
    QCOMPARE(retained.data(TransactionTableModel::AmountRole), retained_amount);

    // Transaction focus must not emit one selection change per output. Amount
    // sorting interleaves another transaction's outputs, so selecting one large
    // bounding range would incorrectly include its rows. Both sort directions
    // and partial filtering must preserve precisely the visible target rows.
    {
        TransactionView view{style};
        view.setModel(model.get());
        auto* table = view.findChild<QTableView*>("transactionView");
        QVERIFY(table);
        auto* proxy = qobject_cast<TransactionFilterProxy*>(table->model());
        QVERIFY(proxy);
        QSignalSpy selections{table->selectionModel(), &QItemSelectionModel::selectionChanged};
        for (const auto order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
            table->sortByColumn(TransactionTableModel::Amount, order);
            for (const int minimum_output : {0, OUTPUTS / 2}) {
                proxy->setMinAmount(COIN + minimum_output);
                table->selectionModel()->clearSelection();
                selections.clear();
                QItemSelectionModel reference{proxy};
                for (const auto& item : history.indexesForTransaction(hash)) {
                    reference.select(proxy->mapFromSource(item),
                        QItemSelectionModel::Rows | QItemSelectionModel::Select);
                }
                view.focusTransaction(hash);
                QCOMPARE(selections.size(), 1);
                const auto selected = table->selectionModel()->selectedRows();
                QCOMPARE(selected.size(), OUTPUTS - minimum_output);
                QCOMPARE(selected, reference.selectedRows());
                QCOMPARE(selected.front().data(TransactionTableModel::AmountRole).toLongLong(), COIN + minimum_output);
                QCOMPARE(GUIUtil::firstSelectedRow(table), reference.selectedRows().front());
                for (const auto& item : selected) {
                    QCOMPARE(item.data(TransactionTableModel::TxHashRole).toString(), hash_text);
                }
            }
        }
        proxy->setMinAmount(0);
        proxy->setSearchString(QString::fromStdString(extra.GetHash().GetHex()));
        view.focusTransaction(extra.GetHash());
        QVERIFY(table->selectionModel()->hasSelection());
        selections.clear();
        view.focusTransaction(hash); // All target outputs are now filtered out.
        QVERIFY(!table->selectionModel()->hasSelection());
        QCOMPARE(selections.size(), 1);
        proxy->setSearchString({});
        view.focusTransaction(hash);
        QVERIFY(table->selectionModel()->hasSelection());
        view.focusTransaction(Txid{}); // An absent transaction clears selection too.
        QVERIFY(!table->selectionModel()->hasSelection());
    }

    history.updateTransaction(QString::fromStdString(extra.GetHash().GetHex()), CT_DELETED, true);
    QCOMPARE(history.rowCount({}), OUTPUTS);
    QVERIFY(retained.isValid());
    QCOMPARE(retained.data(TransactionTableModel::TxHashRole).toString(), hash_text);
    QCOMPARE(retained.data(TransactionTableModel::AmountRole), retained_amount);
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGHISTORYSTATUS_H
