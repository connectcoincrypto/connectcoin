// Copyright (c) The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGSENDVISIBILITY_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGSENDVISIBILITY_H

#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/optionsmodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/walletmodel.h>
#include <validation.h>
#include <wallet/coincontrol.h>

#include <chrono>

#include <QComboBox>
#include <QDateTime>
#include <QLabel>
#include <QRadioButton>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>

inline void CheckSendFeeVisibility(WalletModel& model, const PlatformStyle* style)
{
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* page = new SendCoinsDialog(style, &window);
    page->setWindowFlags(Qt::Widget);
    layout->addWidget(page);
    page->setModel(&model);
    page->setClientModel(&model.clientModel());
    auto* custom_radio = page->findChild<QRadioButton*>("radioCustomFee");
    auto* smart_radio = page->findChild<QRadioButton*>("radioSmartFee");
    auto* custom_fee = page->findChild<BitcoinAmountField*>("customFee");
    auto* target = page->findChild<QComboBox*>("confTargetSelector");
    QVERIFY(custom_radio && smart_radio && custom_fee && target);
    const bool original_custom = custom_radio->isChecked();
    const CAmount original_fee = custom_fee->value();
    const int original_target = target->currentIndex();
    const auto restore = qScopeGuard([&] {
        // SendCoinsDialog persists these preferences in its destructor.
        const QSignalBlocker block_fee{custom_fee}, block_target{target};
        page->getCoinControl()->UnSelectAll();
        (original_custom ? custom_radio : smart_radio)->setChecked(true);
        custom_fee->setValue(original_fee);
        target->setCurrentIndex(original_target);
    });
    auto* timer = page->findChild<QTimer*>("sendFeeRefreshTimer");
    QVERIFY(timer);
    QTRY_VERIFY(!timer->isActive());
    const auto block = [&] {
        Q_EMIT model.clientModel().numBlocksChanged(0, QDateTime{}, 1.0,
            SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    };

    // Hidden-page block bursts do not query the wallet or update the labels.
    // One deferred refresh is picked up on the next actual display.
    for (int i = 0; i < 100; ++i) block();
    QVERIFY(!timer->isActive());
    window.show();
    QVERIFY(timer->isActive());
    QSignalSpy polls{timer, &QTimer::timeout};
    QTRY_VERIFY(!timer->isActive());
    QVERIFY(!polls.empty());

    page->hide();
    block();
    QVERIFY(!timer->isActive());
    page->show();
    QVERIFY(timer->isActive());
    QTRY_VERIFY(!timer->isActive());
    window.showMinimized();
    block();
    QVERIFY(!timer->isActive());
    window.showNormal();
    QTRY_VERIFY(timer->isActive());
    QTRY_VERIFY(!timer->isActive());

    // An explicit target/user refresh is still allowed while hidden. It is
    // only automatic per-block display work that is deferred.
    window.hide();
    QVERIFY(QMetaObject::invokeMethod(page, "updateSmartFeeLabel"));
    QVERIFY(timer->isActive());
    QTRY_VERIFY(!timer->isActive());

    // The small fee-only worker snapshot must retain exactly the original
    // fixed-1000-byte estimate semantics. Selected inputs and external data
    // are irrelevant, and the sending dialog's explicit custom rate must not
    // override its simultaneously displayed smart fee.
    custom_radio->setChecked(true);
    custom_fee->setValue(123456);
    auto* control = page->getCoinControl();
    control->fOverrideFeeRate = true;
    for (uint32_t row = 0; row < 1024; ++row) control->Select({Txid::FromUint256(uint256::ONE), row});
    const auto selected = control->ListSelected();
    auto* fee_label = page->findChild<QLabel*>("labelSmartFee");
    QVERIFY(target && fee_label);
    for (const auto mode : {FeeEstimateMode::UNSET, FeeEstimateMode::CONSERVATIVE, FeeEstimateMode::ECONOMICAL}) {
        control->m_fee_mode = mode;
        control->m_signal_bip125_rbf = mode != FeeEstimateMode::CONSERVATIVE;
        {
            const QSignalBlocker block_target{target};
            target->setCurrentIndex((target->currentIndex() + 1) % target->count());
        }
        QVERIFY(QMetaObject::invokeMethod(page, "updateSmartFeeLabel"));
        auto original_control = *control;
        original_control.m_feerate.reset();
        auto expected = model.requestWalletData([original_control](interfaces::Wallet& wallet) {
            return wallet.getMinimumFee(1000, original_control, nullptr, nullptr);
        });
        QTRY_VERIFY(expected.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
        const auto expected_label = QString("%1/kvB").arg(BitcoinUnits::formatWithUnit(
            model.getOptionsModel()->getDisplayUnit(), expected.get()));
        QTRY_VERIFY(!timer->isActive());
        QCOMPARE(fee_label->text(), expected_label);
        QVERIFY(control->ListSelected() == selected);
        QVERIFY(control->m_feerate.has_value());
        QCOMPARE(control->m_feerate->GetFeePerK(), CAmount{123456});
    }

    block();
    QVERIFY(!timer->isActive());
    page->setClientModel(nullptr);
    window.show();
    QVERIFY(!timer->isActive());
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGSENDVISIBILITY_H
