// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGCOREMESSAGES_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGCOREMESSAGES_H

#include <node/interface_ui.h>
#include <qt/bitcoingui.h>
#include <qt/guiutil.h>
#include <qt/networkstyle.h>
#include <qt/platformstyle.h>
#include <util/chaintype.h>
#include <util/translation.h>

#include <QCoreApplication>
#include <QEvent>
#include <QMessageBox>
#include <QTest>
#include <QTimer>

#ifdef Q_OS_MACOS
#include <qt/macdockiconhandler.h>
#include <QPointer>
#endif

#include <chrono>
#include <future>
#include <memory>

inline void CheckCoreMessageLifetime(interfaces::Node& node)
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
    const std::unique_ptr<const NetworkStyle> network_style{NetworkStyle::instantiate(ChainType::REGTEST)};
    const std::unique_ptr<const PlatformStyle> platform_style{PlatformStyle::instantiate("other")};
#ifdef Q_OS_MACOS
    const QPointer<MacDockIconHandler> dock_handler{MacDockIconHandler::instance()};
#endif
    auto window = std::make_unique<BitcoinGUI>(node, platform_style.get(), network_style.get());
    window->installEventFilter(&calls);
    auto gate = window->m_core_signal_gate;
    const bilingual_str message{"original <core> message", "translated <core> message"};
    constexpr unsigned int MODAL_STYLE{CClientUIInterface::MODAL | CClientUIInterface::BTN_OK | CClientUIInterface::BTN_CANCEL};

    // Invoke this window's bridge directly: broadcasting through uiInterface
    // would also reach the application window owned by AppTests.
    auto notification = std::async(std::launch::async, [gate, message] {
        return BitcoinGUI::ThreadSafeMessageBox(gate, message, CClientUIInterface::MSG_INFORMATION);
    });
    QVERIFY(!notification.get());
    QCOMPARE(calls.count, 0);
    // Dispatch after the caller has gone away. Nonmodal delivery must not carry
    // a pointer to its caller's stack and still uses a queued GUI event.
    QCoreApplication::sendPostedEvents(window.get(), QEvent::MetaCall);
    QCOMPARE(calls.count, 1);

    bool shown{false};
    bool text_preserved{false};
    QTimer::singleShot(0, window.get(), [&] {
        auto* dialog = window->findChild<QMessageBox*>();
        if (!dialog) return;
        shown = true;
        text_preserved = dialog->text() == QString::fromStdString(message.translated) &&
            dialog->detailedText().endsWith(QString::fromStdString(message.original)) &&
            dialog->textFormat() == Qt::PlainText;
        dialog->done(QMessageBox::Ok);
    });
    // A GUI-origin modal message must enter its dialog directly, without
    // waiting for a queued callback on its own blocked thread.
    const bool direct_accepted = BitcoinGUI::ThreadSafeMessageBox(gate, message, MODAL_STYLE);
    QVERIFY(shown);
    QVERIFY(text_preserved);
    QVERIFY(direct_accepted);

    auto modal = std::async(std::launch::async, [gate, message] {
        return BitcoinGUI::ThreadSafeMessageBox(gate, message, MODAL_STYLE);
    });
    QTimer answer_dialog;
    QObject::connect(&answer_dialog, &QTimer::timeout, [&] {
        if (auto* dialog = window->findChild<QMessageBox*>()) dialog->done(QMessageBox::Ok);
    });
    answer_dialog.start(10);
    const bool worker_accepted = GUIUtil::WaitForBackendTask(std::move(modal), window.get());
    answer_dialog.stop();
    QVERIFY(worker_accepted);

    std::promise<void> entered;
    auto started = entered.get_future();
    auto pending = std::async(std::launch::async, [gate, message, &entered] {
        entered.set_value();
        return BitcoinGUI::ThreadSafeMessageBox(gate, message, MODAL_STYLE);
    });
    started.wait();
    // Deliberately do not pump GUI events: cancellation must release a caller
    // even when its dialog has never been dispatched.
    const bool pending_before_stop = pending.wait_for(std::chrono::milliseconds{50}) != std::future_status::ready;
    window->unsubscribeFromCoreSignals();
    const bool cancelled_before_dispatch = pending.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    // Let a regressed blocking queued connection finish before reporting a
    // failure, so an async future's destructor cannot strand the test runner.
    answer_dialog.start(10);
    const bool cancelled_answer = GUIUtil::WaitForBackendTask(std::move(pending), window.get());
    QCoreApplication::sendPostedEvents(window.get(), QEvent::MetaCall);
    answer_dialog.stop();
    QVERIFY(pending_before_stop);
    QVERIFY(cancelled_before_dispatch);
    QVERIFY(!cancelled_answer);
    QVERIFY(!window->findChild<QMessageBox*>());

    // A modal dialog runs a nested GUI event loop. Unsubscribe there must not
    // contend with a gate mutex held throughout message()/QMessageBox::exec().
    window->subscribeToCoreSignals();
    gate = window->m_core_signal_gate;
    auto active = std::async(std::launch::async, [gate, message] {
        return BitcoinGUI::ThreadSafeMessageBox(gate, message, MODAL_STYLE);
    });
    bool cancelled_during_dialog{false};
    QTimer stop_dialog;
    QObject::connect(&stop_dialog, &QTimer::timeout, [&] {
        auto* dialog = window->findChild<QMessageBox*>();
        if (!dialog) return;
        stop_dialog.stop();
        window->unsubscribeFromCoreSignals();
        cancelled_during_dialog = active.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
        dialog->done(QMessageBox::Ok);
    });
    stop_dialog.start(10);
    GUIUtil::WaitForBackendTaskReady([&] {
        return active.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
    }, window.get());
    const bool active_answer = active.get();
    stop_dialog.stop();
    QVERIFY(cancelled_during_dialog);
    QVERIFY(!active_answer);

    window->subscribeToCoreSignals();
    gate = window->m_core_signal_gate;
    auto discarded_notification = std::async(std::launch::async, [gate, message] {
        return BitcoinGUI::ThreadSafeMessageBox(gate, message, CClientUIInterface::MSG_INFORMATION);
    });
    QVERIFY(!discarded_notification.get());
    std::promise<void> deletion_entered;
    auto deletion_started = deletion_entered.get_future();
    auto destroyed = std::async(std::launch::async, [gate, message, &deletion_entered] {
        deletion_entered.set_value();
        return BitcoinGUI::ThreadSafeMessageBox(gate, message, MODAL_STYLE);
    });
    deletion_started.wait();
    const bool pending_before_deletion = destroyed.wait_for(std::chrono::milliseconds{50}) != std::future_status::ready;
    // Destruction must cancel a pending modal reply and discard the queued
    // notification without requiring another GUI event-loop turn.
    window.reset();
#ifdef Q_OS_MACOS
    // Closing a temporary window must not destroy the main window's Dock
    // handler or leave a dangling singleton for application teardown.
    QVERIFY(dock_handler);
    QCOMPARE(MacDockIconHandler::instance(), dock_handler.data());
#endif
    const bool released_by_deletion = destroyed.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    const bool destroyed_answer = destroyed.get();
    QVERIFY(pending_before_deletion);
    QVERIFY(released_by_deletion);
    QVERIFY(!destroyed_answer);

    // An already-entered core callback can retain its gate beyond GUI teardown.
    // Both message kinds must reject that target without touching freed memory.
    auto late = std::async(std::launch::async, [gate, message] {
        return BitcoinGUI::ThreadSafeMessageBox(gate, message, MODAL_STYLE) ||
            BitcoinGUI::ThreadSafeMessageBox(gate, message, CClientUIInterface::MSG_INFORMATION);
    });
    QVERIFY(!late.get());
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGCOREMESSAGES_H
