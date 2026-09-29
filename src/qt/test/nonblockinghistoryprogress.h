// Copyright (c) The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGHISTORYPROGRESS_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGHISTORYPROGRESS_H

#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>
#include <wallet/wallet.h>

#include <future>

#include <QCoreApplication>
#include <QEvent>
#include <QScopeGuard>
#include <QTest>

inline void CheckTransactionProgressCoalescing(WalletModel& model, wallet::CWallet& wallet, const PlatformStyle* style)
{
    TransactionTableModel history{style, &model};
    QTRY_VERIFY(history.rowCount({}) > 0);
    const int initial_rows = history.rowCount({});
    const auto hash_text = history.index(0, 0).data(TransactionTableModel::TxHashRole).toString();
    const auto hash = Txid::FromHex(hash_text.toStdString()).value();
    history.updateTransaction(hash_text, CT_DELETED, true);
    const int removed_rows = history.rowCount({});
    QVERIFY(removed_rows < initial_rows);
    const auto complete_progress = qScopeGuard([&] { wallet.ShowProgress("history progress fixture", 100); });
    class MetaCalls final : public QObject {
    public:
        int calls{0};
        bool eventFilter(QObject*, QEvent* event) override
        {
            if (event->type() == QEvent::MetaCall) ++calls;
            return false;
        }
    } delivery;
    history.installEventFilter(&delivery);

    // Joining a rescan at 49%, without seeing its original 0%, must still
    // enter buffering. Repeated progress does not change the table's state.
    auto producer = std::async(std::launch::async, [&] {
        for (int count{0}; count < 10'000; ++count) wallet.ShowProgress("history progress fixture", 49);
        wallet.NotifyTransactionChanged(hash, CT_NEW);
    });
    producer.get();
    QCoreApplication::sendPostedEvents(&history, QEvent::MetaCall);
    QCOMPARE(delivery.calls, 1);
    QCOMPARE(history.rowCount({}), removed_rows);

    producer = std::async(std::launch::async, [&] {
        for (int count{0}; count < 10'000; ++count) wallet.ShowProgress("history progress fixture", 50);
    });
    producer.get();
    QCoreApplication::sendPostedEvents(&history, QEvent::MetaCall);
    QCOMPARE(delivery.calls, 1); // No no-op progress deliveries were posted.
    QCOMPARE(history.rowCount({}), removed_rows);

    wallet.ShowProgress("history progress fixture", 100);
    QCoreApplication::sendPostedEvents(&history, QEvent::MetaCall);
    QCOMPARE(delivery.calls, 2);
    QTRY_COMPARE(history.rowCount({}), initial_rows);

    // Completion is idempotent too; a subsequent start remains a real event.
    wallet.ShowProgress("history progress fixture", 100);
    QCoreApplication::sendPostedEvents(&history, QEvent::MetaCall);
    QCOMPARE(delivery.calls, 2);
    wallet.ShowProgress("history progress fixture", 0);
    wallet.ShowProgress("history progress fixture", 100);
    QCoreApplication::sendPostedEvents(&history, QEvent::MetaCall);
    QCOMPARE(delivery.calls, 3);
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGHISTORYPROGRESS_H
