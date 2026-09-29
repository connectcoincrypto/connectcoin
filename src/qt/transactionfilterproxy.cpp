// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/transactionfilterproxy.h>

#include <qt/transactiontablemodel.h>
#include <qt/transactionrecord.h>

#include <algorithm>
#include <cstdlib>
#include <optional>

TransactionFilterProxy::TransactionFilterProxy(QObject* parent)
    : QSortFilterProxyModel(parent)
{
}

bool TransactionFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (showInactive && typeFilter == ALL_TYPES && !dateFrom && !dateTo && m_search_string.isEmpty() && minAmount == 0) return true;
    QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);

    if (!showInactive && index.data(TransactionTableModel::StatusRole).toInt() == TransactionStatus::Conflicted)
        return false;

    if (typeFilter != ALL_TYPES) {
        int type = index.data(TransactionTableModel::TypeRole).toInt();
        if (!(TYPE(type) & typeFilter)) return false;
    }

    if (dateFrom || dateTo) {
        QDateTime datetime = index.data(TransactionTableModel::DateRole).toDateTime();
        if (dateFrom && datetime < *dateFrom) return false;
        if (dateTo && datetime > *dateTo) return false;
    }

    if (!m_search_string.isEmpty()) {
        if (!index.data(TransactionTableModel::AddressRole).toString().contains(m_search_string, Qt::CaseInsensitive) &&
            !index.data(TransactionTableModel::LabelRole).toString().contains(m_search_string, Qt::CaseInsensitive) &&
            !index.data(TransactionTableModel::TxHashRole).toString().contains(m_search_string, Qt::CaseInsensitive)) {
            return false;
        }
    }

    if (minAmount > 0) {
        qint64 amount = llabs(index.data(TransactionTableModel::AmountRole).toLongLong());
        if (amount < minAmount) return false;
    }

    return true;
}

void TransactionFilterProxy::setDateRange(const std::optional<QDateTime>& from, const std::optional<QDateTime>& to)
{
    if (dateFrom == from && dateTo == to) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif

    dateFrom = from;
    dateTo = to;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateFilter();
#endif
}

void TransactionFilterProxy::setSearchString(const QString &search_string)
{
    if (m_search_string == search_string) return;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif

    m_search_string = search_string;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateFilter();
#endif
}

void TransactionFilterProxy::setTypeFilter(quint32 modes)
{
    if (typeFilter == modes) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif

    this->typeFilter = modes;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateFilter();
#endif
}

void TransactionFilterProxy::setMinAmount(const CAmount& minimum)
{
    if (minAmount == minimum) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif

    this->minAmount = minimum;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateFilter();
#endif
}

void TransactionFilterProxy::setShowInactive(bool _showInactive)
{
    if (showInactive == _showInactive) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif

    this->showInactive = _showInactive;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateFilter();
#endif
}
