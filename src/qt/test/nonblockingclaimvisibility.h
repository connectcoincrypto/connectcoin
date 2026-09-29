// Copyright (c) The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGCLAIMVISIBILITY_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGCLAIMVISIBILITY_H

#include <interfaces/node.h>
#include <qt/p2cclaimdialog.h>
#include <qt/walletmodel.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <chrono>
#include <future>
#include <memory>

#include <QLabel>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>

inline void CheckP2CClaimVisibility(WalletModel& source, const PlatformStyle* style)
{
    auto wallet = std::make_shared<wallet::CWallet>(&source.wallet().wallet()->chain(),
        "claim status visibility", wallet::CreateMockableWalletDatabase());
    wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
    auto model = std::make_unique<WalletModel>(interfaces::MakeWallet(*source.node().walletLoader().context(), wallet),
        source.clientModel(), style);
    const auto cleanup = qScopeGuard([&] { WalletModel::destroy(model.release()); });

    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* page = new P2CClaimDialog(&window);
    layout->addWidget(page);
    auto* timer = page->findChild<QTimer*>("p2cClaimRefreshTimer");
    auto* status = page->findChild<QLabel*>("p2cClaimStatus");
    auto* stop = page->findChild<QPushButton*>("p2cClaimStop");
    QVERIFY(timer && status && stop);
    page->setModel(model.get());
    QVERIFY(!timer->isActive());
    window.show();
    QTRY_VERIFY(timer->isActive());
    QTRY_VERIFY(status->text().contains("Active rate: Disabled (0)"));

    page->hide();
    QVERIFY(!timer->isActive());
    page->show();
    QTRY_VERIFY(timer->isActive());
    // Child pages must observe their top-level window, not just their own
    // visibility: Windows may keep the child marked visible while minimized.
    window.showMinimized();
    QTRY_VERIFY(!timer->isActive());
    window.showNormal();
    QTRY_VERIFY(timer->isActive());
    window.hide();
    QTRY_VERIFY(!timer->isActive());
    window.show();
    QTRY_VERIFY(timer->isActive());

    // An already submitted configuration still completes while invisible.
    // This fixture only disables claiming: no DNS/TLS/network work is started.
    std::promise<void> release;
    auto released = release.get_future().share();
    bool sent{false};
    const auto unblock = qScopeGuard([&] { if (!sent) release.set_value(); });
    auto blocker = model->requestWalletData([released](interfaces::Wallet&) {
        return released.wait_for(std::chrono::seconds{5}) == std::future_status::timeout;
    });
    stop->click();
    QVERIFY(!stop->isEnabled());
    page->hide();
    QVERIFY(timer->isActive());
    QSignalSpy ticks{timer, &QTimer::timeout};
    QTRY_VERIFY(!ticks.empty());
    QVERIFY(!stop->isEnabled());
    release.set_value();
    sent = true;
    QTRY_VERIFY(stop->isEnabled());
    QVERIFY(!blocker.get());
    QVERIFY(!timer->isActive());

    page->show();
    QTRY_VERIFY(timer->isActive());
    page->setModel(nullptr);
    QVERIFY(!timer->isActive());
    QVERIFY(!page->isEnabled());
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGCLAIMVISIBILITY_H
