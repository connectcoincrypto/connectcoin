// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_MACDOCKICONHANDLER_H
#define CONNECTCOIN_QT_MACDOCKICONHANDLER_H

#include <QObject>

/** Application-wide macOS Dock icon handler, shared by all windows.
 */
class MacDockIconHandler : public QObject
{
    Q_OBJECT

public:
    static MacDockIconHandler *instance();
    /** Release the shared handler at application shutdown, not window close. */
    static void cleanup();

Q_SIGNALS:
    void dockIconClicked();

private:
    MacDockIconHandler();
};

#endif // CONNECTCOIN_QT_MACDOCKICONHANDLER_H
