// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGPEERUPDATES_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGPEERUPDATES_H

#include <qt/peertablemodel.h>
#include <qt/peertablesortproxy.h>

#include <QAbstractItemModelTester>
#include <QItemSelectionModel>
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QTest>

inline void CheckBatchedPeerUpdates(interfaces::Node& node)
{
    PeerTableModel peers{node, nullptr};
    // Use deterministic snapshots after draining the ordinary node query.
    peers.stop();
    PeerTableSortProxy proxy;
    proxy.setSourceModel(&peers);
    proxy.setDynamicSortFilter(true);
    proxy.sort(PeerTableModel::NetNodeId, Qt::DescendingOrder);
    QAbstractItemModelTester source_tester{&peers, QAbstractItemModelTester::FailureReportingMode::QtTest};
    QAbstractItemModelTester proxy_tester{&proxy, QAbstractItemModelTester::FailureReportingMode::QtTest};
    const auto snapshot = [](int first, int end, uint64_t bytes = 0) {
        QList<CNodeCombinedStats> rows;
        rows.reserve(end - first);
        for (int id = first; id < end; ++id) {
            CNodeCombinedStats stats{};
            stats.nodeStats.nodeid = id;
            stats.nodeStats.nSendBytes = bytes;
            rows.append(std::move(stats));
        }
        return rows;
    };
    constexpr int ROWS{2048};
    peers.applyStats(snapshot(0, ROWS));
    QCOMPARE(peers.rowCount(), ROWS);
    QCOMPARE(proxy.index(0, 0).data().toInt(), ROWS - 1);
    QPersistentModelIndex retained{peers.index(1000, 0)};
    QItemSelectionModel selection{&proxy};
    selection.select(proxy.mapFromSource(retained), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QSignalSpy removed{&peers, &QAbstractItemModel::rowsRemoved};
    QSignalSpy inserted{&peers, &QAbstractItemModel::rowsInserted};

    // Hundreds of consecutive departures become one range, not hundreds of
    // list shifts/proxy notifications. Appended IDs stay sorted by the proxy.
    peers.applyStats(snapshot(900, ROWS + 10, 123));
    QCOMPARE(removed.count(), 1);
    QCOMPARE(removed.at(0).at(1).toInt(), 0);
    QCOMPARE(removed.at(0).at(2).toInt(), 899);
    QCOMPARE(inserted.count(), 1);
    QVERIFY(retained.isValid());
    QCOMPARE(retained.row(), 100);
    QCOMPARE(retained.data().toInt(), 1000);
    QCOMPARE(retained.data(PeerTableModel::StatsRole).value<CNodeCombinedStats*>()->nodeStats.nSendBytes, uint64_t{123});
    QCOMPARE(selection.selectedRows().size(), 1);
    QCOMPARE(selection.selectedRows().front().data().toInt(), 1000);

    // Separated runs must not discard a surviving selected peer or retain an
    // index into a freed old snapshot.
    auto sparse = snapshot(1000, 1001, 456);
    sparse.append(snapshot(1500, 1501, 456));
    peers.applyStats(std::move(sparse));
    QCOMPARE(removed.count(), 4); // prefix, middle and suffix in this update
    QCOMPARE(peers.rowCount(), 2);
    QVERIFY(retained.isValid());
    QCOMPARE(retained.row(), 0);
    QCOMPARE(retained.data().toInt(), 1000);
    QCOMPARE(retained.data(PeerTableModel::StatsRole).value<CNodeCombinedStats*>()->nodeStats.nSendBytes, uint64_t{456});
    QCOMPARE(proxy.index(0, 0).data().toInt(), 1500);

    removed.clear();
    peers.applyStats({});
    QCOMPARE(removed.count(), 1);
    QCOMPARE(removed.at(0).at(1).toInt(), 0);
    QCOMPARE(removed.at(0).at(2).toInt(), 1);
    QVERIFY(!retained.isValid());
    QVERIFY(selection.selectedRows().isEmpty());
    QCOMPARE(proxy.rowCount(), 0);
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGPEERUPDATES_H
