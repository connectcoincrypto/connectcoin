// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGHISTORYLABELS_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGHISTORYLABELS_H

#include <interfaces/node.h>
#include <key_io.h>
#include <qt/addresstablemodel.h>
#include <qt/guiutil.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>
#include <tinyformat.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <memory>
#include <set>
#include <utility>

#include <QTest>

inline void CheckBoundedHistoryLabels(WalletModel& source, const PlatformStyle* style)
{
    const auto tip = GUIUtil::WaitForBackendTask(source.requestWalletData([](interfaces::Wallet& backend) {
        auto& wallet = *backend.wallet();
        LOCK(wallet.cs_wallet);
        return std::make_pair(wallet.GetLastBlockHeight(), wallet.GetLastBlockHash());
    }));
    auto wallet = std::make_shared<wallet::CWallet>(&source.wallet().wallet()->chain(),
        "bounded history labels", wallet::CreateMockableWalletDatabase());
    CKey first_key, second_key;
    first_key.MakeNewKey(true);
    second_key.MakeNewKey(true);
    QVERIFY(wallet::CreateDescriptor(*wallet, strprintf("rawtr(%s)", EncodeSecret(first_key)), true));
    QVERIFY(wallet::CreateDescriptor(*wallet, strprintf("rawtr(%s)", EncodeSecret(second_key)), true));
    const CTxDestination first_destination{WitnessV1Taproot{XOnlyPubKey{first_key.GetPubKey()}}};
    const CTxDestination second_destination{WitnessV1Taproot{XOnlyPubKey{second_key.GetPubKey()}}};
    const auto first_address = QString::fromStdString(EncodeDestination(first_destination));
    const auto second_address = QString::fromStdString(EncodeDestination(second_destination));
    constexpr int ROWS{320};
    {
        LOCK(wallet->cs_wallet);
        wallet->SetLastBlockProcessed(tip.first, tip.second);
        wallet->SetAddressBook(first_destination, "first", wallet::AddressPurpose::RECEIVE);
        wallet->SetAddressBook(second_destination, "second", wallet::AddressPurpose::RECEIVE);
        for (int row{0}; row < ROWS; ++row) {
            CMutableTransaction transaction;
            transaction.vin.emplace_back(Txid::FromUint256(uint256::ONE), row);
            transaction.vout.emplace_back(COIN, XOnlyPubKey{(row % 2 ? first_key : second_key).GetPubKey()});
            QVERIFY(wallet->AddToWallet(MakeTransactionRef(transaction), wallet::TxStateInactive{}));
        }
    }
    auto model = std::make_unique<WalletModel>(interfaces::MakeWallet(*source.node().walletLoader().context(), wallet),
        source.clientModel(), style);
    auto& history = *model->getTransactionTableModel();
    auto& addresses = *model->getAddressTableModel();
    QTRY_COMPARE(history.rowCount({}), ROWS);
    QTRY_COMPARE(addresses.rowCount({}), 2);

    QObject observer;
    std::set<QString> notified_hashes;
    int notified_rows{0};
    bool bounded{true};
    QObject::connect(&history, &QAbstractItemModel::dataChanged, &observer,
        [&](const QModelIndex& first, const QModelIndex& last, const QList<int>& roles) {
            if (!roles.contains(TransactionTableModel::LabelRole)) return;
            bounded &= last.row() - first.row() + 1 <= 128;
            for (int row{first.row()}; row <= last.row(); ++row) {
                notified_hashes.insert(history.index(row, 0).data(TransactionTableModel::TxHashRole).toString());
                ++notified_rows;
            }
        });

    // An event queued behind the first chunk must run before the remaining
    // history is examined or passed to its sorting/filtering proxies.
    history.updateAddressBookLabels({});
    int first_turn_rows{-1};
    QMetaObject::invokeMethod(&observer, [&] { first_turn_rows = notified_rows; }, Qt::QueuedConnection);
    QTRY_COMPARE(notified_hashes.size(), size_t{ROWS});
    QCOMPARE(first_turn_rows, 128);
    QCOMPARE(notified_rows, ROWS);
    QVERIFY(bounded);

    TransactionFilterProxy filtered;
    filtered.setSourceModel(&history);
    filtered.setDynamicSortFilter(true);
    filtered.setSearchString("renamed first");
    QCOMPARE(filtered.rowCount({}), 0);
    std::set<QString> expected_hashes;
    for (int row{0}; row < ROWS; ++row) {
        const auto item = history.index(row, 0);
        if (item.data(TransactionTableModel::AddressRole).toString() == first_address) {
            expected_hashes.insert(item.data(TransactionTableModel::TxHashRole).toString());
        }
    }
    QCOMPARE(expected_hashes.size(), size_t{ROWS / 2});
    notified_hashes.clear();
    notified_rows = 0;
    addresses.updateEntry(first_address, "renamed first", true, wallet::AddressPurpose::RECEIVE, CT_UPDATED);
    QTRY_COMPARE(notified_rows, ROWS / 2);
    QTRY_COMPARE(filtered.rowCount({}), ROWS / 2);
    QCOMPARE(notified_hashes, expected_hashes);
    QCOMPARE(notified_rows, ROWS / 2); // No unrelated rows between sparse matches.
    QVERIFY(bounded);

    // Removing rows on either side of the continuation cursor must not skip
    // surviving records. The first removed record was already notified.
    notified_hashes.clear();
    notified_rows = 0;
    history.updateAddressBookLabels({});
    bool removed{false};
    QMetaObject::invokeMethod(&observer, [&] {
        const auto before = history.index(0, 0).data(TransactionTableModel::TxHashRole).toString();
        const auto after = history.index(200, 0).data(TransactionTableModel::TxHashRole).toString();
        history.updateTransaction(before, CT_DELETED, true);
        history.updateTransaction(after, CT_DELETED, true);
        removed = true;
    }, Qt::QueuedConnection);
    QTRY_VERIFY(removed && notified_hashes.size() == size_t{ROWS - 1});
    QCOMPARE(history.rowCount({}), ROWS - 2);
    for (int row{0}; row < history.rowCount({}); ++row) {
        QVERIFY(notified_hashes.contains(history.index(row, 0).data(TransactionTableModel::TxHashRole).toString()));
    }
    QCOMPARE(notified_rows, ROWS - 1);

    // A different label can change after the scan has passed some of its
    // rows. Restarting with both addresses must refresh those earlier rows.
    filtered.setSearchString("both renamed");
    QCOMPARE(filtered.rowCount({}), 0);
    addresses.updateEntry(first_address, "both renamed", true, wallet::AddressPurpose::RECEIVE, CT_UPDATED);
    QMetaObject::invokeMethod(&observer, [&] {
        addresses.updateEntry(second_address, "both renamed", true, wallet::AddressPurpose::RECEIVE, CT_UPDATED);
    }, Qt::QueuedConnection);
    QTRY_COMPARE(filtered.rowCount({}), ROWS - 2);
    QVERIFY(bounded);
    filtered.setSourceModel(nullptr);
    WalletModel::destroy(model.release());
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGHISTORYLABELS_H
