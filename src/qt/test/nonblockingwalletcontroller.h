// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGWALLETCONTROLLER_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGWALLETCONTROLLER_H

#include <interfaces/node.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/walletcontroller.h>
#include <qt/walletmodel.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

inline void CheckNonblockingWalletController(WalletModel& source, const PlatformStyle* style)
{
    auto& context = *source.node().walletLoader().context();
    auto mock = std::make_shared<wallet::CWallet>(&source.wallet().wallet()->chain(),
        "controller registration fixture", wallet::CreateMockableWalletDatabase());
    mock->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);

    // Explicit stop remains responsive even when disconnect must acquire a
    // loader mutex. Destruction after stop must not dispatch unrelated events.
    {
        // Directory results arrive as a completed ordered snapshot. Before
        // registering wallets, every discovered entry is marked unloaded.
        const auto directory = GUIUtil::WaitForBackendTask(source.clientModel().requestNodeData([](interfaces::Node& node) {
            return node.walletLoader().listWalletDir();
        }));
        auto controller = std::make_unique<WalletController>(source.clientModel(), style, nullptr);
        QSignalSpy directory_changed{controller.get(), &WalletController::walletDirectoryChanged};
        const auto initial_directory = controller->listWalletDir();
        QVERIFY(initial_directory.empty());
        QVERIFY(!controller->hasWalletDirSnapshot());
        QTRY_VERIFY(controller->hasWalletDirSnapshot());
        QCOMPARE(directory_changed.count(), 1);
        const auto snapshot = controller->listWalletDir();
        QCOMPARE(snapshot.size(), directory.size());
        for (const auto& [name, format] : directory) {
            const auto found = snapshot.find(name);
            QVERIFY(found != snapshot.end());
            QVERIFY(!found->second.first);
            QCOMPARE(found->second.second, format);
        }
        auto* load = new LoadWalletsActivity(controller.get(), nullptr);
        QSignalSpy loaded(load, &WalletControllerActivity::finished);
        load->load(false);
        QTRY_COMPARE(loaded.count(), 1);
        std::promise<void> acquired, release;
        auto ready = acquired.get_future();
        auto released = release.get_future();
        auto blocker = std::async(std::launch::async, [&] {
            LOCK(context.wallets_mutex);
            acquired.set_value();
            return released.wait_for(std::chrono::seconds{3}) == std::future_status::timeout;
        });
        ready.wait();
        int ticks{0};
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, [&] {
            if (++ticks == 5) release.set_value();
        });
        heartbeat.start(10);
        controller->stop();
        heartbeat.stop();
        if (ticks < 5) release.set_value();
        const bool timed_out = blocker.get();
        QVERIFY(!timed_out);
        QVERIFY(ticks >= 5);
        QVERIFY(controller->isStopping());
        QVERIFY(!controller->hasActiveActivities());
        QSignalSpy added(controller.get(), &WalletController::walletAdded);
        wallet::NotifyWalletLoaded(context, mock);
        QCoreApplication::processEvents();
        QCOMPARE(added.count(), 0);
        bool reentered{false};
        QObject event_owner;
        QTimer::singleShot(0, &event_owner, [&] { reentered = true; });
        controller.reset();
        QVERIFY(!reentered);
    }

    // A loader holding wallets_mutex may already be waiting for GUI model
    // registration. The fallback destructor must cancel and wake that request
    // without a nested event loop or a stranded controller callback.
    {
        auto controller = std::make_unique<WalletController>(source.clientModel(), style, nullptr);
        auto* load = new LoadWalletsActivity(controller.get(), nullptr);
        QSignalSpy loaded(load, &WalletControllerActivity::finished);
        load->load(false);
        QTRY_COMPARE(loaded.count(), 1);
        QTRY_VERIFY(!controller->hasActiveActivities());
        std::promise<void> entered;
        auto ready = entered.get_future();
        auto notification = std::async(std::launch::async, [&] {
            entered.set_value();
            wallet::NotifyWalletLoaded(context, mock);
        });
        ready.wait();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        // Deliberately do not process GUI events before destruction: that
        // would consume the very registration this test needs to cancel.
        while (!controller->hasActiveActivities() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        const bool registration_waiting = controller->hasActiveActivities();
        bool reentered{false};
        QObject event_owner;
        QTimer::singleShot(0, &event_owner, [&] { reentered = true; });
        controller.reset();
        notification.get();
        QVERIFY(registration_waiting);
        QVERIFY(!reentered);
        QCoreApplication::processEvents();
        QVERIFY(reentered);
        wallet::NotifyWalletLoaded(context, mock);
    }
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGWALLETCONTROLLER_H
