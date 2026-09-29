// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/apptests.h>

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <chainparams.h>
#include <common/args.h>
#include <key.h>
#include <logging.h>
#include <node/interface_ui.h>
#include <qt/bitcoin.h>
#include <qt/bitcoingui.h>
#include <qt/guiutil.h>
#include <qt/networkstyle.h>
#include <qt/platformstyle.h>
#include <qt/qtlogforwarder.h>
#include <qt/rpcconsole.h>
#include <qt/utilitydialog.h>
#include <qt/test/nonblockingintro.h>
#include <qt/test/nonblockingnetworkstyle.h>
#include <qt/test/nonblockingtranslations.h>
#include <qt/test/nonblockingcoremessages.h>
#include <qt/test/nonblockingrpcconsole.h>
#include <qt/test/nonblockingsplash.h>
#ifdef ENABLE_WALLET
#include <qt/miningpage.h>
#include <qt/modaloverlay.h>
#include <qt/walletframe.h>
#endif
#include <test/util/setup_common.h>
#include <validation.h>

#include <QAction>
#include <QCoreApplication>
#include <QEvent>
#include <QIcon>
#include <QImage>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QtGlobal>
#include <QtTest/QtTestWidgets>
#include <QtTest/QtTestGui>

#include <algorithm>
#include <atomic>
#include <functional>
#include <future>

namespace {
void CheckQtInfoLogging()
{
    std::promise<void> release;
    auto released = release.get_future();
    std::atomic<bool> entered{false};
    bool sink_on_gui{false}, timed_out{false}, system_seen{false};
    const auto callback = LogInstance().PushBackCallback([&](const std::string& message) {
        if (message.find("System: ") != std::string::npos) system_seen = true;
        if (message.find("Qt ") == std::string::npos || entered.exchange(true)) return;
        sink_on_gui = QThread::currentThread() == qApp->thread();
        timed_out = released.wait_for(std::chrono::seconds{3}) != std::future_status::ready;
    });
    bool timer_fired{false};
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
        if (!entered || timer_fired) return;
        timer_fired = true;
        release.set_value();
    });
    heartbeat.start(10);
    GUIUtil::LogQtInfo();
    heartbeat.stop();
    if (!timer_fired) release.set_value();
    LogInstance().DeleteCallback(callback);
    QVERIFY(entered.load());
    QVERIFY(system_seen);
    QVERIFY(timer_fired);
    QVERIFY(!timed_out);
    QVERIFY(!sink_on_gui);
}

void CheckBoundedQtLogging()
{
    std::promise<void> entered;
    auto ready = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    bool sink_on_gui{false};
    bool timed_out{false};
    bool drop_reported{false};
    qsizetype largest_message{0};
    QtLogForwarder forwarder{[&, first = true](QtMsgType, const QString& message) mutable {
        sink_on_gui |= QThread::currentThread() == qApp->thread();
        largest_message = std::max(largest_message, message.size());
        drop_reported |= message.contains("queue saturated; discarded");
        if (first) {
            first = false;
            entered.set_value();
            timed_out = released.wait_for(std::chrono::seconds{2}) != std::future_status::ready;
        }
    }};
    QVERIFY(forwarder.submit(QtInfoMsg, "Hold the log sink"));
    ready.wait();
    forwarder.submit(QtDebugMsg, QString(QtLogForwarder::MAX_MESSAGE_CHARS * 4, QChar('x')));
    for (size_t i = 0; i < QtLogForwarder::MAX_QUEUED_MESSAGES * 2; ++i) {
        forwarder.submit(QtDebugMsg, QString(1024, QChar('x')));
    }
    const auto saturated = forwarder.stats();
    bool timer_fired{false};
    QTimer heartbeat;
    heartbeat.setSingleShot(true);
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
        timer_fired = true;
        release.set_value();
    });
    heartbeat.start(50);
    GUIUtil::WaitForBackendTask(std::async(std::launch::async, [&] { forwarder.stop(); }));
    heartbeat.stop();
    if (!timer_fired) release.set_value();
    QVERIFY(timer_fired);
    QVERIFY(!timed_out);
    QVERIFY(!sink_on_gui);
    QVERIFY(drop_reported);
    QVERIFY(largest_message <= QtLogForwarder::MAX_MESSAGE_CHARS);
    QVERIFY(saturated.dropped_messages > 0);
    QVERIFY(saturated.queued_messages <= QtLogForwarder::MAX_QUEUED_MESSAGES);
    QVERIFY(saturated.queued_bytes <= QtLogForwarder::MAX_QUEUED_BYTES);
    QCOMPARE(forwarder.stats().queued_messages, size_t{0});
    QVERIFY(!forwarder.submit(QtInfoMsg, "The stopped queue rejects new work"));
}

