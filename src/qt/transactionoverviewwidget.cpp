// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/transactionoverviewwidget.h>

#include <qt/transactiontablemodel.h>

#include <QListView>
#include <QSize>
#include <QSizePolicy>

#include <algorithm>

TransactionOverviewModel::TransactionOverviewModel(QObject* parent) : QAbstractProxyModel(parent) {}

void TransactionOverviewModel::setSourceModel(QAbstractItemModel* source_model)
{
    if (sourceModel() == source_model) return;
    beginResetModel();
    for (const auto& connection : m_source_connections) disconnect(connection);
    m_source_connections.clear();
    QAbstractProxyModel::setSourceModel(source_model);
    if (source_model) {
        const auto before = [this] { beginSourceChange(); };
        const auto after = [this] { endSourceChange(); };
        // Only five rows are exposed, so a source structural change has a
        // bounded reset cost, including when a row crosses the fifth boundary.
        m_source_connections << connect(source_model, &QAbstractItemModel::rowsAboutToBeInserted, this, before);
        m_source_connections << connect(source_model, &QAbstractItemModel::rowsInserted, this, after);
        m_source_connections << connect(source_model, &QAbstractItemModel::rowsAboutToBeRemoved, this, before);
        m_source_connections << connect(source_model, &QAbstractItemModel::rowsRemoved, this, after);
        m_source_connections << connect(source_model, &QAbstractItemModel::rowsAboutToBeMoved, this, before);
        m_source_connections << connect(source_model, &QAbstractItemModel::rowsMoved, this, after);
        m_source_connections << connect(source_model, &QAbstractItemModel::columnsAboutToBeInserted, this, before);
        m_source_connections << connect(source_model, &QAbstractItemModel::columnsInserted, this, after);
        m_source_connections << connect(source_model, &QAbstractItemModel::columnsAboutToBeRemoved, this, before);
        m_source_connections << connect(source_model, &QAbstractItemModel::columnsRemoved, this, after);
        m_source_connections << connect(source_model, &QAbstractItemModel::columnsAboutToBeMoved, this, before);
        m_source_connections << connect(source_model, &QAbstractItemModel::columnsMoved, this, after);
        m_source_connections << connect(source_model, &QAbstractItemModel::layoutAboutToBeChanged, this, before);
        m_source_connections << connect(source_model, &QAbstractItemModel::layoutChanged, this, after);
        m_source_connections << connect(source_model, &QAbstractItemModel::modelAboutToBeReset, this, before);
        m_source_connections << connect(source_model, &QAbstractItemModel::modelReset, this, after);
        m_source_connections << connect(source_model, &QObject::destroyed, this, [this] {
            // QAbstractProxyModel clears its source first, but emits no model
            // reset. Tell attached views that the previous window is gone.
            m_source_connections.clear();
            if (m_change_depth == 0) beginResetModel();
            m_change_depth = 0;
            endResetModel();
        });
        m_source_connections << connect(source_model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& first, const QModelIndex& last, const QList<int>& roles) {
                if (m_change_depth || first.parent().isValid() || first.row() >= rowCount()) return;
                const auto bottom = index(std::min(last.row(), rowCount() - 1), last.column());
                if (bottom.isValid()) Q_EMIT dataChanged(index(first.row(), first.column()), bottom, roles);
            });
        m_source_connections << connect(source_model, &QAbstractItemModel::headerDataChanged, this,
            [this](Qt::Orientation orientation, int first, int last) {
                if (orientation == Qt::Vertical) last = std::min(last, rowCount() - 1);
                if (first <= last) Q_EMIT headerDataChanged(orientation, first, last);
            });
    }
    endResetModel();
}

void TransactionOverviewModel::beginSourceChange()
{
    if (m_change_depth++ == 0) beginResetModel();
}

void TransactionOverviewModel::endSourceChange()
{
    if (--m_change_depth == 0) endResetModel();
}

QModelIndex TransactionOverviewModel::mapToSource(const QModelIndex& item) const
{
    if (!item.isValid() || item.model() != this || !sourceModel() || item.row() >= rowCount()) return {};
    return sourceModel()->index(item.row(), item.column());
}

QModelIndex TransactionOverviewModel::mapFromSource(const QModelIndex& item) const
{
    if (!item.isValid() || item.model() != sourceModel() || item.parent().isValid()) return {};
    return index(item.row(), item.column());
}

QModelIndex TransactionOverviewModel::index(int row, int column, const QModelIndex& parent) const
{
    return hasIndex(row, column, parent) ? createIndex(row, column) : QModelIndex{};
}

int TransactionOverviewModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() || !sourceModel() ? 0 : std::min(ROW_LIMIT, sourceModel()->rowCount());
}

int TransactionOverviewModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() || !sourceModel() ? 0 : sourceModel()->columnCount();
}

TransactionOverviewWidget::TransactionOverviewWidget(QWidget* parent)
    : QListView(parent) {}

QSize TransactionOverviewWidget::sizeHint() const
{
    return {sizeHintForColumn(TransactionTableModel::ToAddress), QListView::sizeHint().height()};
}

void TransactionOverviewWidget::showEvent(QShowEvent* event)
{
    Q_UNUSED(event);
    QSizePolicy sp = sizePolicy();
    sp.setHorizontalPolicy(QSizePolicy::Minimum);
    setSizePolicy(sp);
}
