// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGRPCCONSOLE_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGRPCCONSOLE_H

#include <qt/rpcconsole.h>
#include <sync.h>
#include <validation.h>

#include <QLabel>
#include <QLineEdit>
#include <QAbstractProxyModel>
#include <QTableView>
#include <QTabWidget>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>

#include <chrono>
#include <future>

inline void CheckNonblockingRPCConsole(RPCConsole* console)
{
    auto* messages = console->findChild<QTextEdit*>("messagesWidget");
    auto* prompt = console->findChild<QLineEdit*>("lineEdit");
    auto* executing = console->findChild<QLabel*>("executingLabel");
    QVERIFY(messages && prompt && executing);
    auto* tabs = console->findChild<QTabWidget*>("tabWidget");
    auto* peer_view = console->findChild<QTableView*>("peerWidget");
    QVERIFY(tabs && peer_view);
    auto* peer_proxy = qobject_cast<QAbstractProxyModel*>(peer_view->model());
    QVERIFY(peer_proxy && peer_proxy->sourceModel());
    auto* peer_refresh = peer_proxy->sourceModel()->findChild<QTimer*>("peerRefreshTimer");
    auto* peer_results = peer_proxy->sourceModel()->findChild<QTimer*>("peerRefreshResultTimer");
    QVERIFY(peer_refresh && peer_results);
    const int original_tab = tabs->currentIndex();
    console->setTabFocus(RPCConsole::TabTypes::PEERS);
    QVERIFY(peer_refresh->isActive());
    console->setTabFocus(RPCConsole::TabTypes::CONSOLE);
    QVERIFY(!peer_refresh->isActive());
    QVERIFY(!peer_results->isActive());
    console->setTabFocus(RPCConsole::TabTypes::PEERS);
    QVERIFY(peer_refresh->isActive());
    // Both standalone (wallet enabled) and embedded node consoles must pause
    // the invisible model updates while their top-level window is minimized.
    auto* top_level = console->window();
    top_level->showMinimized();
    QTRY_VERIFY(!peer_refresh->isActive());
    QVERIFY(!peer_results->isActive());
    top_level->showNormal();
    QTRY_VERIFY(peer_refresh->isActive());
    console->hide();
    QVERIFY(!peer_refresh->isActive());
    QVERIFY(!peer_results->isActive());
    console->show();
    QTRY_VERIFY(peer_refresh->isActive());
    tabs->setCurrentIndex(original_tab);
    QVERIFY(executing->isHidden());
    QVERIFY(!messages->document()->isUndoRedoEnabled());
    const int original_size = messages->document()->defaultFont().pointSize();
    console->setFontSize(12);
    console->message(RPCConsole::CMD_REPLY, "font-selection-marker <literal> & unchanged");
    auto selection = messages->document()->find("font-selection-marker");
    QVERIFY(!selection.isNull());
    // A fixed HTML font size would defeat changing the document default.
    QVERIFY(!selection.charFormat().hasProperty(QTextFormat::FontPointSize));
    messages->setTextCursor(selection);
    const QString before = messages->toPlainText();
    console->fontBigger();
    QCOMPARE(messages->document()->defaultFont().pointSize(), 13);
    QCOMPARE(messages->toPlainText(), before);
    QCOMPARE(messages->textCursor().selectedText(), QString("font-selection-marker"));
    console->fontSmaller();
    QCOMPARE(messages->document()->defaultFont().pointSize(), 12);
    QCOMPARE(messages->toPlainText(), before);

    // Keep a real RPC in flight while clearing/reformatting its output. Its
    // completion must not undo the welcome text or a subsequently added entry.
    std::promise<void> acquired;
    auto locked = acquired.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    auto blocker = std::async(std::launch::async, [&] {
        LOCK(cs_main);
        acquired.set_value();
        return released.wait_for(std::chrono::seconds{3}) == std::future_status::timeout;
    });
    locked.wait();
    prompt->setText("getblockchaininfo");
    QTest::keyClick(prompt, Qt::Key_Return);
    const bool executing_before_clear = !executing->isHidden();
    console->clear();
    console->fontBigger();
    const bool executing_after_clear = !executing->isHidden();
    console->message(RPCConsole::MC_DEBUG, "retained-during-rpc");
    const QString retained = messages->toPlainText();
    int ticks{0};
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        if (++ticks == 5) release.set_value();
    });
    heartbeat.start(10);
    QTRY_VERIFY(executing->isHidden());
    heartbeat.stop();
    const bool timed_out = blocker.get();
    QVERIFY(!timed_out);
    QVERIFY(ticks >= 5);
    QVERIFY(executing_before_clear);
    QVERIFY(executing_after_clear);
    QVERIFY(messages->toPlainText().startsWith(retained));
    QVERIFY(messages->toPlainText().contains("\"chain\": \"regtest\""));
    console->setFontSize(original_size);

    // Worker-prepared history must still move a repeated command to the end
    // and preserve its neighbors. Only accepted commands update that history.
    const QString first{"getblockchaininfo()[chain]"};
    const QString second{"getblockchaininfo()[blocks]"};
    for (const auto& command : {first, second, first}) {
        prompt->setText(command);
        QTest::keyClick(prompt, Qt::Key_Return);
        QTRY_VERIFY(executing->isHidden());
    }
    console->browseHistory(-1);
    QCOMPARE(prompt->text(), first);
    console->browseHistory(-1);
    QCOMPARE(prompt->text(), second);
    console->browseHistory(-1);
    QCOMPARE(prompt->text(), QString{"getblockchaininfo"});
    prompt->clear();

    // Both worker paths now return complete HTML rows, not merely escaped
    // payloads. The command and reply must be escaped exactly once, retaining
    // literal markup, ampersands and non-ASCII text without nested tables or
    // treating user input as rich text.
    const QString literal{QString::fromUtf8("<b>rpc-worker-ol\xc3\xa1</b> & literal")};
    const QString command{QStringLiteral("echo \"") + literal + QStringLiteral("\"")};
    const QString before_echo = messages->toPlainText();
    prompt->setText(command);
    QTest::keyClick(prompt, Qt::Key_Return);
    QTRY_VERIFY(executing->isHidden());
    const QString echo_output = messages->toPlainText().mid(before_echo.size());
    QCOMPARE(echo_output.count(literal), 2);
    QVERIFY(echo_output.contains(command));
    QVERIFY(!echo_output.contains(QStringLiteral("&lt;b&gt;")));
    QVERIFY(!echo_output.contains(QStringLiteral("<table>")));
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGRPCCONSOLE_H
