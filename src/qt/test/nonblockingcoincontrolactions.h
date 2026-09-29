// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGCOINCONTROLACTIONS_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGCOINCONTROLACTIONS_H

#include <qt/coincontroldialog.h>
#include <qt/guiutil.h>
#include <qt/walletmodel.h>
#include <wallet/coincontrol.h>

#include <chrono>
#include <future>

#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

inline void CheckCoinControlActionDelivery(WalletModel& model, const PlatformStyle* style)
{
    wallet::CCoinControl control;
    {
        CoinControlDialog dialog(control, &model, style);
        auto* tree = dialog.findChild<QTreeWidget*>("treeWidget");
        QVERIFY(tree);
        QTRY_VERIFY(tree->topLevelItemCount() > 0);
        QTest::qWait(250); // Consume initial snapshot/statistics delivery.
        control.UnSelectAll();
        auto* group = new QTreeWidgetItem(tree);
        {
            const QSignalBlocker blocked{tree};
            group->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            group->setCheckState(0, Qt::Unchecked);
            for (int output{0}; output < 256; ++output) {
                auto* item = new QTreeWidgetItem(group);
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
                item->setData(3, Qt::UserRole, QString(64, QLatin1Char{'1'}));
                item->setData(3, Qt::UserRole + 1, output);
                item->setCheckState(0, Qt::Unchecked);
            }
        }
        class MetaCalls final : public QObject {
        public:
            int count{0};
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::MetaCall) ++count;
                return false;
            }
        } calls;
        dialog.installEventFilter(&calls);
        group->setCheckState(0, Qt::Checked);
        QCOMPARE(control.ListSelected().size(), size_t{256});
        QCoreApplication::sendPostedEvents(&dialog, QEvent::MetaCall);
        QCOMPARE(calls.count, 1);
        dialog.removeEventFilter(&calls);
    }
    control.UnSelectAll();

    // A coin-control page can be removed while its lock/unlock action waits
    // on the wallet. Neither the scoped cleanup nor the menu continuation may
    // write through the deleted page after the backend finishes.
    for (const bool unlocking : {false, true}) {
        auto* dialog = new CoinControlDialog(control, &model, style);
        QPointer<CoinControlDialog> guard{dialog};
        auto* tree = dialog->findChild<QTreeWidget*>("treeWidget");
        QVERIFY(tree);
        dialog->resize(1000, 650);
        dialog->show();
        QTRY_VERIFY(tree->topLevelItemCount() > 0);
        tree->expandAll();
        QTreeWidgetItem* leaf{nullptr};
        for (QTreeWidgetItemIterator item(tree); *item; ++item) {
            if (Txid::FromHex((*item)->data(3, Qt::UserRole).toString().toStdString())) { leaf = *item; break; }
        }
        QVERIFY(leaf);
        const COutPoint outpoint{Txid::FromHex(leaf->data(3, Qt::UserRole).toString().toStdString()).value(),
                                 leaf->data(3, Qt::UserRole + 1).toUInt()};
        if (unlocking) QVERIFY(GUIUtil::WaitForBackendTask(model.requestWalletData([outpoint](interfaces::Wallet& wallet) {
            return wallet.lockCoin(outpoint, true);
        })));
        tree->scrollToItem(leaf);
        const QPoint point = tree->visualItemRect(leaf).center();
        QVERIFY(tree->itemAt(point) == leaf);

        std::promise<void> release;
        auto released = release.get_future().share();
        auto blocker = model.requestWalletData([released](interfaces::Wallet&) {
            return released.wait_for(std::chrono::seconds{3}) == std::future_status::timeout;
        });
        QObject timer_owner;
        bool released_once{false}, action_invoked{false};
        QTimer::singleShot(0, &timer_owner, [&] {
            QTimer::singleShot(0, &timer_owner, [&] {
                delete guard.data();
                release.set_value();
                released_once = true;
            });
            action_invoked = QMetaObject::invokeMethod(dialog, unlocking ? "unlockCoin" : "lockCoin", Qt::DirectConnection);
        });
        const bool menu_invoked = QMetaObject::invokeMethod(dialog, "showMenu", Qt::DirectConnection, Q_ARG(QPoint, point));
        if (!released_once) { release.set_value(); released_once = true; }
        const bool timed_out = blocker.get();
        const bool destroyed = guard.isNull();
        delete guard.data();
        GUIUtil::WaitForBackendTask(model.requestWalletData([outpoint](interfaces::Wallet& wallet) { wallet.unlockCoin(outpoint); }));
        QVERIFY(menu_invoked);
        QVERIFY(action_invoked);
        QVERIFY(destroyed);
        QVERIFY(!timed_out);
    }
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGCOINCONTROLACTIONS_H
