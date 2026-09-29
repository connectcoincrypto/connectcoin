// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGWALLETACTIONS_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGWALLETACTIONS_H

#include <key_io.h>
#include <qt/clientmodel.h>
#include <qt/psbtoperationsdialog.h>
#include <qt/signverifymessagedialog.h>
#include <qt/transactionview.h>
#include <qt/walletmodel.h>

#include <chrono>
#include <future>
#include <utility>
#include <vector>

#include <QDialog>
#include <QMetaObject>
#include <QPointer>
#include <QTest>
#include <QTableView>
#include <QTimer>

inline void CheckWalletActionLifetime(WalletModel& model, const PlatformStyle* style)
{
    // Put bounded blockers ahead of the action on every model worker. The
    // action must process the deletion event while waiting, then discard its
    // result instead of touching the deleted dialog or opening another one.
    const auto destroy_during_action = [](QWidget* dialog, auto queue, int workers, auto action) {
        QPointer<QWidget> guard{dialog};
        std::promise<void> release;
        auto released = release.get_future().share();
        std::vector<std::future<bool>> blockers;
        for (int i{0}; i < workers; ++i) {
            blockers.push_back(queue([released](auto&) {
                return released.wait_for(std::chrono::seconds{3}) == std::future_status::timeout;
            }));
        }
        QObject event_owner;
        QTimer::singleShot(0, &event_owner, [&] {
            delete guard.data();
            release.set_value();
        });
        action();
        const bool destroyed = guard.isNull();
        if (!destroyed) release.set_value();
        bool timed_out{false};
        for (auto& blocker : blockers) timed_out |= blocker.get();
        delete guard.data();
        return destroyed && !timed_out;
    };
    const auto queue_wallet = [&model](auto task) { return model.requestWalletData(std::move(task)); };
    const auto queue_node = [&model](auto task) { return model.clientModel().requestNodeData(std::move(task)); };

    CMutableTransaction transaction;
    transaction.vin.emplace_back();
    transaction.vout.emplace_back(1, CScript{} << OP_TRUE);

    auto* loading = new PSBTOperationsDialog(nullptr, nullptr, &model.clientModel());
    QVERIFY(destroy_during_action(loading, queue_node, 2, [&] {
        loading->openWithPSBT(PartiallySignedTransaction{transaction});
    }));

    auto* copying = new PSBTOperationsDialog(nullptr, nullptr, &model.clientModel());
    copying->openWithPSBT(PartiallySignedTransaction{transaction});
    QVERIFY(destroy_during_action(copying, queue_node, 2, [&] { copying->copyToClipboard(); }));

    auto* signing = new SignVerifyMessageDialog(style, nullptr);
    signing->setModel(&model);
    signing->setAddress_SM(QString::fromStdString(EncodeDestination(PKHash{})));
    bool invoked{false};
    // Signing first refreshes encryption status in requestUnlock(). That
    // responsive wait can delete the caller before the signing request starts.
    QVERIFY(destroy_during_action(signing, queue_wallet, 1, [&] {
        invoked = QMetaObject::invokeMethod(signing, "on_signMessageButton_SM_clicked", Qt::DirectConnection);
    }));
    QVERIFY(invoked);

    // A history view can also be removed while its explicit detail/context
    // queries wait on the wallet. Neither continuation may use the old view.
    for (const bool context_menu : {false, true}) {
        auto* history = new TransactionView(style);
        history->setModel(&model);
        history->resize(800, 500);
        history->show();
        auto* table = history->findChild<QTableView*>();
        QVERIFY(table);
        QTRY_VERIFY(table->model()->rowCount() > 0);
        table->selectRow(0);
        const QPoint position = table->visualRect(table->model()->index(0, 0)).center();
        QVERIFY(table->indexAt(position).isValid());
        invoked = false;
        QVERIFY(destroy_during_action(history, queue_wallet, 1, [&] {
            invoked = context_menu
                ? QMetaObject::invokeMethod(history, "contextualMenu", Qt::DirectConnection, Q_ARG(QPoint, position))
                : QMetaObject::invokeMethod(history, "showDetails", Qt::DirectConnection);
        }));
        QVERIFY(invoked);
    }
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGWALLETACTIONS_H
