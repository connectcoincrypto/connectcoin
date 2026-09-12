// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/apptests.h>

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <chainparams.h>
#include <key.h>
#include <logging.h>
#include <node/interface_ui.h>
#include <qt/bitcoin.h>
#include <qt/bitcoingui.h>
#include <qt/networkstyle.h>
#include <qt/platformstyle.h>
#include <qt/rpcconsole.h>
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
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QtGlobal>
#include <QtTest/QtTestWidgets>
#include <QtTest/QtTestGui>

namespace {
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
    QCOMPARE(mw_spy.count(), 4);
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
    m_app.parameterSetup();
    QVERIFY(m_app.createOptionsModel(/*resetSettings=*/true));
    QScopedPointer<const NetworkStyle> style(NetworkStyle::instantiate(Params().GetChainType()));
    m_app.setupPlatformStyle();
    m_app.createWindow(style.data());
    connect(&m_app, &BitcoinApplication::windowShown, this, &AppTests::guiTests);
    expectCallback("guiTests");
    m_app.baseInitialize();
    m_app.requestInitialize();
    m_app.exec();
    m_app.requestShutdown();
    m_app.exec();

    // Reset global state to avoid interfering with later tests.
    LogInstance().DisconnectTestLogger();
}

//! Entry point for BitcoinGUI tests.
void AppTests::guiTests(BitcoinGUI* window)
{
    HandleCallback callback{"guiTests", *this};
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
