// Copyright (c) 2026 The ConnectCoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_MININGPAGE_H
#define CONNECTCOIN_QT_MININGPAGE_H

#include <QWidget>
#include <QPointer>

#include <string>

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
    friend class WalletTests;
    void refresh();
    void start();
    void newAddress();
    QString walletAddress();
    void updateThreadWarning();
    void updateDatasetStatus(const std::string& dataset);
    WalletModel* m_wallet;
    QPointer<ClientModel> m_client;
    bool m_mining_command_requested{false};
    QLineEdit* m_address;
    QSpinBox* m_threads;
    QLabel* m_thread_warning;
    int m_logical_cpus{1};
    QPushButton* m_new_address;
    QPushButton* m_start;
    QPushButton* m_stop;
    QLabel* m_status;
    QLabel* m_dataset_status;
    QLabel* m_dataset_warning;
};

#endif // CONNECTCOIN_QT_MININGPAGE_H
