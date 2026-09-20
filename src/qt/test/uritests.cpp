// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/uritests.h>

#include <consensus/amount.h>
#include <policy/feerate.h>
#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/walletmodel.h>

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QUrl>

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