//! Startup/shutdown must dispatch GUI events even while the backend needs a
//! settings lock held by another thread (for example a slow settings-file write).
bool WithBlockedSettings(const std::function<void()>& operation)
{
    std::promise<void> acquired;
    auto locked = acquired.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    std::atomic<bool> timed_out{false};
    auto blocker = std::async(std::launch::async, [&] {
        gArgs.LockSettings([&](common::Settings&) {
            acquired.set_value();
            timed_out = released.wait_for(std::chrono::seconds{2}) != std::future_status::ready;
        });
    });
    locked.wait();
    bool timer_fired{false};
    QTimer heartbeat;
    heartbeat.setSingleShot(true);
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
        timer_fired = true;
        release.set_value();
    });
    heartbeat.start(50);
    operation();
    heartbeat.stop();
    if (!timer_fired) release.set_value();
    blocker.get();
    return timer_fired && !timed_out;
}

void CheckLazyHelp(BitcoinGUI* window)
{
    // Constructing the main window must not build a hidden help document or
    // read cs_args. The explicit first help action loads it off-GUI instead.
    QVERIFY(!window->findChild<HelpMessageDialog*>());
    // The test runner clears the ordinary server argument registry between
    // fixtures; register one known option to exercise actual document content.
    gArgs.AddArg("-qt-responsive-help", "GUI help loading regression", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-qt-responsive-help-extra", "Second option in one group", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    gArgs.AddArg("-qt-responsive-help-network", "Another help group", ArgsManager::ALLOW_ANY, OptionsCategory::CONNECTION);
    bool invoked{false};
    const bool responsive = WithBlockedSettings([&] {
        invoked = QMetaObject::invokeMethod(window, "showHelpMessageClicked", Qt::DirectConnection);
    });
    QVERIFY(invoked);
    QVERIFY(responsive);
    auto* dialog = window->findChild<HelpMessageDialog*>();
    QVERIFY(dialog && dialog->isVisible());
    const auto* document = dialog->findChild<QTextEdit*>("helpMessage");
    QVERIFY(document);
    const auto help_text = document->toPlainText();
    const bool option_present = help_text.contains("-qt-responsive-help") &&
        help_text.contains("-qt-responsive-help-extra") && help_text.contains("Second option in one group") &&
        help_text.contains("-qt-responsive-help-network") && help_text.contains("Another help group");
    dialog->hide();
    QVERIFY(option_present);
    QVERIFY(QMetaObject::invokeMethod(window, "showHelpMessageClicked", Qt::DirectConnection));
    QCOMPARE(window->findChildren<HelpMessageDialog*>().size(), 1);
    QCOMPARE(window->findChild<HelpMessageDialog*>(), dialog);
    dialog->hide();
}

void CheckWaitSurvivesCallerDeletion()
{
    auto* caller = new QWidget;
    caller->show();
    QPointer<QWidget> caller_guard{caller};
    std::promise<int> completed;
    auto result = completed.get_future();
    int ticks{0};
    bool caller_deleted_before_result{false};
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        if (++ticks == 1) caller->deleteLater();
        if (ticks == 5) {
            caller_deleted_before_result = caller_guard.isNull();
            completed.set_value(42);
        }
    });
    heartbeat.start(10);
    const int value = GUIUtil::WaitForBackendTask(std::move(result), caller);
    heartbeat.stop();
    QVERIFY(caller_deleted_before_result);
    QVERIFY(caller_guard.isNull());
    QVERIFY(ticks >= 5);
    QCOMPARE(value, 42);
}

//! Regex find a string group inside of the console output
QString FindInConsole(const QString& output, const QString& pattern)
{
    const QRegularExpression re(pattern);
    return re.match(output).captured(1);
}

