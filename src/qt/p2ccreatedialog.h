// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_QT_P2CCREATEDIALOG_H
#define CONNECTCOIN_QT_P2CCREATEDIALOG_H

#include <primitives/transaction_identifier.h>

#include <QPointer>
#include <QWidget>

#include <memory>

class BitcoinAmountField;
class WalletModel;
namespace wallet { class P2CTransactionBatch; }
QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QMessageBox;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
QT_END_NAMESPACE

/** Create P2C bounties. Does not generate HTTPS traffic or connection proofs. */
class P2CCreateDialog : public QWidget
{
    Q_OBJECT
public:
    explicit P2CCreateDialog(QWidget* parent = nullptr);
    ~P2CCreateDialog();
    void setModel(WalletModel* model);

Q_SIGNALS:
    void coinsSent(const Txid& txid);
    void message(const QString& title, const QString& message, unsigned int style);

private Q_SLOTS:
    void prepare(bool checked = false);
    void updateWorkMode();
    void finishConfirmation(int result);

private:
    void showError(const QString& text);
    QPointer<WalletModel> m_model;
    QWidget* m_form;
    QLineEdit* m_domain;
    BitcoinAmountField* m_amount;
    QSpinBox* m_count;
    QComboBox* m_work_mode;
    QSpinBox* m_bits;
    QLineEdit* m_target;
    QComboBox* m_roots;
    QCheckBox* m_custom_fee;
    BitcoinAmountField* m_fee_rate;
    QPushButton* m_create;
    QLabel* m_status;
    QPlainTextEdit* m_receipt;
    QPointer<QMessageBox> m_confirmation;
    std::unique_ptr<wallet::P2CTransactionBatch> m_batch;
};

#endif // CONNECTCOIN_QT_P2CCREATEDIALOG_H
