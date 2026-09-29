// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/uritests.h>

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <consensus/amount.h>
#include <policy/feerate.h>
#include <qt/bitcoinunits.h>
#include <qt/bitcoinaddressvalidator.h>
#include <qt/guiutil.h>
#include <qt/walletmodel.h>

#ifdef ENABLE_WALLET
#include <common/args.h>
#include <key_io.h>
#include <qt/paymentserver.h>

#include <atomic>
#include <functional>
#include <future>

#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QUuid>
#include <qscopeguard.h>
#endif

#include <QByteArray>
#include <QAbstractTableModel>
#include <QApplication>
#include <QClipboard>
#include <QItemSelectionModel>
#include <QTableView>
#include <QDataStream>
#include <QElapsedTimer>
#include <QIODevice>
#include <QUrl>

#include <vector>

void URITests::firstSelectedRow()
{
    class SelectionModel final : public QAbstractTableModel {
    public:
        mutable int index_calls{0};
        int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 100000; }
        int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 3; }
        QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override
        {
            ++index_calls;
            return QAbstractTableModel::index(row, column, parent);
        }
        QVariant data(const QModelIndex& index, int role) const override
        {
            if (!index.isValid() || (role != Qt::EditRole && role != Qt::DisplayRole)) return {};
            return QStringLiteral("%1:%2").arg(index.row()).arg(index.column());
        }
    } model;
    QTableView view;
    view.setModel(&model);
    auto* selection = view.selectionModel();
    selection->select(QItemSelection(model.index(0, 0), model.index(99999, 2)), QItemSelectionModel::Select);
    model.index_calls = 0;
    GUIUtil::copyEntryData(&view, 1);
    QCOMPARE(QApplication::clipboard()->text(), QString{"0:1"});
    QVERIFY(model.index_calls < 16); // Must not create an index for every row.
    model.index_calls = 0;
    QVERIFY(GUIUtil::hasEntryData(&view, 1, Qt::EditRole));
    QVERIFY(model.index_calls < 16);
    model.index_calls = 0;
    const auto first = GUIUtil::firstSelectedRow(&view, 2);
    QCOMPARE(first.row(), 0);
    QCOMPARE(first.column(), 2);
    QVERIFY(model.index_calls < 16);

    // A partial row must not be copied; split ranges covering a whole row
    // are valid, and disjoint full rows preserve selectedRows() ordering.
    selection->clear();
    selection->select(model.index(4, 1), QItemSelectionModel::Select);
    GUIUtil::setClipboard("unchanged");
    GUIUtil::copyEntryData(&view, 1);
    QCOMPARE(QApplication::clipboard()->text(), QString{"unchanged"});
    QVERIFY(!GUIUtil::hasEntryData(&view, 1, Qt::EditRole));
    QVERIFY(!GUIUtil::firstSelectedRow(&view).isValid());
    selection->select(model.index(4, 0), QItemSelectionModel::Select);
    selection->select(model.index(4, 2), QItemSelectionModel::Select);
    selection->select(QItemSelection(model.index(9, 0), model.index(9, 2)), QItemSelectionModel::Select);
    const auto expected = selection->selectedRows(2).front().data(Qt::EditRole).toString();
    GUIUtil::copyEntryData(&view, 2);
    QCOMPARE(QApplication::clipboard()->text(), expected);
    QVERIFY(GUIUtil::hasEntryData(&view, 2, Qt::EditRole));
    QVERIFY(!GUIUtil::hasEntryData(nullptr, 0, Qt::EditRole));
    QCOMPARE(GUIUtil::firstSelectedRow(&view, 2).data(Qt::EditRole).toString(), expected);
    QVERIFY(!GUIUtil::firstSelectedRow(nullptr).isValid());
}

void URITests::addressEntryValidation()
{
    BitcoinAddressEntryValidator validator{nullptr};
    int position{3};
    QString input;
    QCOMPARE(validator.validate(input, position), QValidator::Intermediate);
    input = QStringLiteral(" \t\n") + QChar{0x200B} + "cc1xyz" + QChar{0xFEFF} + " ";
    QCOMPARE(validator.validate(input, position), QValidator::Acceptable);
    QCOMPARE(input, QString{"cc1xyz"});
    QCOMPARE(position, 3);
    input = " a_I O ";
    QCOMPARE(validator.validate(input, position), QValidator::Invalid);
    QCOMPARE(input, QString{"a_IO"});
    input = QString(262144, QChar{' '}) + "cc1xyz";
    QCOMPARE(validator.validate(input, position), QValidator::Acceptable);
    QCOMPARE(input, QString{"cc1xyz"});
    input = " \t\n";
    QCOMPARE(validator.validate(input, position), QValidator::Acceptable);
    QVERIFY(input.isEmpty());
}

