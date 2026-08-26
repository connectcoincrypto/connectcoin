// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/uritests.h>

#include <qt/guiutil.h>
#include <qt/walletmodel.h>

#include <QUrl>

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
    QVERIFY(rv.amount == 100000);

    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=1.001"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 100100000);

    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=100&label=Wikipedia Example"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.amount == 10000000000LL);
    QVERIFY(rv.label == QString("Wikipedia Example"));

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
    QVERIFY(rv.amount == 20000000000LL);
    QVERIFY(rv.label == QString("Wikipedia Example"));

    // The first amount value is correct. However, the second amount value is not valid. Hence, the URI is not valid.
    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=100&amount=1,000&label=Wikipedia Example"));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    // Test label containing a question mark ('?').
    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=100&label=?"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.amount == 10000000000LL);
    QVERIFY(rv.label == QString("?"));

    // Escape sequences are not supported.
    uri.setUrl(QString("connectcoin:CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF?amount=100&label=%3F"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("CNYn5rwCC4QeGuBVFhRnESsB8Y52RTR2uF"));
    QVERIFY(rv.amount == 10000000000LL);
    QVERIFY(rv.label == QString("%3F"));
}
