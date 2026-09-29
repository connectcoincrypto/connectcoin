// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TRANSACTIONOVERVIEWWIDGET_H
#define CONNECTCOIN_QT_TRANSACTIONOVERVIEWWIDGET_H

#include <QAbstractProxyModel>
#include <QListView>
#include <QSize>

QT_BEGIN_NAMESPACE
class QShowEvent;
class QWidget;
QT_END_NAMESPACE

//! A bounded window over an already sorted/filtered flat transaction model.
//! The overview must not lay out or hide every row in the full wallet history.
class TransactionOverviewModel : public QAbstractProxyModel
{
public:
    static constexpr int ROW_LIMIT{5};
    explicit TransactionOverviewModel(QObject* parent = nullptr);
    void setSourceModel(QAbstractItemModel* source_model) override;
    QModelIndex mapToSource(const QModelIndex& index) const override;
    QModelIndex mapFromSource(const QModelIndex& index) const override;
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex&) const override { return {}; }
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;

private:
    QList<QMetaObject::Connection> m_source_connections;
    int m_change_depth{0};
    void beginSourceChange();
    void endSourceChange();
};

class TransactionOverviewWidget : public QListView
{
    Q_OBJECT

public:
    explicit TransactionOverviewWidget(QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void showEvent(QShowEvent* event) override;
};

#endif // CONNECTCOIN_QT_TRANSACTIONOVERVIEWWIDGET_H
