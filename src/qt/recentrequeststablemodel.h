// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_RECENTREQUESTSTABLEMODEL_H
#define CONNECTCOIN_QT_RECENTREQUESTSTABLEMODEL_H

#include <qt/sendcoinsrecipient.h>

#include <string>
#include <future>
#include <cstdint>
#include <map>

#include <QAbstractTableModel>
#include <QStringList>
#include <QDateTime>

class WalletModel;
class QTimer;

class RecentRequestEntry
{
public:
    RecentRequestEntry() = default;

    static constexpr int CURRENT_VERSION{1};
    int nVersion{RecentRequestEntry::CURRENT_VERSION};
    int64_t id{0};
    QDateTime date;
    SendCoinsRecipient recipient;

    SERIALIZE_METHODS(RecentRequestEntry, obj) {
        unsigned int date_timet;
        SER_WRITE(obj, date_timet = obj.date.toSecsSinceEpoch());
        READWRITE(obj.nVersion, obj.id, date_timet, obj.recipient);
        SER_READ(obj, obj.date = QDateTime::fromSecsSinceEpoch(date_timet));
    }
};

class RecentRequestEntryLessThan
{
public:
    RecentRequestEntryLessThan(int nColumn, Qt::SortOrder fOrder):
        column(nColumn), order(fOrder) {}
    bool operator()(const RecentRequestEntry& left, const RecentRequestEntry& right) const;

private:
    int column;
    Qt::SortOrder order;
};

/** Model for list of recently generated payment requests / connectcoin: URIs.
 * Part of wallet model.
 */
class RecentRequestsTableModel: public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit RecentRequestsTableModel(WalletModel *parent);
    ~RecentRequestsTableModel();

    enum ColumnIndex {
        Date = 0,
        Label = 1,
        Message = 2,
        Amount = 3,
        NUMBER_OF_COLUMNS
    };

    /** @name Methods overridden from QAbstractTableModel
        @{*/
    int rowCount(const QModelIndex &parent) const override;
    int columnCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    /*@}*/

    const RecentRequestEntry &entry(int row) const { return list[row]; }
    void addNewRequest(const SendCoinsRecipient &recipient);
    void addNewRequest(const std::string &recipient);
    void addNewRequest(RecentRequestEntry &recipient);
    bool isReady() const { return m_ready; }

Q_SIGNALS:
    void ready();

public Q_SLOTS:
    void updateDisplayUnit();

private:
    WalletModel *walletModel;
    QStringList columns;
    QList<RecentRequestEntry> list;
    int64_t nReceiveRequestsMaxId{0};
    struct Snapshot {
        QList<RecentRequestEntry> entries;
        int64_t max_id{0};
        int sort_column{Date};
        Qt::SortOrder sort_order{Qt::DescendingOrder};
    };
    std::future<Snapshot> m_snapshot;
    QTimer* m_snapshot_timer{nullptr};
    bool m_ready{false};
    uint64_t m_revision{0};
    uint64_t m_sort_request{0};
    struct SortSnapshot {
        QList<RecentRequestEntry> entries;
        std::map<int64_t, int> positions;
    };
    std::future<SortSnapshot> m_sort_query;
    QTimer* m_sort_timer{nullptr};
    uint64_t m_sort_revision{0};
    int m_sort_column{Date};
    Qt::SortOrder m_sort_order{Qt::DescendingOrder};
    void applySnapshot(Snapshot snapshot);
    void ensureReady();
    static SortSnapshot prepareSort(QList<RecentRequestEntry> entries, int column, Qt::SortOrder order);
    void applySort(SortSnapshot sorted);
    void startDeferredSort();
    void pollDeferredSort();

    /** Updates the column title to "Amount (DisplayUnit)" and emits headerDataChanged() signal for table headers to react. */
    void updateAmountColumnTitle();
    /** Gets title for amount column including current display unit if optionsModel reference available. */
    QString getAmountTitle();
};

#endif // CONNECTCOIN_QT_RECENTREQUESTSTABLEMODEL_H
