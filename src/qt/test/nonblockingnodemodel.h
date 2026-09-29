// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGNODEMODEL_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGNODEMODEL_H

#include <qt/bantablemodel.h>
#include <qt/clientmodel.h>
#include <qt/peertablemodel.h>

#include <interfaces/node.h>
#include <node/interface_ui.h>
#include <sync.h>
#include <validation.h>

#include <chrono>
#include <array>
#include <future>
#include <memory>
#include <string>

#include <QTableView>
#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// Exercise Qt's cached node paths while chain work is stalled. The minimal Qt
// test platform covers event dispatch/rendering, not native taskbar latency.
inline void CheckNonblockingNodeSnapshots(ClientModel& source, interfaces::Node& node)
{
    std::promise<void> locked;
    auto ready = locked.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    auto holder = std::async(std::launch::async, [&] {
        LOCK(cs_main);
        locked.set_value();
        return released.wait_for(std::chrono::seconds{3}) == std::future_status::timeout;
    });
    ready.wait();

    int heartbeats{0};
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
    heartbeat.start(10);
    auto model = std::make_unique<ClientModel>(node, source.getOptionsModel());
    auto* peers = model->getPeerTableModel();
    auto* bans = model->getBanTableModel();
    peers->startAutoRefresh();
    bans->refresh();

    // These were synchronous backend queries before the snapshot change.
    model->getNumConnections();
    model->getNumBlocks();
    model->getBestBlockHash();
    model->getHeaderTipHeight();
    model->getHeaderTipTime();
    model->getBlockSource();
    model->getStatusBarWarnings();
    model->getNetLocalAddresses();
    model->getTotalBytesRecv();
    model->getTotalBytesSent();
    model->getCpuMiningStatus();
    std::string proxy;
    model->getProxyInfo(proxy);

    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* peer_view = new QTableView(&window);
    auto* ban_view = new QTableView(&window);
    peer_view->setModel(peers);
    ban_view->setModel(bans);
    layout->addWidget(peer_view);
    layout->addWidget(ban_view);
    window.resize(640, 480);
    window.show();
    const bool rendered = !window.grab().isNull();
    window.showMinimized();
    window.showNormal();
    // Timer overruns are coalesced if a loaded CI runner is descheduled.
    // Wait for delivered GUI events while the chain lock is still held.
    const bool responsive = QTest::qWaitFor([&] { return heartbeats >= 3; }, 1000);
    const bool snapshot_pending = !model->hasNodeState();
    release.set_value();
    const bool timed_out = holder.get();
    heartbeat.stop();

    QVERIFY2(!timed_out, "Node snapshot work exhausted the 3-second cs_main lock watchdog");
    QVERIFY(rendered);
    QVERIFY(snapshot_pending);
    QVERIFY2(responsive, qPrintable(QString("Node snapshot GUI delivered %1 of 3 required heartbeats within 1 second").arg(heartbeats)));
    QTRY_VERIFY(model->hasNodeState());
    // Unchanged periodic snapshots must not reload icons or reformat hidden
    // node-window metadata. Statistics still publish through their signals.
    QSignalSpy metadata(model.get(), &ClientModel::nodeStateChanged);
    QSignalSpy bytes(model.get(), &ClientModel::bytesChanged);
    QTRY_VERIFY(bytes.count() >= 2);
    QCOMPARE(metadata.count(), 0);

    // Connection/network/alert/ban invalidations share one pending GUI event,
    // not merely one outstanding backend query. Observe actual queued-event
    // delivery so a burst regression cannot hide behind a fast test backend.
    {
        class RefreshEventCounter : public QObject {
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
        for (int burst = 0; burst < 2; ++burst) {
            auto refreshes = std::async(std::launch::async, [] {
                for (int i = 0; i < 2000; ++i) {
                    uiInterface.NotifyNumConnectionsChanged(i);
                    uiInterface.NotifyNetworkActiveChanged(i % 2 == 0);
                    uiInterface.NotifyAlertChanged();
                    uiInterface.BannedListChanged();
                }
            });
            refreshes.get();
            QCOMPARE(counter.calls, burst);
            QCoreApplication::sendPostedEvents(model.get(), QEvent::MetaCall);
            QCOMPARE(counter.calls, burst + 1);
        }
        model->removeEventFilter(&counter);
    }

    // Core callbacks never touch a Qt model from their emitting thread. Tip
    // bursts are coalesced without losing the newest header's height/time.
    QSignalSpy tips(model.get(), &ClientModel::numBlocksChanged);
    QSignalSpy progress(model.get(), &ClientModel::showProgress);
    const int header_height = model->getHeaderTipHeight();
    const int64_t header_time = model->getHeaderTipTime();
    auto notifications = std::async(std::launch::async, [header_height, header_time] {
        uiInterface.ShowProgress("background notification", 25, false);
        for (int i = 1; i <= 3; ++i) {
            uiInterface.NotifyHeaderTip(SynchronizationState::POST_INIT, header_height + i, header_time + i, false);
        }
    });
    notifications.get();
    QCOMPARE(tips.count(), 0);
    QCOMPARE(progress.count(), 0);
    QTRY_COMPARE(model->getHeaderTipHeight(), header_height + 3);
    QCOMPARE(model->getHeaderTipTime(), header_time + 3);
    QCOMPARE(tips.count(), 1);
    QCOMPARE(progress.count(), 1);
    uiInterface.NotifyHeaderTip(SynchronizationState::POST_INIT, header_height, header_time, false);
    QTRY_COMPARE(model->getHeaderTipHeight(), header_height);
    tips.clear();
    progress.clear();

    // Core verification/replay reports every block, including repeated
    // percentages. This must not become one GUI event per block. Intermediate
    // values may merge; starts/completions and separate operations keep order.
    {
        class ProgressEventCounter : public QObject {
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
        auto burst = std::async(std::launch::async, [] {
            for (int i = 0; i < 10; ++i) uiInterface.ShowProgress("verify burst", 0, false);
            for (int i = 0; i < 10000; ++i) uiInterface.ShowProgress("verify burst", 49, false);
            uiInterface.ShowProgress("verify burst", 87, false);
            uiInterface.ShowProgress("verify burst", 100, false);
            uiInterface.ShowProgress("next operation", 0, false);
            uiInterface.ShowProgress("next operation", 23, false);
            uiInterface.ShowProgress("next operation", 100, false);
        });
        burst.get();
        QCOMPARE(progress.count(), 0);
        QCoreApplication::sendPostedEvents(model.get(), QEvent::MetaCall);
        QCOMPARE(counter.calls, 1);
        QCOMPARE(progress.count(), 6);
        const std::array<int, 6> expected{0, 87, 100, 0, 23, 100};
        for (size_t i = 0; i < expected.size(); ++i) {
            QCOMPARE(progress.at(i).at(1).toInt(), expected[i]);
            QCOMPARE(progress.at(i).at(0).toString(), i < 3 ? QString{"verify burst"} : QString{"next operation"});
        }
        // Repeated values remain redundant after the previous event drained.
        auto duplicate = std::async(std::launch::async, [] {
            uiInterface.ShowProgress("next operation", 100, false);
        });
        duplicate.get();
        QCoreApplication::sendPostedEvents(model.get(), QEvent::MetaCall);
        QCOMPARE(counter.calls, 1);
        QCOMPARE(progress.count(), 6);
        progress.clear();
        counter.calls = 0;

        // Separate operation boundaries cannot merge, but their delivery still
        // yields after 64 values instead of monopolizing one GUI callback.
        auto boundaries = std::async(std::launch::async, [] {
            for (int i = 0; i < 100; ++i) {
                uiInterface.ShowProgress("bounded lifecycle", 0, false);
                uiInterface.ShowProgress("bounded lifecycle", 100, false);
            }
        });
        boundaries.get();
        QCoreApplication::sendPostedEvents(model.get(), QEvent::MetaCall);
        QCOMPARE(counter.calls, 1);
        QCOMPARE(progress.count(), 64);
        QTRY_COMPARE(progress.count(), 200);
        QCOMPARE(counter.calls, 4);
        for (int i = 0; i < progress.count(); ++i) QCOMPARE(progress.at(i).at(1).toInt(), i % 2 ? 100 : 0);
        progress.clear();
        model->removeEventFilter(&counter);
    }

    // Draining a node job must also keep dispatching GUI events. A bounded
    // lock owner turns a synchronous-join regression into a failure, not a hang.
    std::promise<void> stop_locked;
    auto stop_ready = stop_locked.get_future();
    std::promise<void> stop_release;
    auto stop_released = stop_release.get_future();
    auto stop_holder = std::async(std::launch::async, [&] {
        LOCK(cs_main);
        stop_locked.set_value();
        return stop_released.wait_for(std::chrono::seconds{3}) == std::future_status::timeout;
    });
    stop_ready.wait();
    auto pending = model->requestNodeData([](interfaces::Node& backend) { return backend.getNumBlocks(); });
    int stop_ticks{0};
    QTimer stop_heartbeat;
    QObject::connect(&stop_heartbeat, &QTimer::timeout, [&] {
        if (++stop_ticks == 5) stop_release.set_value();
    });
    stop_heartbeat.start(10);
    // These are already queued when stop nulls the shared Core callback gate.
    // Its responsive drain must not publish them or restart any producer.
    auto late_notifications = std::async(std::launch::async, [header_height, header_time] {
        uiInterface.ShowProgress("stopped notification", 50, false);
        uiInterface.NotifyHeaderTip(SynchronizationState::POST_INIT, header_height, header_time, false);
        uiInterface.NotifyNumConnectionsChanged(0);
        uiInterface.NotifyNetworkActiveChanged(false);
        uiInterface.NotifyAlertChanged();
        uiInterface.BannedListChanged();
    });
    late_notifications.get();
    model->stopWorkers();
    stop_heartbeat.stop();
    const bool stop_timed_out = stop_holder.get();
    QVERIFY(!stop_timed_out);
    QVERIFY(stop_ticks >= 5);
    QCOMPARE(tips.count(), 0);
    QCOMPARE(progress.count(), 0);
    QVERIFY(pending.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
    pending.get();
    // Already queued refreshes must not restart any producers after stop.
    peers->refresh();
    bans->refresh();
    QTest::qWait(50);
    for (auto* timer : model->findChildren<QTimer*>()) QVERIFY(!timer->isActive());
    peer_view->setModel(nullptr);
    ban_view->setModel(nullptr);
    model.reset();
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGNODEMODEL_H
