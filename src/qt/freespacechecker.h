// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_FREESPACECHECKER_H
#define CONNECTCOIN_QT_FREESPACECHECKER_H

#include <QObject>
#include <QString>
#include <QtGlobal>

/* Check free space using copied paths, without accessing an Intro widget from
   the worker thread. Intro coalesces requests and rejects obsolete replies. */
class FreespaceChecker : public QObject
{
    Q_OBJECT

public:
    enum Status {
        ST_OK,
        ST_ERROR
    };

public Q_SLOTS:
    void check(const QString& path);

Q_SIGNALS:
    void reply(const QString& path, int status, const QString& message, quint64 available);
};

#endif // CONNECTCOIN_QT_FREESPACECHECKER_H
