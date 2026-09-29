// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_BANTABLEMODEL_H
#define CONNECTCOIN_QT_BANTABLEMODEL_H

#include <addrdb.h>
#include <net.h>
#include <util/threadpool.h>

#include <memory>

#include <QAbstractTableModel>
#include <QStringList>

class BanTablePriv;
class QTimer;

namespace interfaces {
    class Node;
}

struct CCombinedBan {
    CSubNet subnet;
    CBanEntry banEntry;
};

class BannedNodeLessThan
{
public:
    BannedNodeLessThan(int nColumn, Qt::SortOrder fOrder) :
        column(nColumn), order(fOrder) {}
    bool operator()(const CCombinedBan& left, const CCombinedBan& right) const;

private:
    int column;
    Qt::SortOrder order;
};

/**
   Qt model providing information about banned peers, similar to the
   "getpeerinfo" RPC call. Used by the rpc console UI.
 */
class BanTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit BanTableModel(interfaces::Node& node, QObject* parent);
    ~BanTableModel();
    void startAutoRefresh();
    void stopAutoRefresh();
    void stop();
    void interrupt();

    enum ColumnIndex {
        Address = 0,
        Bantime = 1
    };

    /** @name Methods overridden from QAbstractTableModel
        @{*/
    int rowCount(const QModelIndex &parent) const override;
    int columnCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QModelIndex index(int row, int column, const QModelIndex &parent) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    void sort(int column, Qt::SortOrder order) override;
    /*@}*/

    bool shouldShow();

    //! Queue an unban operation, returning whether the request was accepted.
    bool unban(const QModelIndex& index);

public Q_SLOTS:
    void refresh();

private:
    interfaces::Node& m_node;
    QStringList columns;
    std::unique_ptr<BanTablePriv> priv;
    ThreadPool m_worker{"qt-banlist"};
    std::future<QList<CCombinedBan>> m_query;
    std::vector<std::future<bool>> m_unban_queries;
    QTimer* m_result_timer{nullptr};
    bool m_refresh_requested{false};
    bool m_stopped{false};
    bool m_worker_stopped{false};
    void pollRefresh();
};

#endif // CONNECTCOIN_QT_BANTABLEMODEL_H
