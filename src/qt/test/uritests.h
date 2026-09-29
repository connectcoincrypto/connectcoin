// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_URITESTS_H
#define CONNECTCOIN_QT_TEST_URITESTS_H

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <QObject>
#include <QTest>

class URITests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void currencyUnits();
    void uriTests();
    void firstSelectedRow();
    void addressEntryValidation();
#ifdef ENABLE_WALLET
    void paymentServerStartup();
    void partialIpcRequests();
#endif
};

#endif // CONNECTCOIN_QT_TEST_URITESTS_H
