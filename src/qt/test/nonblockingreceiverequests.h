// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGRECEIVEREQUESTS_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGRECEIVEREQUESTS_H

#include <interfaces/node.h>
#include <key_io.h>
#include <qt/guiutil.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/walletmodel.h>
#include <streams.h>
#include <util/string.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <chrono>
#include <array>
#include <future>
#include <memory>
#include <set>

#include <QPersistentModelIndex>
#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QProgressBar>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

inline void CheckNonblockingReceiveRemoval(WalletModel& source, const PlatformStyle* style)
{
    auto wallet = std::make_shared<wallet::CWallet>(&source.wallet().wallet()->chain(),
        "bulk receive request removal", wallet::CreateMockableWalletDatabase());
    wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
    auto model = std::make_unique<WalletModel>(interfaces::MakeWallet(*source.node().walletLoader().context(), wallet),
        source.clientModel(), style);
    const auto cleanup = qScopeGuard([&] { WalletModel::destroy(model.release()); });
    auto* requests = model->getRecentRequestsTableModel();
    QTRY_VERIFY(requests->isReady());

    const CTxDestination destination{PKHash{GenerateRandomKey().GetPubKey()}};
    const auto address = QString::fromStdString(EncodeDestination(destination));
    QList<RecentRequestEntry> entries;
    for (int id{1}; id <= 512; ++id) {
        RecentRequestEntry entry;
        entry.id = id;
        entry.date = QDateTime::fromSecsSinceEpoch(id);
        entry.recipient = SendCoinsRecipient{address,
            QString{"%1-%2"}.arg(id % 2).arg(id, 6, 10, QLatin1Char{'0'}), id, {}};
        entries.append(entry);
        requests->addNewRequest(entry);
    }
    QVERIFY(GUIUtil::WaitForBackendTask(model->requestWalletData([entries, destination](interfaces::Wallet& backend) {
        for (const auto& entry : entries) {
            DataStream stream;
            stream << entry;
            if (!backend.setAddressReceiveRequest(destination, util::ToString(entry.id), stream.str())) return false;
        }
        return true;
    })));
    // Selecting a different order before the initial read finishes must
    // prepare that order on the worker and publish the right first snapshot.
    auto* loading = new RecentRequestsTableModel(model.get());
    loading->sort(RecentRequestsTableModel::Label, Qt::AscendingOrder);
    QTRY_VERIFY(loading->isReady());
    QCOMPARE(loading->rowCount({}), 512);
    QCOMPARE(loading->entry(0).id, int64_t{2});
    delete loading;
    requests->sort(RecentRequestsTableModel::Amount, Qt::AscendingOrder);

    QSignalSpy removed{requests, &QAbstractItemModel::rowsRemoved};
    QSignalSpy reset{requests, &QAbstractItemModel::modelReset};
    QPersistentModelIndex survivor{requests->index(0, RecentRequestsTableModel::Amount)};
    QVERIFY(requests->removeRows(64, 64));
    QCOMPARE(requests->rowCount({}), 448);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(reset.count(), 0);
    QVERIFY(survivor.isValid());
    QCOMPARE(requests->entry(survivor.row()).id, int64_t{1});

    // These IDs are selected in amount order before another sort/add runs
    // during the responsive persistence wait. Rebase must preserve both the
    // new request and indexes belonging to surviving rows.
    std::set<int64_t> deleting;
    for (int row{64}; row < 192; ++row) deleting.insert(requests->entry(row).id);
    std::promise<void> release;
    auto released = release.get_future().share();
    auto blocker = model->requestWalletData([released](interfaces::Wallet&) {
        return released.wait_for(std::chrono::seconds{3}) == std::future_status::timeout;
    });
    QTimer heartbeat;
    int ticks{0};
    bool sort_preserved_index{false};
    QObject::connect(&heartbeat, &QTimer::timeout, requests, [&] {
        if (++ticks != 5) return;
        // Sorting also uses this worker; release it before starting the
        // nested sort, while the outer removal wait is still unwinding.
        release.set_value();
        requests->sort(RecentRequestsTableModel::Label, Qt::AscendingOrder);
        sort_preserved_index = survivor.isValid() && requests->entry(survivor.row()).id == 1;
        RecentRequestEntry added;
        added.id = 1001;
        added.recipient = SendCoinsRecipient{address, "new request during deletion", 1001, {}};
        requests->addNewRequest(added);
        for (int row{0}; row < requests->rowCount({}); ++row) {
            if (requests->entry(row).id == 400) survivor = requests->index(row, RecentRequestsTableModel::Amount);
        }
    });
    heartbeat.start(10);
    removed.clear();
    const bool success = requests->removeRows(64, 128);
    heartbeat.stop();
    const bool timed_out = blocker.get();
    QVERIFY(!timed_out);
    QVERIFY(ticks >= 5);
    QVERIFY(success);
    QVERIFY(sort_preserved_index);
    QCOMPARE(requests->rowCount({}), 321);
    QCOMPARE(removed.count(), 2);
    QCOMPARE(reset.count(), 0);
    QVERIFY(survivor.isValid());
    QCOMPARE(requests->entry(survivor.row()).id, int64_t{400});
    bool retained_added{false};
    for (int row{0}; row < requests->rowCount({}); ++row) {
        const auto id = requests->entry(row).id;
        QVERIFY(!deleting.contains(id));
        retained_added |= id == 1001;
    }
    QVERIFY(retained_added);
    QCOMPARE(GUIUtil::WaitForBackendTask(model->requestWalletData([](interfaces::Wallet& backend) {
        return backend.getAddressReceiveRequests().size();
    })), size_t{320});

    // Make each of the three synchronous sort snapshots stale, deterministically.
    // After that, sort must return with one pending asynchronous retry instead
    // of falling back to a full sort on the GUI or extending the modal wait.
    class SortWaitEdits final : public QObject {
    public:
        WalletModel& model;
        RecentRequestsTableModel& requests;
        QString address;
        std::array<std::promise<void>, 4> releases;
        std::array<std::future<bool>, 4> blockers;
        std::array<bool, 4> sent{};
        int waits{0};
        SortWaitEdits(WalletModel& model_in, RecentRequestsTableModel& requests_in, QString address_in)
            : model(model_in), requests(requests_in), address(std::move(address_in)) { block(0); }
        ~SortWaitEdits() override {
            qApp->removeEventFilter(this);
            for (int i{0}; i < 4; ++i) if (!sent[i]) release(i);
        }
        void block(int index) {
            auto ready = releases[index].get_future().share();
            blockers[index] = model.requestWalletData([ready](interfaces::Wallet&) {
                return ready.wait_for(std::chrono::seconds{3}) == std::future_status::timeout;
            });
        }
        void release(int index) { releases[index].set_value(); sent[index] = true; }
        bool eventFilter(QObject* object, QEvent* event) override {
            auto* dialog = qobject_cast<QDialog*>(object);
            if (waits >= 3 || event->type() != QEvent::Show || !dialog ||
                dialog->windowModality() != Qt::ApplicationModal || !dialog->findChild<QProgressBar*>()) return false;
            RecentRequestEntry entry;
            entry.id = 2000 + waits;
            entry.recipient = SendCoinsRecipient{address, "changed during sort", entry.id, {}};
            requests.addNewRequest(entry);
            block(waits + 1);
            release(waits++);
            return false;
        }
    } sort_edits{*model, *requests, address};
    qApp->installEventFilter(&sort_edits);
    requests->sort(RecentRequestsTableModel::Amount, Qt::AscendingOrder);
    qApp->removeEventFilter(&sort_edits);
    QCOMPARE(sort_edits.waits, 3);
    auto* sort_timer = requests->findChild<QTimer*>("receiveRequestSortRetry");
    QVERIFY(sort_timer);
    QVERIFY(sort_timer->isActive());
    RecentRequestEntry latest;
    latest.id = 3000;
    latest.recipient = SendCoinsRecipient{address, "latest asynchronous edit", 3000, {}};
    requests->addNewRequest(latest);
    sort_edits.release(3);
    QTRY_VERIFY(!sort_timer->isActive());
    for (auto& pending : sort_edits.blockers) QVERIFY(!pending.get());
    QCOMPARE(requests->entry(requests->rowCount({}) - 1).id, int64_t{3000});
    for (int row{1}; row < requests->rowCount({}); ++row) {
        QVERIFY(requests->entry(row - 1).recipient.amount <= requests->entry(row).recipient.amount);
    }
    QVERIFY(survivor.isValid());
    QCOMPARE(requests->entry(survivor.row()).id, int64_t{400});
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGRECEIVEREQUESTS_H
