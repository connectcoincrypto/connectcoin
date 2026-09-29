// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGWALLETPROGRESS_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGWALLETPROGRESS_H

#include <interfaces/node.h>
#include <qt/walletmodel.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <array>
#include <future>
#include <memory>

#include <QCoreApplication>
#include <QEvent>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>

inline void CheckBoundedWalletProgress(WalletModel& source, const PlatformStyle* style)
{
    auto wallet = std::make_shared<wallet::CWallet>(&source.wallet().wallet()->chain(),
        "bounded wallet progress", wallet::CreateMockableWalletDatabase());
    wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
    auto model = std::make_unique<WalletModel>(
        interfaces::MakeWallet(*source.node().walletLoader().context(), wallet), source.clientModel(), style);
    const auto cleanup = qScopeGuard([&] { WalletModel::destroy(model.release()); });
    QSignalSpy progress(model.get(), &WalletModel::showProgress);
    class EventCounter : public QObject {
    public:
        int calls{0};
        bool eventFilter(QObject* object, QEvent* event) override
        {
            if (event->type() == QEvent::MetaCall) ++calls;
            return QObject::eventFilter(object, event);
        }
    } counter;
    QCoreApplication::sendPostedEvents(model.get(), QEvent::MetaCall);
    model->installEventFilter(&counter);

    // Simulate long rescans which report thousands of unchanged percentages.
    // Only the newest pending intermediate value is useful to the dialog.
    auto burst = std::async(std::launch::async, [wallet] {
        for (int i{0}; i < 10; ++i) wallet->ShowProgress("rescan one", 0);
        for (int i{0}; i < 10000; ++i) wallet->ShowProgress("rescan one", 49);
        wallet->ShowProgress("rescan one", 87);
        wallet->ShowProgress("rescan one", 100);
        wallet->ShowProgress("rescan two", 0);
        wallet->ShowProgress("rescan two", 23);
        wallet->ShowProgress("rescan two", 100);
    });
    burst.get();
    QCOMPARE(progress.count(), 0);
    QCoreApplication::sendPostedEvents(model.get(), QEvent::MetaCall);
    QCOMPARE(counter.calls, 1);
    QCOMPARE(progress.count(), 6);
    const std::array<int, 6> expected{0, 87, 100, 0, 23, 100};
    for (size_t i{0}; i < expected.size(); ++i) {
        QCOMPARE(progress.at(i).at(1).toInt(), expected[i]);
        QCOMPARE(progress.at(i).at(0).toString(), i < 3 ? QString{"rescan one"} : QString{"rescan two"});
    }
    wallet->ShowProgress("rescan two", 100);
    QCoreApplication::sendPostedEvents(model.get(), QEvent::MetaCall);
    QCOMPARE(counter.calls, 1);
    QCOMPARE(progress.count(), 6);

    // A new title or operation boundary must not be merged with another one.
    progress.clear();
    counter.calls = 0;
    auto boundaries = std::async(std::launch::async, [wallet] {
        for (int i{0}; i < 100; ++i) {
            wallet->ShowProgress("successive rescans", 0);
            wallet->ShowProgress("successive rescans", 100);
        }
    });
    boundaries.get();
    QCoreApplication::sendPostedEvents(model.get(), QEvent::MetaCall);
    QCOMPARE(counter.calls, 1);
    QCOMPARE(progress.count(), 64);
    QTRY_COMPARE(progress.count(), 200);
    QCOMPARE(counter.calls, 4);
    for (int i{0}; i < progress.count(); ++i) QCOMPARE(progress.at(i).at(1).toInt(), i % 2 ? 100 : 0);

    // Reentrant progress goes into the next batch, without losing the wakeup.
    progress.clear();
    counter.calls = 0;
    auto next = QObject::connect(model.get(), &WalletModel::showProgress, model.get(), [wallet](const QString& title, int value) {
        if (title == "reentrant rescan" && value == 45) wallet->ShowProgress("reentrant rescan", 46);
    });
    wallet->ShowProgress("reentrant rescan", 45);
    QCoreApplication::sendPostedEvents(model.get(), QEvent::MetaCall);
    QCOMPARE(progress.count(), 1);
    QTRY_COMPARE(progress.count(), 2);
    QCOMPARE(progress.at(0).at(1).toInt(), 45);
    QCOMPARE(progress.at(1).at(1).toInt(), 46);
    QCOMPARE(counter.calls, 2);
    QObject::disconnect(next);

    // Progress dialogs can pump events in setValue. Teardown during the first
    // delivery must discard the rest of the current batch and all later work.
    progress.clear();
    auto stop = QObject::connect(model.get(), &WalletModel::showProgress, model.get(), [&](const QString&, int) {
        model->stopWorker();
    });
    wallet->ShowProgress("stopping rescan", 0);
    wallet->ShowProgress("stopping rescan", 50);
    wallet->ShowProgress("stopping rescan", 100);
    QCoreApplication::sendPostedEvents(model.get(), QEvent::MetaCall);
    QCOMPARE(progress.count(), 1);
    QObject::disconnect(stop);
    wallet->ShowProgress("stopped rescan", 0);
    QCoreApplication::processEvents();
    QCOMPARE(progress.count(), 1);
    model->removeEventFilter(&counter);
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGWALLETPROGRESS_H