//! Call getblockchaininfo RPC and check first field of JSON output.
void TestRpcCommand(RPCConsole* console)
{
    QTextEdit* messagesWidget = console->findChild<QTextEdit*>("messagesWidget");
    QLineEdit* lineEdit = console->findChild<QLineEdit*>("lineEdit");
    QSignalSpy mw_spy(messagesWidget, &QTextEdit::textChanged);
    QVERIFY(mw_spy.isValid());
    QTest::keyClicks(lineEdit, "getblockchaininfo");
    QTest::keyClick(lineEdit, Qt::Key_Return);
    QVERIFY(mw_spy.wait(1000));
    QCOMPARE(mw_spy.count(), 2);
    const QString output = messagesWidget->toPlainText();
    const QString pattern = QStringLiteral("\"chain\": \"(\\w+)\"");
    QCOMPARE(FindInConsole(output, pattern), QString("regtest"));
}
//! Creation and automatic claims keep distinct glyphs after a theme change.
void TestP2CIcon(BitcoinGUI* window)
{
    auto* action = window->findChild<QAction*>("p2cAction");
    auto* claims = window->findChild<QAction*>("p2cClaimAction");
    QVERIFY(action && claims);
    const QImage source(":/icons/p2c");
    const QImage claims_source(":/icons/p2c_claim");
    QVERIFY(!source.isNull());
    QVERIFY(!claims_source.isNull());
    QVERIFY(source.hasAlphaChannel());
    QVERIFY(claims_source.hasAlphaChannel());
    QCOMPARE(source.size(), QSize(128, 128));
    QCOMPARE(claims_source.size(), QSize(128, 128));
    QCOMPARE(source.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(claims_source.pixelColor(0, 0).alpha(), 0);
    const auto expected = QIcon(":/icons/p2c").pixmap(32, 32).toImage().convertToFormat(QImage::Format_Alpha8);
    const auto claims_expected = QIcon(":/icons/p2c_claim").pixmap(32, 32).toImage().convertToFormat(QImage::Format_Alpha8);
    const auto send = QIcon(":/icons/send").pixmap(32, 32).toImage().convertToFormat(QImage::Format_Alpha8);
    QVERIFY(expected != send);
    QVERIFY(claims_expected != send);
    QVERIFY(claims_expected != expected);
    QCOMPARE(action->icon().pixmap(32, 32).toImage().convertToFormat(QImage::Format_Alpha8), expected);
    QCOMPARE(claims->icon().pixmap(32, 32).toImage().convertToFormat(QImage::Format_Alpha8), claims_expected);
    QEvent palette_change(QEvent::PaletteChange);
    QCoreApplication::sendEvent(window, &palette_change);
    QCOMPARE(action->icon().pixmap(32, 32).toImage().convertToFormat(QImage::Format_Alpha8), expected);
    QCOMPARE(claims->icon().pixmap(32, 32).toImage().convertToFormat(QImage::Format_Alpha8), claims_expected);
    // Exercise the recoloring path even when this test runs on Windows.
    for (const auto& platform : {QStringLiteral("windows"), QStringLiteral("macosx"), QStringLiteral("other")}) {
        QScopedPointer<const PlatformStyle> style(PlatformStyle::instantiate(platform));
        QVERIFY(style);
        QCOMPARE(style->SingleColorIcon(QStringLiteral(":/icons/p2c")).pixmap(32, 32).toImage().convertToFormat(QImage::Format_Alpha8), expected);
        QCOMPARE(style->SingleColorIcon(QStringLiteral(":/icons/p2c_claim")).pixmap(32, 32).toImage().convertToFormat(QImage::Format_Alpha8), claims_expected);
    }
}

//! Disabling desktop notifications must not suppress modal errors/confirmations.
void TestNotificationDialogs(BitcoinGUI* window)
{
    bool shown{false};
    bool accepted{false};
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, window, [&] {
        auto* dialog = window->findChild<QMessageBox*>();
        if (dialog) {
            shown = true;
            dialog->done(QMessageBox::Ok);
        }
    });
    timer.start(0);
    window->message("Notification settings test", "Modal errors remain visible", CClientUIInterface::MSG_ERROR, &accepted);
    QVERIFY(shown);
    QVERIFY(accepted);
}

#ifdef ENABLE_WALLET
//! Claims is the default main tab, directly before creation, without enabling it.
void TestAutomaticClaimsNavigation(BitcoinGUI* window)
{
    auto* claims = window->findChild<QAction*>("p2cClaimAction");
    auto* create = window->findChild<QAction*>("p2cAction");
    auto* toolbar = window->findChild<QToolBar*>();
    auto* frame = window->findChild<WalletFrame*>();
    QVERIFY(claims && create && toolbar && frame);
    QVERIFY(!frame->currentWalletModel());
    QVERIFY(claims->isChecked());
    QVERIFY(!create->isChecked());
    QVERIFY(!claims->isEnabled());
    QCOMPARE(claims->text(), QCoreApplication::translate("P2CCreateDialog", "Automatic claims"));
    const auto actions{toolbar->actions()};
    const auto position{actions.indexOf(claims)};
    QVERIFY(position >= 0);
    QVERIFY(position + 1 < actions.size());
    QCOMPARE(actions.at(position + 1), create);
    window->gotoP2CPage();
    QVERIFY(create->isChecked());
    QVERIFY(!claims->isChecked());
    window->gotoP2CClaimPage();
    QVERIFY(claims->isChecked());
    QVERIFY(!create->isChecked());
    QVERIFY(!claims->isEnabled());
}

