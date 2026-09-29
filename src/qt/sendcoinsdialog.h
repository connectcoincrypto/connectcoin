// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_SENDCOINSDIALOG_H
#define CONNECTCOIN_QT_SENDCOINSDIALOG_H

#include <primitives/transaction_identifier.h>
#include <qt/clientmodel.h>
#include <qt/walletmodel.h>
#include <util/fees.h>

#include <cstdint>
#include <future>
#include <optional>
#include <vector>

#include <QDialog>
#include <QMessageBox>
#include <QPointer>
#include <QString>
#include <QTimer>

class PlatformStyle;
class SendCoinsEntry;
class SendCoinsRecipient;
enum class SynchronizationState;
namespace wallet {
class CCoinControl;
} // namespace wallet

namespace Ui {
    class SendCoinsDialog;
}

QT_BEGIN_NAMESPACE
class QShowEvent;
class QUrl;
QT_END_NAMESPACE

/** Dialog for sending ConnectCoin */
class SendCoinsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SendCoinsDialog(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~SendCoinsDialog();

    void setClientModel(ClientModel *clientModel);
    void setModel(WalletModel *model);

    /** Set up the tab chain manually, as Qt messes up the tab chain by default in some cases (issue https://bugreports.qt-project.org/browse/QTBUG-10907).
     */
    QWidget *setupTabChain(QWidget *prev);

    void setAddress(const QString &address);
    void pasteEntry(const SendCoinsRecipient &rv);
    bool handlePaymentRequest(const SendCoinsRecipient &recipient);

    // Only used for testing-purposes
    wallet::CCoinControl* getCoinControl() { return m_coin_control.get(); }

public Q_SLOTS:
    void clear();
    void reject() override;
    void accept() override;
    SendCoinsEntry *addEntry();
    void updateTabsAndLabels();
    void setBalance(const interfaces::WalletBalances& balances);

Q_SIGNALS:
    void coinsSent(const Txid& txid);

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    Ui::SendCoinsDialog *ui;
    QPointer<ClientModel> clientModel;
    QPointer<WalletModel> model;
    std::unique_ptr<wallet::CCoinControl> m_coin_control;
    std::unique_ptr<WalletModelTransaction> m_current_transaction;
    bool fNewRecipientAllowed{true};
    bool fFeeMinimized{true};
    const PlatformStyle *platformStyle;

    struct FeeEstimate {
        CAmount fee;
        std::optional<int> returned_target;
        FeeReason reason;
        int requested_target;
    };
    // Futures from the model's worker do not wait on destruction. Only copied
    // fee inputs/results cross threads; no worker captures this dialog.
    std::future<FeeEstimate> m_fee_estimate;
    std::optional<FeeEstimate> m_cached_fee_estimate;
    QTimer m_fee_refresh_timer;
    uint64_t m_fee_generation{0};
    uint64_t m_pending_fee_generation{0};
    bool m_fee_refresh_requested{false};
    bool m_fee_refresh_enabled{true};
    bool m_fee_block_refresh_pending{false};
    QPointer<QWidget> m_watched_window;
    std::vector<QMetaObject::Connection> m_model_connections;
    std::future<bool> m_change_query;
    QTimer m_change_debounce;
    QTimer m_change_poll;
    uint64_t m_change_generation{0};
    uint64_t m_pending_change_generation{0};
    QString m_pending_change_text;
    QPointer<WalletModel> m_pending_change_model;
    QPointer<QMessageBox> m_change_confirmation;
    bool m_change_requested{false};
    bool m_change_pending{false};
    bool m_send_in_progress{false};

    // Copy PSBT to clipboard and offer to save it.
    void presentPSBT(PartiallySignedTransaction psbt, const QList<SendCoinsRecipient>& recipients);
    // Process WalletModel::SendCoinsReturn and generate a pair consisting
    // of a message and message flags for use in Q_EMIT message().
    // Additional parameter msgArg can be used via .arg(msgArg).
    void processSendCoinsReturn(const WalletModel::SendCoinsReturn &sendCoinsReturn, const QString &msgArg = QString());
    void minimizeFeeSection(bool fMinimize);
    // Format confirmation message
    bool PrepareSendText(QString& question_string, QString& informative_text, QString& detailed_text);
    /* Sign PSBT using external signer.
     *
     * @param[in,out] psbtx the PSBT to sign
     * @param[out] transaction the finalized transaction, when complete
     * @param[in,out] complete whether the PSBT is complete (a successfully signed multisig transaction may not be complete)
     *
     * @returns false if any failure occurred, which may include the user rejection of a transaction on the device.
     */
    bool signWithExternalSigner(PartiallySignedTransaction& psbt, WalletModelTransaction& transaction, bool& complete);
    void updateFeeMinimizedLabel();
    void updateCoinControlState();
    void startFeeEstimate();
    void pollFeeEstimate();
    void renderSmartFeeLabel();
    void refreshVisibleBlockFee();
    void startChangeCheck();
    void pollChangeCheck();
    void updateSendButton();

private Q_SLOTS:
    void sendButtonClicked(bool checked);
    void on_buttonChooseFee_clicked();
    void on_buttonMinimizeFee_clicked();
    void removeEntry(SendCoinsEntry* entry);
    void useAvailableBalance(SendCoinsEntry* entry);
    void refreshBalance();
    void coinControlFeatureChanged(bool);
    void coinControlButtonClicked();
#if (QT_VERSION >= QT_VERSION_CHECK(6, 7, 0))
    void coinControlChangeChecked(Qt::CheckState);
#else
    void coinControlChangeChecked(int);
#endif
    void coinControlChangeEdited(const QString &);
    void coinControlUpdateLabels();
    void coinControlClipboardQuantity();
    void coinControlClipboardAmount();
    void coinControlClipboardFee();
    void coinControlClipboardAfterFee();
    void coinControlClipboardBytes();
    void coinControlClipboardChange();
    void updateFeeSectionControls();
    void updateNumberOfBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, SyncType synctype, SynchronizationState sync_state);
    void updateSmartFeeLabel();

Q_SIGNALS:
    // Fired when a message should be reported to the user
    void message(const QString &title, const QString &message, unsigned int style);
};


#define SEND_CONFIRM_DELAY   3

class SendConfirmationDialog : public QMessageBox
{
    Q_OBJECT

public:
    SendConfirmationDialog(const QString& title, const QString& text, const QString& informative_text = "", const QString& detailed_text = "", int secDelay = SEND_CONFIRM_DELAY, bool enable_send = true, bool always_show_unsigned = true, QWidget* parent = nullptr);
    /* Returns QMessageBox::Cancel, QMessageBox::Yes when "Send" is
       clicked and QMessageBox::Save when "Create Unsigned" is clicked. */
    int exec() override;

private Q_SLOTS:
    void countDown();
    void updateButtons();

private:
    QAbstractButton *yesButton;
    QAbstractButton *m_psbt_button;
    QTimer countDownTimer;
    int secDelay;
    QString confirmButtonText{tr("Send")};
    bool m_enable_send;
    QString m_psbt_button_text{tr("Create Unsigned")};
};

#endif // CONNECTCOIN_QT_SENDCOINSDIALOG_H
