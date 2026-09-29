// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGSPLASH_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGSPLASH_H

#include <connectcoin-build-config.h> // IWYU pragma: keep
#include <interfaces/node.h>
#include <node/interface_ui.h>
#include <qt/networkstyle.h>
#include <qt/splashscreen.h>
#include <util/chaintype.h>
#ifdef ENABLE_WALLET
#include <interfaces/wallet.h>
#include <wallet/context.h>
#endif

#include <QCoreApplication>
#include <QEvent>
#include <QTest>
#include <QTimer>

#include <chrono>
#include <future>
#include <memory>

inline void CheckNonblockingSplash(interfaces::Node& node)
{
    class MetaCallCounter : public QObject {
    public:
        int count{0};
        bool eventFilter(QObject*, QEvent* event) override
        {
            if (event->type() == QEvent::MetaCall) ++count;
            return false;
        }
    } calls;
    std::unique_ptr<const NetworkStyle> style{NetworkStyle::instantiate(ChainType::REGTEST)};
    auto splash = std::make_unique<SplashScreen>(style.get());
    splash->setNode(node);
    splash->installEventFilter(&calls);
    auto messages = std::async(std::launch::async, [] {
        for (int i = 0; i < 16; ++i) uiInterface.InitMessage("coalesced splash message");
        uiInterface.ShowProgress("coalesced splash progress", 50, false);
    });
    messages.get();
    QCOMPARE(calls.count, 0);
    QCoreApplication::sendPostedEvents(splash.get(), QEvent::MetaCall);
    QCOMPARE(calls.count, 1);
    calls.count = 0;

#ifdef ENABLE_WALLET
    // InitWallet registers its loader callback synchronously so it cannot miss
    // a subsequent wallet load. Stop must wait for an entered registration off
    // the GUI thread, without holding the notification gate across this lock.
    std::promise<void> acquired;
    auto locked = acquired.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    auto blocker = std::async(std::launch::async, [&] {
        LOCK(node.walletLoader().context()->wallets_mutex);
        acquired.set_value();
        return released.wait_for(std::chrono::seconds{3}) == std::future_status::timeout;
    });
    locked.wait();
    std::promise<void> started;
    auto ready = started.get_future();
    auto registration = std::async(std::launch::async, [&] {
        started.set_value();
        uiInterface.InitWallet();
    });
    ready.wait();
    const bool registration_pending = registration.wait_for(std::chrono::milliseconds{50}) != std::future_status::ready;
    int ticks{0};
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        if (++ticks == 5) release.set_value();
    });
    heartbeat.start(10);
    splash->stop();
    heartbeat.stop();
    if (ticks < 5) release.set_value();
    const bool timed_out = blocker.get();
    registration.get();
    QVERIFY(registration_pending);
    QVERIFY(!timed_out);
    QVERIFY(ticks >= 5);
#else
    splash->stop();
#endif
    uiInterface.InitMessage("late splash message");
    uiInterface.ShowProgress("late splash progress", 100, false);
    QCoreApplication::sendPostedEvents(splash.get(), QEvent::MetaCall);
    QCOMPARE(calls.count, 0);
    splash.reset();
    uiInterface.InitMessage("message after splash destruction");
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGSPLASH_H