//! The node's mining controls are available before any wallet is loaded.
void TestMiningWithoutWallet(BitcoinGUI* window)
{
    auto* frame = window->findChild<WalletFrame*>();
    QVERIFY(frame);
    QVERIFY(!frame->currentWalletModel());
    // An old genesis can display the initial-sync overlay, which disables all
    // tab actions until the user closes it.
    auto* overlay = window->findChild<ModalOverlay*>();
    QVERIFY(overlay);
    overlay->closeClicked();
    auto* action = window->findChild<QAction*>("miningAction");
    QVERIFY(action);
    QVERIFY(action->isEnabled());
    action->trigger();
    auto* page = frame->findChild<MiningPage*>("walletlessMiningPage");
    QVERIFY(page);
    QVERIFY(page->isVisible());
    QVERIFY(page->findChild<QPushButton*>("startMining")->isEnabled());
    QVERIFY(!page->findChild<QPushButton*>("newMiningAddress")->isEnabled());
    window->gotoOverviewPage();
}
#endif
} // namespace

//! Entry point for BitcoinApplication tests.
void AppTests::appTests()
{
    qRegisterMetaType<interfaces::BlockAndHeaderTipInfo>("interfaces::BlockAndHeaderTipInfo");
    QVERIFY2(WithBlockedSettings([&] { m_app.parameterSetup(); }),
             "Initial logging/parameter setup blocked GUI events on cs_args");
    gArgs.ForceSetArg("-lang", "en");
    QVERIFY2(WithBlockedSettings([&] { m_app.loadTranslations(); }),
             "Startup translation loading blocked GUI events on cs_args");
    QVERIFY(m_app.createOptionsModel(/*resetSettings=*/true));
    QScopedPointer<const NetworkStyle> style(NetworkStyle::instantiate(Params().GetChainType()));
    m_app.setupPlatformStyle();
    m_app.createWindow(style.data());
    connect(&m_app, &BitcoinApplication::windowShown, this, &AppTests::guiTests);
    expectCallback("guiTests");
    bool initialized{false};
    const bool responsive_init = WithBlockedSettings([&] { initialized = m_app.baseInitialize(); });
    QVERIFY(initialized);
    QVERIFY2(responsive_init, "Base initialization blocked the GUI while waiting for backend settings");
    m_app.requestInitialize();
    m_app.exec();
    const bool responsive_shutdown = WithBlockedSettings([&] {
        m_app.requestShutdown();
        m_app.exec();
    });
    QVERIFY2(responsive_shutdown, "Node interruption blocked the GUI while waiting for backend settings");

    // Reset global state to avoid interfering with later tests.
    LogInstance().DisconnectTestLogger();
}

//! Entry point for BitcoinGUI tests.
void AppTests::guiTests(BitcoinGUI* window)
{
    HandleCallback callback{"guiTests", *this};
    CheckBoundedQtLogging();
    CheckQtInfoLogging();
    CheckNetworkStyleImages();
    CheckNonblockingTranslations();
    CheckNonblockingIntro();
    CheckNonblockingSplash(m_app.node());
    CheckCoreMessageLifetime(m_app.node());
    CheckLazyHelp(window);
    CheckWaitSurvivesCallerDeletion();
    TestP2CIcon(window);
    TestNotificationDialogs(window);
#ifdef ENABLE_WALLET
    TestAutomaticClaimsNavigation(window);
    TestMiningWithoutWallet(window);
#endif
    connect(window, &BitcoinGUI::consoleShown, this, &AppTests::consoleTests);
    expectCallback("consoleTests");
    QAction* action = window->findChild<QAction*>("openRPCConsoleAction");
    action->activate(QAction::Trigger);
}

//! Entry point for RPCConsole tests.
void AppTests::consoleTests(RPCConsole* console)
{
    HandleCallback callback{"consoleTests", *this};
    TestRpcCommand(console);
    CheckNonblockingRPCConsole(console);
}

//! Destructor to shut down after the last expected callback completes.
AppTests::HandleCallback::~HandleCallback()
{
    auto& callbacks = m_app_tests.m_callbacks;
    auto it = callbacks.find(m_callback);
    assert(it != callbacks.end());
    callbacks.erase(it);
    if (callbacks.empty()) {
        m_app_tests.m_app.exit(0);
    }
}
