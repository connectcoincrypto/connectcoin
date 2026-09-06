// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_QT_P2CCLAIMDIALOG_H
#define CONNECTCOIN_QT_P2CCLAIMDIALOG_H

#include <QPointer>
#include <QWidget>
#include <future>
#include <string>

class WalletModel;
QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
QT_END_NAMESPACE

class P2CClaimDialog : public QWidget
{
    Q_OBJECT
public:
    explicit P2CClaimDialog(QWidget* parent = nullptr);
    void setModel(WalletModel* model);
private:
    void Configure(bool stop);
    void Refresh();
    QPointer<WalletModel> m_model;
    QSpinBox* m_rate;
    QCheckBox* m_unlimited;
    QSpinBox* m_concurrency;
    QLineEdit* m_domains;
    QPushButton* m_start;
    QPushButton* m_stop;
    QLabel* m_status;
    std::future<std::string> m_operation;
    QString m_configuration_error;
};
#endif // CONNECTCOIN_QT_P2CCLAIMDIALOG_H
