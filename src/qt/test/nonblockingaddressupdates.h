// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGADDRESSUPDATES_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGADDRESSUPDATES_H

#include <qt/addresstablemodel.h>
#include <qt/walletmodel.h>
#include <interfaces/node.h>
#include <key.h>
#include <key_io.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QEvent>
#include <QPersistentModelIndex>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTest>
#include <QTimer>

inline void CheckBoundedAddressUpdates(WalletModel& source, const PlatformStyle* style)
{
    auto wallet = std::make_shared<wallet::CWallet>(&source.wallet().wallet()->chain(),
        "bounded address notifications", wallet::CreateMockableWalletDatabase());
    wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
    auto model = std::make_unique<WalletModel>(interfaces::MakeWallet(*source.node().walletLoader().context(), wallet),
        source.clientModel(), style);
    const auto cleanup = qScopeGuard([&] { WalletModel::destroy(model.release()); });
    auto* addresses = model->getAddressTableModel();
    QSignalSpy initial_reset{addresses, &QAbstractItemModel::modelReset};
    QTRY_COMPARE(initial_reset.count(), 1);

    model->refresh(/*pk_hash_only=*/true);
    auto* signing_addresses = model->getAddressTableModel();
    QSignalSpy signing_reset{signing_addresses, &QAbstractItemModel::modelReset};
    QVERIFY(signing_addresses != addresses);
    for (int repeat{0}; repeat < 10; ++repeat) {
        model->refresh(/*pk_hash_only=*/true);
        QCOMPARE(model->getAddressTableModel(), signing_addresses);
    }
    model->refresh(/*pk_hash_only=*/false);
    QCOMPARE(model->getAddressTableModel(), addresses);
    QCOMPARE(model->findChildren<AddressTableModel*>(QString{}, Qt::FindDirectChildrenOnly).size(), 2);
    QTRY_COMPARE(signing_reset.count(), 1);
    QSignalSpy signing_labels{signing_addresses, &AddressTableModel::labelsChanged};

    struct Address {
        QString text;
        CTxDestination destination;
    };
    std::vector<Address> entries;
    for (int i{0}; i < 193; ++i) {
        const CTxDestination destination{PKHash{GenerateRandomKey().GetPubKey()}};
        entries.push_back({QString::fromStdString(EncodeDestination(destination)), destination});
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.text < b.text; });

    class MetaCallCounter final : public QObject {
    public:
        int calls{0};
        bool eventFilter(QObject*, QEvent* event) override
        {
            if (event->type() == QEvent::MetaCall) ++calls;
            return false;
        }
    } counter;
    QCoreApplication::sendPostedEvents(model.get(), QEvent::MetaCall);
    model->installEventFilter(&counter);
    // Repeated updates, removal and re-addition collapse to the final state.
    // The fixture emits notifications only; no wallet data is persisted.
    std::thread producer([&] {
        for (const auto& entry : entries) {
            wallet->NotifyAddressBookChanged(entry.destination, "old", false, wallet::AddressPurpose::SEND, CT_NEW);
            wallet->NotifyAddressBookChanged(entry.destination, "", false, wallet::AddressPurpose::SEND, CT_DELETED);
            wallet->NotifyAddressBookChanged(entry.destination, "final", false, wallet::AddressPurpose::REFUND, CT_NEW);
        }
        wallet->NotifyAddressBookChanged(entries.back().destination, "", false, wallet::AddressPurpose::REFUND, CT_DELETED);
    });
    producer.join();
    QCoreApplication::sendPostedEvents(model.get(), QEvent::MetaCall);
    QCOMPARE(counter.calls, 1);
    QCOMPARE(addresses->rowCount({}), 64);
    for (int turn{0}; turn < 3; ++turn) QCoreApplication::sendPostedEvents(model.get(), QEvent::MetaCall);
    QCOMPARE(counter.calls, 4);
    QCOMPARE(addresses->rowCount({}), 192);
    QCOMPARE(signing_addresses->rowCount({}), 192);
    QCOMPARE(signing_labels.count(), 193);
    for (size_t i{0}; i + 1 < entries.size(); ++i) {
        QCOMPARE(addresses->labelForAddress(entries[i].text), QString{"final"});
        QVERIFY(addresses->purposeForAddress(entries[i].text) == wallet::AddressPurpose::REFUND);
        QCOMPARE(signing_addresses->labelForAddress(entries[i].text), QString{"final"});
        QVERIFY(signing_addresses->purposeForAddress(entries[i].text) == wallet::AddressPurpose::REFUND);
    }
    QVERIFY(!addresses->purposeForAddress(entries.back().text));
    model->removeEventFilter(&counter);

    // Hold the snapshot worker while a second address model accumulates live
    // updates. Applying that snapshot must also replay at most 64 per turn.
    std::promise<void> release;
    auto released = release.get_future().share();
    bool release_sent{false};
    const auto release_on_failure = qScopeGuard([&] { if (!release_sent) release.set_value(); });
    auto blocked = model->requestWalletData([released](interfaces::Wallet&) {
        return released.wait_for(std::chrono::seconds{5}) == std::future_status::ready;
    });
    auto* replay = new AddressTableModel(model.get());
    auto* timer = replay->findChild<QTimer*>();
    QVERIFY(timer);
    // Deliver the queued timer start before manually driving its first result.
    QCoreApplication::sendPostedEvents(timer, QEvent::MetaCall);
    for (const auto& entry : entries) {
        replay->updateEntry(entry.text, "pending", false, wallet::AddressPurpose::SEND, CT_NEW);
        replay->updateEntry(entry.text, "latest pending", false, wallet::AddressPurpose::REFUND, CT_UPDATED);
    }
    QCOMPARE(replay->rowCount({}), 193);
    release.set_value();
    release_sent = true;
    auto snapshot_complete = model->requestWalletData([](interfaces::Wallet&) { return true; });
    // Keep GUI delivery controlled here: the backend barrier is already
    // released and bounded, so no timeout can strand an owned worker.
    QVERIFY(snapshot_complete.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    QVERIFY(snapshot_complete.get());
    QVERIFY(blocked.get());
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QCOMPARE(replay->rowCount({}), 64);

    const auto& newest = entries.back();
    const auto& deleted = entries[entries.size() - 2];
    QSignalSpy replay_labels{replay, &AddressTableModel::labelsChanged};
    replay->updateEntry(newest.text, "newer live update", false, wallet::AddressPurpose::RECEIVE, CT_UPDATED);
    replay->updateEntry(deleted.text, "", false, wallet::AddressPurpose::SEND, CT_DELETED);
    QCOMPARE(replay_labels.count(), 2);
    QCOMPARE(replay->rowCount({}), 65);
    for (int turn{0}; turn < 3; ++turn) QCoreApplication::sendPostedEvents(replay, QEvent::MetaCall);
    QCOMPARE(replay->rowCount({}), 192);
    QCOMPARE(replay->labelForAddress(newest.text), QString{"newer live update"});
    QVERIFY(replay->purposeForAddress(newest.text) == wallet::AddressPurpose::RECEIVE);
    QVERIFY(!replay->purposeForAddress(deleted.text));
    for (size_t i{0}; i + 2 < entries.size(); ++i) {
        QCOMPARE(replay->labelForAddress(entries[i].text), QString{"latest pending"});
        QVERIFY(replay->purposeForAddress(entries[i].text) == wallet::AddressPurpose::REFUND);
    }
}