void URITests::currencyUnits()
{
    // Renaming the ticker must not change amounts or saved display-unit IDs.
    QCOMPARE(BitcoinUnits::shortName(BitcoinUnit::BTC), QString::fromStdString(CURRENCY_UNIT));
    const struct {
        BitcoinUnit unit;
        QString name;
        QString one_coin;
        int decimals;
        CAmount factor;
    } cases[]{
        {BitcoinUnit::BTC, "CONN", "1.0000000000 CONN", 10, COIN},
        {BitcoinUnit::mBTC, "mCONN", "1000.0000000 mCONN", 7, COIN / 1'000},
        {BitcoinUnit::uBTC, QString::fromUtf8("µCONN"), QString::fromUtf8("1000000.0000 µCONN"), 4, COIN / 1'000'000},
        {BitcoinUnit::SAT, "con", "10000000000 con", 0, 1},
    };
    char stored_id{0};
    for (const auto& test : cases) {
        QCOMPARE(BitcoinUnits::shortName(test.unit), test.name);
        QCOMPARE(BitcoinUnits::longName(test.unit), test.unit == BitcoinUnit::SAT ? QString("connect (con)") : test.name);
        QCOMPARE(BitcoinUnits::formatWithUnit(test.unit, COIN, false, BitcoinUnits::SeparatorStyle::NEVER), test.one_coin);
        QCOMPARE(BitcoinUnits::decimals(test.unit), test.decimals);
        QCOMPARE(BitcoinUnits::factor(test.unit), test.factor);
        CAmount parsed{0};
        QVERIFY(BitcoinUnits::parse(test.unit, BitcoinUnits::format(test.unit, COIN), &parsed));
        QCOMPARE(parsed, COIN);
        QByteArray serialized;
        QDataStream stream(&serialized, QIODevice::WriteOnly);
        stream << test.unit;
        QCOMPARE(serialized, QByteArray(1, stored_id++));
    }
}

void URITests::uriTests()
{
    SendCoinsRecipient rv;
    QUrl uri;
    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?req-dontexist="));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?dontexist="));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 0);

    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?label=Wikipedia Example Address"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.label == QString("Wikipedia Example Address"));
    QVERIFY(rv.amount == 0);

    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=0.001"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 10000000);

    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=1.001"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 10010000000LL);

    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=100&label=Wikipedia Example"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.amount == 1000000000000LL);
    QVERIFY(rv.label == QString("Wikipedia Example"));

    // Monetary boundaries use ConnectCoin's 10-decimal precision and 100-million-CONN limit.
    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=100000000.0000000000"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.amount == BitcoinUnits::maxMoney());

    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=100000000.0000000001"));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=-0.0000000001"));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?message=Wikipedia Example Address"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.label == QString());

    QVERIFY(GUIUtil::parseBitcoinURI("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?message=Wikipedia Example Address", &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.label == QString());

    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?req-message=Wikipedia Example Address"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));

    // Commas in amounts are not allowed.
    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=1,000&label=Wikipedia Example"));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=1,000.0&label=Wikipedia Example"));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    // There are two amount specifications. The last value wins.
    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=100&amount=200&label=Wikipedia Example"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.amount == 2000000000000LL);
    QVERIFY(rv.label == QString("Wikipedia Example"));

    // The first amount value is correct. However, the second amount value is not valid. Hence, the URI is not valid.
    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=100&amount=1,000&label=Wikipedia Example"));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    // Test label containing a question mark ('?').
    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=100&label=?"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.amount == 1000000000000LL);
    QVERIFY(rv.label == QString("?"));

    // Escape sequences are not supported.
    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=100&label=%3F"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.amount == 1000000000000LL);
    QVERIFY(rv.label == QString("%3F"));
}

