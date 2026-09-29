// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGHISTORYMODELS_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGHISTORYMODELS_H

#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <kernel/types.h>
#include <key_io.h>
#include <primitives/block.h>
#include <qt/guiutil.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionoverviewwidget.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>
#include <tinyformat.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <limits>
#include <memory>
#include <set>
#include <utility>

#include <QAbstractItemModelTester>
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTest>

inline void CheckBoundedOverviewModel()
{
    QStandardItemModel source{1000, TransactionTableModel::Amount + 1};
    for (int row{0}; row < source.rowCount(); ++row) source.setData(source.index(row, 0), row);
    QSortFilterProxyModel sorted;
    sorted.setSourceModel(&source);
    sorted.sort(0, Qt::DescendingOrder);
    TransactionOverviewModel recent;
    recent.setSourceModel(&sorted);
    QAbstractItemModelTester tester{&recent, QAbstractItemModelTester::FailureReportingMode::QtTest};
    const auto matches_source = [&] {
        if (recent.rowCount() != std::min(5, sorted.rowCount())) return false;
        for (int row{0}; row < recent.rowCount(); ++row) {
            if (recent.index(row, 0).data() != sorted.index(row, 0).data()) return false;
            if (recent.mapToSource(recent.index(row, 0)) != sorted.index(row, 0)) return false;
            if (recent.mapFromSource(sorted.index(row, 0)) != recent.index(row, 0)) return false;
        }
        return true;
    };
    QVERIFY(matches_source());
    QVERIFY(!recent.mapFromSource(sorted.index(5, 0)).isValid());
    QCOMPARE(recent.index(0, 0).data().toInt(), 999);
    source.setData(source.index(0, 0), 2000); // Cross the fifth boundary via layout change.
    QVERIFY(matches_source());
    QCOMPARE(recent.index(0, 0).data().toInt(), 2000);
    source.insertRow(0, new QStandardItem("3000"));
    QVERIFY(matches_source());
    source.removeRows(0, 2);
    QVERIFY(matches_source());
    sorted.sort(0, Qt::AscendingOrder);
    QVERIFY(matches_source());

    // Data changes outside the window must never reach the overview. Visible
    // changes retain their roles and indexes without resetting the model.
    QSignalSpy updates{&recent, &QAbstractItemModel::dataChanged};
    QSignalSpy resets{&recent, &QAbstractItemModel::modelReset};
    source.setData(sorted.mapToSource(sorted.index(10, 0)), "offscreen", Qt::ToolTipRole);
    QCOMPARE(updates.size(), 0);
    source.setData(sorted.mapToSource(sorted.index(2, 0)), "visible", Qt::ToolTipRole);
    QCOMPARE(updates.size(), 1);
    QCOMPARE(updates.front().at(0).value<QModelIndex>(), recent.index(2, 0));
    QCOMPARE(updates.front().at(1).value<QModelIndex>(), recent.index(2, 0));
    QCOMPARE(updates.front().at(2).value<QList<int>>(), QList<int>{Qt::ToolTipRole});
    QCOMPARE(resets.size(), 0);

    class CountingDelegate final : public QStyledItemDelegate {
    public:
        mutable std::set<int> measured_rows;
        QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& item) const override
        {
            measured_rows.insert(item.row());
            return {100, 54};
        }
    } delegate;
    TransactionOverviewWidget view;
    view.setModel(&recent);
    view.setModelColumn(TransactionTableModel::ToAddress);
    view.setItemDelegate(&delegate);
    (void)view.sizeHint();
    QCOMPARE(delegate.measured_rows.size(), size_t{5});
    QCOMPARE(*delegate.measured_rows.rbegin(), 4);

    source.removeRows(0, source.rowCount() - 3);
    QCOMPARE(recent.rowCount(), 3);
    QVERIFY(matches_source());
    source.clear();
    QCOMPARE(recent.rowCount(), 0);
    auto replacement = std::make_unique<QStandardItemModel>(2, 1);
    recent.setSourceModel(replacement.get());
    QCOMPARE(recent.rowCount(), 2);
    const QPersistentModelIndex previous_source_index{recent.index(0, 0)};
    resets.clear();
    replacement.reset(); // Source destruction must leave no dangling indexes.
    QCOMPARE(recent.rowCount(), 0);
    QVERIFY(!previous_source_index.isValid());
    QCOMPARE(resets.size(), 1);
    recent.setSourceModel(nullptr);
}

