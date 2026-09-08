// Copyright (c) 2026 The ConnectCoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_MININGPAGE_H
#define CONNECTCOIN_QT_MININGPAGE_H

#include <QWidget>

class ClientModel;
class WalletModel;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class MiningPage : public QWidget
{
    Q_OBJECT
public:
    explicit MiningPage(WalletModel* wallet_model, QWidget* parent = nullptr);
    void setClientModel(ClientModel* model);

private:
    void refresh();
    void start();
    void newAddress();
    QString walletAddress();
    void updateThreadWarning();
    WalletModel* m_wallet;
    ClientModel* m_client{nullptr};
    QLineEdit* m_address;
    QSpinBox* m_threads;
    QLabel* m_thread_warning;
    int m_logical_cpus{1};
    QPushButton* m_new_address;
    QPushButton* m_start;
    QPushButton* m_stop;
    QLabel* m_status;
};

#endif // CONNECTCOIN_QT_MININGPAGE_H