#ifdef ENABLE_WALLET
namespace {
bool WithBlockedPaymentSettings(const std::function<void()>& operation,
                                const std::function<void()>& during_wait = {})
{
    std::promise<void> acquired;
    auto locked = acquired.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    std::atomic<bool> timed_out{false};
    auto blocker = std::async(std::launch::async, [&] {
        gArgs.LockSettings([&](common::Settings&) {
            acquired.set_value();
            // A regression must fail, not leave the test suite hanging.
            timed_out = released.wait_for(std::chrono::seconds{3}) != std::future_status::ready;
        });
    });
    locked.wait();
    bool timer_fired{false};
    QTimer heartbeat;
    heartbeat.setSingleShot(true);
    const auto cleanup = qScopeGuard([&] {
        heartbeat.stop();
        if (!timer_fired) release.set_value();
        blocker.get();
    });
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
        timer_fired = true;
        if (during_wait) during_wait();
        release.set_value();
    });
    heartbeat.start(50);
    operation();
    return timer_fired && !timed_out;
}
} // namespace

void URITests::paymentServerStartup()
{
    {
        PaymentServer payment_server{nullptr};
        QVERIFY(!payment_server.findChild<QLocalServer*>());
        QVERIFY2(WithBlockedPaymentSettings([&] { payment_server.startLocalServer(); }, [&] {
            // Responsive startup must not permit a second listener/worker.
            payment_server.startLocalServer();
        }), "IPC startup waited for cs_args on the GUI thread");
        auto* server = payment_server.findChild<QLocalServer*>();
        QVERIFY(server && server->isListening());
        QCOMPARE(payment_server.findChildren<QLocalServer*>().size(), 1);
        payment_server.uiReady();

        std::vector<SendCoinsRecipient> received;
        connect(&payment_server, &PaymentServer::receivedPaymentRequest, &payment_server,
                [&](const SendCoinsRecipient& recipient) { received.push_back(recipient); });
        QByteArray executable{"connectcoin-qt"};
        const auto address = QString::fromStdString(EncodeDestination(PKHash{}));
        QByteArray uri = (QStringLiteral("connectcoin:") + address + "?label=startup IPC").toUtf8();
        char* arguments[]{executable.data(), uri.data()};
        PaymentServer::ipcParseCommandLine(2, arguments);
        // Actual applications exit after successful forwarding. Consume the
        // saved command line silently so this fixture leaves no global URIs.
        const auto clear_pending = qScopeGuard([&] {
            const QSignalBlocker quiet{payment_server};
            payment_server.uiReady();
        });
        bool sent{false};
        QVERIFY2(WithBlockedPaymentSettings([&] { sent = PaymentServer::ipcSendCommandLine(); }),
                 "IPC sender resolved its datadir before entering the worker");
        QVERIFY(sent);
        QTRY_COMPARE_WITH_TIMEOUT(received.size(), size_t{1}, 1000);
        QCOMPARE(received.front().address, address);
        QCOMPARE(received.front().label, QStringLiteral("startup IPC"));
    }

    QPointer<PaymentServer> deleted = new PaymentServer(nullptr);
    QVERIFY2(WithBlockedPaymentSettings([&] { deleted->startLocalServer(); }, [&] {
        delete deleted.data();
    }), "Deleting the startup target stopped GUI event delivery");
    QVERIFY(deleted.isNull());
}