// No view paints the history in this test. Hidden conflicts must be restored
// by their targeted wallet notifications, not an O(history) block invalidation.
inline void CheckBoundedHistoryUpdates(WalletModel& source, const PlatformStyle* style)
{
    const auto tip = GUIUtil::WaitForBackendTask(source.requestWalletData([](interfaces::Wallet& backend) {
        auto& wallet = *backend.wallet();
        LOCK(wallet.cs_wallet);
        return std::make_pair(wallet.GetLastBlockHeight(), wallet.GetLastBlockHash());
    }));
    QTRY_COMPARE(source.getLastBlockProcessed(), tip.second);
    auto wallet = std::make_shared<wallet::CWallet>(&source.wallet().wallet()->chain(),
        "hidden history status", wallet::CreateMockableWalletDatabase());
    CKey key;
    key.MakeNewKey(true);
    QVERIFY(wallet::CreateDescriptor(*wallet, strprintf("rawtr(%s)", EncodeSecret(key)), true));
    CMutableTransaction parent;
    parent.vin.emplace_back(Txid::FromUint256(uint256::ONE), 0);
    parent.vout.emplace_back(COIN, XOnlyPubKey{key.GetPubKey()});
    CMutableTransaction child;
    child.vin.emplace_back(parent.GetHash(), 0);
    child.vout.emplace_back(COIN / 2, CScript{} << OP_RETURN);
    CMutableTransaction competing;
    competing.vin = parent.vin;
    competing.vout.emplace_back(COIN / 2, CScript{} << OP_RETURN);
    CMutableTransaction coinbase;
    coinbase.vin.emplace_back();
    coinbase.vout.emplace_back(COIN, XOnlyPubKey{key.GetPubKey()});
    constexpr int UNRELATED_TRANSACTIONS{32};
    Txid multi_output_hash;
    {
        LOCK(wallet->cs_wallet);
        wallet->SetLastBlockProcessed(tip.first, tip.second);
        QVERIFY(wallet->AddToWallet(MakeTransactionRef(parent), wallet::TxStateInactive{}));
        QVERIFY(wallet->AddToWallet(MakeTransactionRef(child), wallet::TxStateInactive{}));
        QVERIFY(wallet->AddToWallet(MakeTransactionRef(coinbase), wallet::TxStateInactive{/*abandoned=*/true}));
        // Spread the changed hashes through unrelated history so a single
        // broad dataChanged interval would invalidate unaffected records.
        for (int i{0}; i < UNRELATED_TRANSACTIONS; ++i) {
            CMutableTransaction unrelated;
            unrelated.vin.emplace_back(Txid::FromUint256(uint256::ONE), i + 1);
            unrelated.vout.emplace_back(COIN, XOnlyPubKey{key.GetPubKey()});
            if (i == 0) {
                unrelated.vout.emplace_back(COIN / 2, XOnlyPubKey{key.GetPubKey()});
                multi_output_hash = unrelated.GetHash();
            }
            QVERIFY(wallet->AddToWallet(MakeTransactionRef(unrelated), wallet::TxStateInactive{}));
        }
    }
    auto model = std::make_unique<WalletModel>(interfaces::MakeWallet(*source.node().walletLoader().context(), wallet),
        source.clientModel(), style);
    auto& history = *model->getTransactionTableModel();
    TransactionFilterProxy active;
    active.setSourceModel(&history);
    active.setDynamicSortFilter(true);
    active.setShowInactive(false);
    QTRY_COMPARE(active.rowCount(), UNRELATED_TRANSACTIONS + 4);
    QVERIFY(history.indexesForTransaction(Txid{}).empty());
    QVERIFY(history.indexesForTransaction(competing.GetHash()).empty());
    QCOMPARE(history.indexesForTransaction(parent.GetHash()).size(), 1);
    const auto multi_output_indexes = history.indexesForTransaction(multi_output_hash);
    QCOMPARE(multi_output_indexes.size(), 2);
    QCOMPARE(multi_output_indexes[1].row(), multi_output_indexes[0].row() + 1);
    for (const auto& item : multi_output_indexes) {
        QCOMPARE(item.model(), &history);
        QCOMPARE(item.column(), int{TransactionTableModel::Status});
        QCOMPARE(item.data(TransactionTableModel::TxHashRole).toString(), QString::fromStdString(multi_output_hash.GetHex()));
    }
    QCOMPARE(multi_output_indexes[0].data(TransactionTableModel::AmountRole).toLongLong(), COIN);
    QCOMPARE(multi_output_indexes[1].data(TransactionTableModel::AmountRole).toLongLong(), COIN / 2);
    const auto coinbase_status = [&] {
        for (int row{0}; row < active.rowCount(); ++row) {
            const auto item = active.index(row, 0);
            if (item.data(TransactionTableModel::TxHashRole).toString() == QString::fromStdString(coinbase.GetHash().GetHex())) {
                return item.data(TransactionTableModel::StatusRole).toInt();
            }
        }
        return -1;
    };
    QTRY_COMPARE(coinbase_status(), int{TransactionStatus::NotAccepted});
    QSignalSpy data_updates{&history, &QAbstractItemModel::dataChanged};
    QSignalSpy repaints{&history, &TransactionTableModel::confirmationsChanged};
    const auto changed_rows = [&] {
        int rows{0};
        for (const auto& update : data_updates) {
            rows += update.at(1).value<QModelIndex>().row() - update.at(0).value<QModelIndex>().row() + 1;
        }
        return rows;
    };
    // A block-only refresh must not scan/refilter the source model at all.
    history.updateConfirmations();
    QCOMPARE(repaints.size(), 1);
    QCOMPARE(data_updates.size(), 0);

    CBlock block;
    block.vtx = {MakeTransactionRef(coinbase), MakeTransactionRef(competing)};
    // Exercise wallet notification processing without mutating the global test
    // chain: reuse its known hash so the status worker can obtain block time.
    interfaces::BlockInfo info{tip.second};
    info.prev_hash = &tip.second;
    info.height = tip.first + 1;
    info.chain_time_max = std::numeric_limits<unsigned>::max();
    info.data = &block;
    auto connected = model->requestWalletData([info](interfaces::Wallet& backend) {
        backend.wallet()->blockConnected(kernel::ChainstateRole{}, info);
    });
    QTRY_VERIFY(connected.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
    connected.get();
    // Parent and descendant disappear solely through targeted CT_UPDATED.
    QTRY_COMPARE(active.rowCount(), UNRELATED_TRANSACTIONS + 2);
    QTRY_COMPARE(coinbase_status(), int{TransactionStatus::Immature});
    QCOMPARE(changed_rows(), 3);
    data_updates.clear();
    auto disconnected = model->requestWalletData([info](interfaces::Wallet& backend) {
        backend.wallet()->blockDisconnected(info);
    });
    QTRY_VERIFY(disconnected.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
    disconnected.get();
    QTRY_COMPARE(active.rowCount(), UNRELATED_TRANSACTIONS + 4);
    QTRY_COMPARE(coinbase_status(), int{TransactionStatus::NotAccepted});
    QCOMPARE(changed_rows(), 3);
    // Explicit all-row changes must retain their normal proxy semantics.
    data_updates.clear();
    history.updateDisplayUnit();
    QVERIFY(!data_updates.empty());
    active.setSourceModel(nullptr);
    WalletModel::destroy(model.release());
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGHISTORYMODELS_H
