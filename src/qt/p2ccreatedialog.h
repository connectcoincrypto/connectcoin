// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_QT_P2CCREATEDIALOG_H
#define CONNECTCOIN_QT_P2CCREATEDIALOG_H

#include <primitives/transaction_identifier.h>

#include <QPointer>
#include <QWidget>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class BitcoinAmountField;
class WalletModel;
class P2CClaimDialog;
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

/** P2C wallet page: bounty creation and a separate opt-in automatic claim tab. */
class P2CCreateDialog : public QWidget
{
    Q_OBJECT
public:
    using RsaProbe = std::function<bool(const std::string&, uint32_t, const std::function<bool()>&,
                                       std::chrono::steady_clock::time_point)>;
    explicit P2CCreateDialog(QWidget* parent = nullptr);
    ~P2CCreateDialog();
    void setModel(WalletModel* model);
    /** Test injection: all normal GUI tests use an in-memory probe, never DNS/TLS. */
    void setRsaProbeForTest(RsaProbe probe);

Q_SIGNALS:
    void coinsSent(const Txid& txid);
    void message(const QString& title, const QString& message, unsigned int style);

private Q_SLOTS:
    void prepare(bool checked = false);
    void updateWorkMode();
    void finishConfirmation(int result);

private:
    struct RsaProbeState;
    void startRsaProbe(const std::string& domain, uint32_t roots_version);
    void cancelRsaProbe();
    void freezeSignatureAlgorithms(const std::shared_ptr<RsaProbeState>& probe);
    void showError(const QString& text);
    QPointer<WalletModel> m_model;
    P2CClaimDialog* m_claim;
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
    RsaProbe m_rsa_probe;
    std::shared_ptr<RsaProbeState> m_probe;
    bool m_ready_to_send{false};
};

#endif // CONNECTCOIN_QT_P2CCREATEDIALOG_H