void URITests::partialIpcRequests()
{
    PaymentServer payment_server{nullptr};
    payment_server.startLocalServer();
    auto* server = payment_server.findChild<QLocalServer*>();
    QVERIFY(server);
    QVERIFY(server->isListening());
    // Rebind the test-owned server so no other process can send requests into
    // the fixture. No access to PaymentServer's private naming helper is needed.
    server->close();
    const QString name = QStringLiteral("ConnectCoinQt-uri-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(server->listen(name));
    payment_server.uiReady();
    std::vector<SendCoinsRecipient> received;
    connect(&payment_server, &PaymentServer::receivedPaymentRequest, &payment_server,
            [&](const SendCoinsRecipient& recipient) { received.push_back(recipient); });

    const QString address = QString::fromStdString(EncodeDestination(PKHash{}));
    const QString uri = QStringLiteral("connectcoin:") + address + QStringLiteral("?label=partial IPC&amount=1");
    QByteArray frame;
    QDataStream encoder(&frame, QIODevice::WriteOnly);
    encoder.setVersion(QDataStream::Qt_4_0);
    encoder << uri;
    QVERIFY(frame.size() > 12);

    QLocalSocket client;
    client.connectToServer(name);
    QTRY_COMPARE_WITH_TIMEOUT(client.state(), QLocalSocket::ConnectedState, 1000);
    int offset{0};
    // Split both the four-byte header and the body. Timers must continue to
    // run instead of a synchronous socket read blocking the GUI for a second.
    for (const int length : {2, 2, 8}) {
        bool timer_fired{false};
        QElapsedTimer elapsed;
        elapsed.start();
        QTimer heartbeat;
        heartbeat.setSingleShot(true);
        connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { timer_fired = true; });
        QCOMPARE(client.write(frame.mid(offset, length)), qint64(length));
        client.flush();
        heartbeat.start(25);
        QTRY_VERIFY_WITH_TIMEOUT(timer_fired, 750);
        QVERIFY2(elapsed.elapsed() < 750, "A partial IPC request blocked the GUI event loop");
        QCOMPARE(received.size(), size_t{0});
        offset += length;
    }
    // A second frame on the same connection must not deliver a duplicate.
    const QByteArray remainder = frame.mid(offset) + frame;
    QCOMPARE(client.write(remainder), qint64(remainder.size()));
    client.flush();
    QTRY_COMPARE_WITH_TIMEOUT(received.size(), size_t{1}, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(client.state(), QLocalSocket::UnconnectedState, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(server->findChildren<QLocalSocket*>().isEmpty(), 1000);
    QCOMPARE(received.front().address, address);
    QCOMPARE(received.front().label, QStringLiteral("partial IPC"));
    QCOMPARE(received.front().amount, COIN);

    // Reject an oversized allocation, an odd UTF-16 byte length, and Qt's null
    // string marker immediately, without waiting for their alleged payloads.
    for (const quint32 payload_length : {quint32{2 * 1024 * 1024}, quint32{3}, quint32{0xffffffffU}}) {
        QByteArray header;
        QDataStream stream(&header, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_4_0);
        stream << payload_length;
        QLocalSocket rejected;
        rejected.connectToServer(name);
        QTRY_COMPARE_WITH_TIMEOUT(rejected.state(), QLocalSocket::ConnectedState, 1000);
        QCOMPARE(rejected.write(header), qint64(header.size()));
        rejected.flush();
        QTRY_COMPARE_WITH_TIMEOUT(rejected.state(), QLocalSocket::UnconnectedState, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(server->findChildren<QLocalSocket*>().isEmpty(), 1000);
        QCOMPARE(received.size(), size_t{1});
    }

    // Disconnects with either a partial header or body must clean up the
    // accepted socket and must never pass an incomplete URI to the UI.
    for (const int bytes : {2, 12}) {
        QLocalSocket truncated;
        truncated.connectToServer(name);
        QTRY_COMPARE_WITH_TIMEOUT(truncated.state(), QLocalSocket::ConnectedState, 1000);
        QCOMPARE(truncated.write(frame.left(bytes)), qint64(bytes));
        truncated.flush();
        QTRY_VERIFY_WITH_TIMEOUT(server->findChild<QLocalSocket*>() != nullptr, 1000);
        truncated.disconnectFromServer();
        QTRY_COMPARE_WITH_TIMEOUT(truncated.state(), QLocalSocket::UnconnectedState, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(server->findChildren<QLocalSocket*>().isEmpty(), 1000);
        QCOMPARE(received.size(), size_t{1});
    }

    // readyRead can fire before the newConnection slot gets the socket. Hold
    // just that server signal until the entire frame has already been read.
    QLocalSocket prebuffered;
    {
        const QSignalBlocker hold_new_connections{server};
        prebuffered.connectToServer(name);
        QTRY_COMPARE_WITH_TIMEOUT(prebuffered.state(), QLocalSocket::ConnectedState, 1000);
        QCOMPARE(prebuffered.write(frame), qint64(frame.size()));
        prebuffered.flush();
        QTRY_VERIFY_WITH_TIMEOUT(server->hasPendingConnections(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(server->findChild<QLocalSocket*>() &&
                                    server->findChild<QLocalSocket*>()->bytesAvailable() == frame.size(), 1000);
    }
    QVERIFY(QMetaObject::invokeMethod(&payment_server, "handleURIConnection", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(received.size(), size_t{2}, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(prebuffered.state(), QLocalSocket::UnconnectedState, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(server->findChildren<QLocalSocket*>().isEmpty(), 1000);
}
#endif // ENABLE_WALLET