inline void CheckAddressModelIndices(WalletModel& source, const PlatformStyle* style)
{
    auto wallet = std::make_shared<wallet::CWallet>(&source.wallet().wallet()->chain(),
        "stable address indices", wallet::CreateMockableWalletDatabase());
    wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
    auto model = std::make_unique<WalletModel>(interfaces::MakeWallet(*source.node().walletLoader().context(), wallet),
        source.clientModel(), style);
    const auto cleanup = qScopeGuard([&] { WalletModel::destroy(model.release()); });

    class CountingAddressModel final : public AddressTableModel {
    public:
        using AddressTableModel::AddressTableModel;
        mutable int data_calls{0};
        QVariant data(const QModelIndex& index, int role) const override
        {
            ++data_calls;
            return AddressTableModel::data(index, role);
        }
    };
    auto* addresses = new CountingAddressModel(model.get());
    QSignalSpy reset{addresses, &QAbstractItemModel::modelReset};
    QTRY_COMPARE(reset.count(), 1);
    QCOMPARE(addresses->lookupAddress("missing"), -1);
    QCOMPARE(addresses->data_calls, 0);

    std::vector<QString> entries;
    for (int i{0}; i < 513; ++i) {
        entries.push_back(QString::fromStdString(EncodeDestination(PKHash{GenerateRandomKey().GetPubKey()})));
    }
    std::sort(entries.begin(), entries.end());
    const auto& retained_address = entries[256];
    addresses->updateEntry(retained_address, "retained", true, wallet::AddressPurpose::RECEIVE, CT_NEW);
    QPersistentModelIndex retained{addresses->index(0, AddressTableModel::Address, {})};
    QPersistentModelIndex retained_label{addresses->index(0, AddressTableModel::Label, {})};
    QVERIFY(retained.internalPointer() == nullptr);

    // Grow/reallocate the backing QList on both sides of an existing index.
    // The selected address must retain its identity, label and edit policy.
    for (const auto& address : entries) {
        if (address != retained_address) addresses->updateEntry(address, "other", false, wallet::AddressPurpose::SEND, CT_NEW);
    }
    QCOMPARE(retained.row(), 256);
    QCOMPARE(retained.data(Qt::EditRole).toString(), retained_address);
    QCOMPARE(retained_label.data(Qt::EditRole).toString(), QString{"retained"});
    QVERIFY(!(addresses->flags(retained) & Qt::ItemIsEditable));
    QVERIFY(addresses->flags(retained_label) & Qt::ItemIsEditable);
    QVERIFY(!addresses->setData(retained_label, "retained", Qt::EditRole));
    QCOMPARE(addresses->getEditStatus(), AddressTableModel::NO_CHANGES);

    addresses->data_calls = 0;
    for (size_t i{0}; i < entries.size(); ++i) QCOMPARE(addresses->lookupAddress(entries[i]), static_cast<int>(i));
    QCOMPARE(addresses->lookupAddress(""), -1);
    QCOMPARE(addresses->lookupAddress("not an address"), -1);
    // Lookup uses the already sorted cache, not Qt's linear match()/data().
    QCOMPARE(addresses->data_calls, 0);
    QVERIFY(!addresses->index(0, -1, {}).isValid());
    QVERIFY(!addresses->index(0, 2, {}).isValid());
    QVERIFY(!addresses->index(0, 0, retained).isValid());
    QStandardItemModel foreign_model{1, 2};
    const QModelIndex foreign_index = foreign_model.index(0, 0);
    QVERIFY(!addresses->data(foreign_index, Qt::EditRole).isValid());
    QCOMPARE(addresses->flags(foreign_index), Qt::NoItemFlags);
    QVERIFY(!addresses->setData(foreign_index, "foreign", Qt::EditRole));

    for (const auto& address : entries) {
        if (address != retained_address) addresses->updateEntry(address, {}, false, wallet::AddressPurpose::SEND, CT_DELETED);
    }
    QCOMPARE(addresses->rowCount({}), 1);
    QCOMPARE(retained.row(), 0);
    QCOMPARE(retained.data(Qt::EditRole).toString(), retained_address);
    QCOMPARE(retained_label.data(Qt::EditRole).toString(), QString{"retained"});
    QCOMPARE(addresses->lookupAddress(retained_address), 0);
    addresses->updateEntry(retained_address, {}, true, wallet::AddressPurpose::RECEIVE, CT_DELETED);
    QVERIFY(!retained.isValid());
    QVERIFY(!retained_label.isValid());
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGADDRESSUPDATES_H
