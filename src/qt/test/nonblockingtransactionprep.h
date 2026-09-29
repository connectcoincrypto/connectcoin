// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGTRANSACTIONPREP_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGTRANSACTIONPREP_H

#include <consensus/consensus.h>
#include <key_io.h>
#include <policy/policy.h>
#include <qt/guiutil.h>
#include <qt/walletmodel.h>
#include <qt/walletmodeltransaction.h>
#include <script/solver.h>
#include <wallet/coincontrol.h>
#include <wallet/wallet.h>

#include <chrono>
#include <future>
#include <limits>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include <QTest>
#include <QTimer>

// Exercise preparation only: none of these transactions is committed or
// broadcast, and explicit existing change avoids consuming keypool entries.
inline void CheckNonblockingTransactionPreparation(WalletModel& model, wallet::CWallet& wallet)
{
    static_assert(std::is_same_v<decltype(std::declval<const WalletModelTransaction&>().getWtx()), const CTransactionRef&>);
    using SelectedCoin = std::tuple<CTxDestination, COutPoint, CAmount>;
    auto selected = GUIUtil::WaitForBackendTask(model.requestWalletData([](interfaces::Wallet& backend) -> std::optional<SelectedCoin> {
        for (const auto& [destination, coins] : backend.listCoins()) {
            for (const auto& [outpoint, coin] : coins) {
                if (!coin.is_spent && coin.depth_in_main_chain > COINBASE_MATURITY && coin.txout.nValue > 3 * COIN) {
                    return SelectedCoin{destination, outpoint, coin.txout.nValue};
                }
            }
        }
        return std::nullopt;
    }));
    QVERIFY(selected.has_value());
    const auto& [change_destination, outpoint, input_amount] = *selected;
    wallet::CCoinControl control;
    control.Select(outpoint);
    control.m_allow_other_inputs = false;
    control.destChange = change_destination;
    control.m_feerate = CFeeRate{1000};

    CKey first_key, second_key;
    first_key.MakeNewKey(true);
    second_key.MakeNewKey(true);
    const CTxDestination first_destination{WitnessV1Taproot{XOnlyPubKey{first_key.GetPubKey()}}};
    const CTxDestination second_destination{WitnessV1Taproot{XOnlyPubKey{second_key.GetPubKey()}}};
    const QString first_address = QString::fromStdString(EncodeDestination(first_destination));
    const QString second_address = QString::fromStdString(EncodeDestination(second_destination));
    const SendCoinsRecipient recipient{first_address, "preparation fixture", COIN, "no broadcast"};

    // Selected-coin balance and creation both need cs_wallet. Releasing it
    // from a GUI timer proves preparation waits without blocking that thread.
    std::promise<void> locked, release;
    auto ready = locked.get_future();
    auto released = release.get_future();
    auto holder = std::async(std::launch::async, [&] {
        LOCK(wallet.cs_wallet);
        locked.set_value();
        return released.wait_for(std::chrono::seconds{3}) == std::future_status::timeout;
    });
    ready.wait();
    int heartbeats{0};
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        if (++heartbeats == 5) release.set_value();
    });
    heartbeat.start(10);
    WalletModelTransaction normal{{recipient}};
    const auto prepared = model.prepareTransaction(normal, control);
    heartbeat.stop();
    const bool timed_out = holder.get();
    QVERIFY(!timed_out);
    QVERIFY(heartbeats >= 5);
    QCOMPARE(prepared.status, WalletModel::OK);
    QVERIFY(normal.getWtx());
    QCOMPARE(normal.getWtx()->vin.size(), size_t{1});
    QVERIFY(normal.getWtx()->vin.front().prevout == outpoint);
    QCOMPARE(normal.getTransactionSize(), static_cast<unsigned int>(GetVirtualTransactionSize(*normal.getWtx())));
    QVERIFY(normal.getTransactionFee() > 0);
    QCOMPARE(normal.getTotalTransactionAmount(), COIN);
    QCOMPARE(normal.getWtx()->GetValueOut() + normal.getTransactionFee(), input_amount);
    bool paid_recipient{false};
    for (const auto& output : normal.getWtx()->vout) {
        if (output.scriptPubKey == GetScriptForDestination(first_destination)) {
            QCOMPARE(output.nValue, COIN);
            paid_recipient = true;
        }
    }
    QVERIFY(paid_recipient);

    const auto check_invalid = [&](const QList<SendCoinsRecipient>& recipients, WalletModel::StatusCode expected) {
        WalletModelTransaction transaction{recipients};
        // Failure must also invalidate the previous transaction's size cache.
        transaction.setWtx(normal.getWtx());
        QCOMPARE(model.prepareTransaction(transaction, control).status, expected);
        QVERIFY(!transaction.getWtx());
        QCOMPARE(transaction.getTransactionSize(), 0U);
    };
    auto invalid = recipient;
    invalid.address = "not a connectcoin address";
    check_invalid({invalid}, WalletModel::InvalidAddress);
    for (const CAmount amount : {CAmount{0}, CAmount{-1}, MAX_MONEY + 1, std::numeric_limits<CAmount>::max()}) {
        invalid = recipient;
        invalid.amount = amount;
        check_invalid({invalid}, WalletModel::InvalidAmount);
    }
    check_invalid({recipient, recipient}, WalletModel::DuplicateAddress);
    auto maximum = recipient;
    maximum.amount = MAX_MONEY;
    auto extra = recipient;
    extra.address = second_address;
    extra.amount = 1;
    check_invalid({maximum, extra}, WalletModel::InvalidAmount);
    invalid = recipient;
    invalid.amount = input_amount + 1;
    check_invalid({invalid}, WalletModel::AmountExceedsBalance);
    check_invalid({}, WalletModel::OK);

    // Only the flagged recipient pays the fee. Reassignment must skip the
    // randomly positioned change output and retain the unflagged amount.
    auto subtracting = recipient;
    subtracting.fSubtractFeeFromAmount = true;
    const SendCoinsRecipient fixed{second_address, "fixed amount", COIN / 2, {}};
    WalletModelTransaction subtract_fee{{subtracting, fixed}};
    QCOMPARE(model.prepareTransaction(subtract_fee, control).status, WalletModel::OK);
    QVERIFY(subtract_fee.getWtx());
    const auto adjusted = subtract_fee.getRecipients();
    QCOMPARE(adjusted.size(), 2);
    QCOMPARE(adjusted[0].address, first_address);
    QCOMPARE(adjusted[0].amount, COIN - subtract_fee.getTransactionFee());
    QCOMPARE(adjusted[1].address, second_address);
    QCOMPARE(adjusted[1].amount, COIN / 2);
    QCOMPARE(subtract_fee.getTotalTransactionAmount(), COIN + COIN / 2 - subtract_fee.getTransactionFee());
    QCOMPARE(subtract_fee.getWtx()->GetValueOut() + subtract_fee.getTransactionFee(), input_amount);
    QCOMPARE(subtract_fee.getTransactionSize(), static_cast<unsigned int>(GetVirtualTransactionSize(*subtract_fee.getWtx())));

    // Assignment/finalization must refresh cached vsize; copying or moving
    // the value object must preserve it without serializing on later reads.
    WalletModelTransaction cached{normal};
    QCOMPARE(cached.getTransactionSize(), normal.getTransactionSize());
    CMutableTransaction enlarged{*normal.getWtx()};
    enlarged.vout.push_back(enlarged.vout.front());
    cached.setWtx(MakeTransactionRef(std::move(enlarged)));
    QVERIFY(cached.getTransactionSize() > normal.getTransactionSize());
    QCOMPARE(cached.getTransactionSize(), static_cast<unsigned int>(GetVirtualTransactionSize(*cached.getWtx())));
    WalletModelTransaction moved{std::move(cached)};
    QCOMPARE(moved.getTransactionSize(), static_cast<unsigned int>(GetVirtualTransactionSize(*moved.getWtx())));
    moved.setWtx(nullptr);
    QVERIFY(!moved.getWtx());
    QCOMPARE(moved.getTransactionSize(), 0U);
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGTRANSACTIONPREP_H
