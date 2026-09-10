// Copyright (c) 2015-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/wallettests.h>
#include <qt/test/util.h>

#include <wallet/coincontrol.h>
#include <interfaces/chain.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/cpu_miner.h>
#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/miningpage.h>
#include <qt/csvmodelwriter.h>
#include <qt/optionsmodel.h>
#include <qt/overviewpage.h>
#include <qt/p2ccreatedialog.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/receivecoinsdialog.h>
#include <qt/receiverequestdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsentry.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/walletmodel.h>
#include <qt/walletframe.h>
#include <qt/walletview.h>
#include <script/solver.h>
#include <support/allocators/secure.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <validation.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QPixmap>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTimer>
#include <QTranslator>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QListView>
#include <QDialogButtonBox>
#include <QXmlStreamReader>

using wallet::AddWallet;
using wallet::CWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::RemoveWallet;
using wallet::WALLET_FLAG_AVOID_REUSE;
using wallet::WALLET_FLAG_DESCRIPTORS;
using wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS;
using wallet::WalletBatch;
using wallet::WalletContext;
using wallet::WalletDescriptor;
using wallet::WalletRescanReserver;

namespace
{
CTxDestination TestDestination(const CKey& key)
{
    return WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}};
}

CTxDestination ExternalTestDestination()
{
    static const CKey key{GenerateRandomKey()};
    return TestDestination(key);
}

//! Press "Yes" or "Cancel" buttons in modal send confirmation dialog.
void ConfirmSend(QString* text = nullptr, QMessageBox::StandardButton confirm_type = QMessageBox::Yes)
{
    QTimer::singleShot(0, [text, confirm_type]() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->inherits("SendConfirmationDialog")) {
                SendConfirmationDialog* dialog = qobject_cast<SendConfirmationDialog*>(widget);
                if (text) *text = dialog->text();
                QAbstractButton* button = dialog->button(confirm_type);
                button->setEnabled(true);
                button->click();
            }
        }
    });
}

//! Send coins to address and return txid.
Txid SendCoins(CWallet& wallet, SendCoinsDialog& sendCoinsDialog, const CTxDestination& address, CAmount amount,
                  QMessageBox::StandardButton confirm_type = QMessageBox::Yes)
{
    QVBoxLayout* entries = sendCoinsDialog.findChild<QVBoxLayout*>("entries");
    SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    entry->findChild<QValidatedLineEdit*>("payTo")->setText(QString::fromStdString(EncodeDestination(address)));
    entry->findChild<BitcoinAmountField*>("payAmount")->setValue(amount);
    Txid txid;
    btcsignals::scoped_connection c(wallet.NotifyTransactionChanged.connect([&txid](const Txid& hash, ChangeType status) {
        if (status == CT_NEW) txid = hash;
    }));
    ConfirmSend(/*text=*/nullptr, confirm_type);
    bool invoked = QMetaObject::invokeMethod(&sendCoinsDialog, "sendButtonClicked", Q_ARG(bool, false));
    assert(invoked);
    return txid;
}

//! Find index of txid in transaction list.
QModelIndex FindTx(const QAbstractItemModel& model, const Txid& txid)
{
    QString hash = QString::fromStdString(txid.ToString());
    int rows = model.rowCount({});
    for (int row = 0; row < rows; ++row) {
        QModelIndex index = model.index(row, 0, {});
        if (model.data(index, TransactionTableModel::TxHashRole) == hash) {
            return index;
        }
    }
    return {};
}

//! Invoke bumpfee on txid and check results.
void BumpFee(TransactionView& view, const Txid& txid, bool expectDisabled, std::string expectError, bool cancel)
{
    QTableView* table = view.findChild<QTableView*>("transactionView");
    QModelIndex index = FindTx(*table->selectionModel()->model(), txid);
    QVERIFY2(index.isValid(), "Could not find BumpFee txid");

    // Select row in table, invoke context menu, and make sure bumpfee action is
    // enabled or disabled as expected.
    QAction* action = view.findChild<QAction*>("bumpFeeAction");
    table->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    action->setEnabled(expectDisabled);
    table->customContextMenuRequested({});
    QCOMPARE(action->isEnabled(), !expectDisabled);

    action->setEnabled(true);
    QString text;
    if (expectError.empty()) {
        ConfirmSend(&text, cancel ? QMessageBox::Cancel : QMessageBox::Yes);
    } else {
        ConfirmMessage(&text, 0ms);
    }
    action->trigger();
    QVERIFY(text.indexOf(QString::fromStdString(expectError)) != -1);
}

void CompareBalance(WalletModel& walletModel, CAmount expected_balance, QLabel* balance_label_to_check)
{
    BitcoinUnit unit = walletModel.getOptionsModel()->getDisplayUnit();
    QString balanceComparison = BitcoinUnits::formatWithUnit(unit, expected_balance, false, BitcoinUnits::SeparatorStyle::ALWAYS);
    QCOMPARE(balance_label_to_check->text().trimmed(), balanceComparison);
}

// Verify the 'useAvailableBalance' functionality. With and without manually selected coins.
// Case 1: No coin control selected coins.
// 'useAvailableBalance' should fill the amount edit box with the total available balance
// Case 2: With coin control selected coins.
// 'useAvailableBalance' should fill the amount edit box with the sum of the selected coins values.
void VerifyUseAvailableBalance(SendCoinsDialog& sendCoinsDialog, const WalletModel& walletModel)
{
    // Verify first entry amount and "useAvailableBalance" button
    QVBoxLayout* entries = sendCoinsDialog.findChild<QVBoxLayout*>("entries");
    QVERIFY(entries->count() == 1); // only one entry
    SendCoinsEntry* send_entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    QVERIFY(send_entry->getValue().amount == 0);
    // Now click "useAvailableBalance", check updated balance (the entire wallet balance should be set)
    Q_EMIT send_entry->useAvailableBalance(send_entry);
    QVERIFY(send_entry->getValue().amount == walletModel.getCachedBalance().balance);

    // Now manually select two coins and click on "useAvailableBalance". Then check updated balance
    // (only the sum of the selected coins should be set).
    int COINS_TO_SELECT = 2;
    auto coins = walletModel.wallet().listCoins();
    CAmount sum_selected_coins = 0;
    int selected = 0;
    QVERIFY(coins.size() == 1); // context check, coins received only on one destination
    for (const auto& [outpoint, tx_out] : coins.begin()->second) {
        sendCoinsDialog.getCoinControl()->Select(outpoint);
        sum_selected_coins += tx_out.txout.nValue;
        if (++selected == COINS_TO_SELECT) break;
    }
    QVERIFY(selected == COINS_TO_SELECT);

    // Now that we have 2 coins selected, "useAvailableBalance" should update the balance label only with
    // the sum of them.
    Q_EMIT send_entry->useAvailableBalance(send_entry);
    QVERIFY(send_entry->getValue().amount == sum_selected_coins);
}

void SyncUpWallet(const std::shared_ptr<CWallet>& wallet, interfaces::Node& node)
{
    WalletRescanReserver reserver(*wallet);
    reserver.reserve();
    CWallet::ScanResult result = wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, /*start_height=*/0, /*max_height=*/{}, reserver, /*save_progress=*/false);
    QCOMPARE(result.status, CWallet::ScanResult::SUCCESS);
    QCOMPARE(result.last_scanned_block, WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    QVERIFY(result.last_failed_block.IsNull());
}

class FailingP2CTestDatabase : public wallet::MockableSQLiteDatabase
{
public:
    // Fail once after this many successful GUI-thread batch creations; -1
    // disables injection. Background wallet callbacks must not consume it.
    std::atomic<int> batches_until_failure{-1};
    std::unique_ptr<wallet::DatabaseBatch> MakeBatch() override
    {
        if (std::this_thread::get_id() == m_gui_thread && batches_until_failure.load() >= 0 && batches_until_failure.fetch_sub(1) == 0) {
            throw std::runtime_error("Injected P2C database failure");
        }
        return wallet::MockableSQLiteDatabase::MakeBatch();
    }

private:
    const std::thread::id m_gui_thread{std::this_thread::get_id()};
};

std::shared_ptr<CWallet> SetupDescriptorsWallet(interfaces::Node& node, TestChain100Setup& test, bool watch_only = false, std::unique_ptr<wallet::WalletDatabase> database = {})
{
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", database ? std::move(database) : CreateMockableWalletDatabase());
    LOCK(wallet->cs_wallet);
    wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    if (watch_only) {
        wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    } else {
        wallet->SetupDescriptorScriptPubKeyMans();
    }

    // Add the coinbase key
    FlatSigningProvider provider;
    std::string error;
    std::string key_str;
    if (watch_only) {
        key_str = HexStr(XOnlyPubKey{test.coinbaseKey.GetPubKey()});
    } else {
        key_str = EncodeSecret(test.coinbaseKey);
    }
    auto descs = Parse(std::string{"rawtr"} + "(" + key_str + ")", provider, error, /* require_checksum=*/false);
    assert(!descs.empty());
    assert(descs.size() == 1);
    auto& desc = descs.at(0);
    WalletDescriptor w_desc(std::move(desc), 0, 0, 1, 1);
    Assert(wallet->AddWalletDescriptor(w_desc, provider, "", false));
    const CTxDestination dest{TestDestination(test.coinbaseKey)};
    wallet->SetAddressBook(dest, "", wallet::AddressPurpose::RECEIVE);
    {
        LOCK(node.context()->chainman->GetMutex());
        const auto* tip{node.context()->chainman->ActiveChain().Tip()};
        wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
    }
    SyncUpWallet(wallet, node);
    wallet->SetBroadcastTransactions(true);
    return wallet;
}

struct MiniGUI {
public:
    SendCoinsDialog sendCoinsDialog;
    TransactionView transactionView;
    OptionsModel optionsModel;
    std::unique_ptr<ClientModel> clientModel;
    std::unique_ptr<WalletModel> walletModel;

    MiniGUI(interfaces::Node& node, const PlatformStyle* platformStyle) : sendCoinsDialog(platformStyle), transactionView(platformStyle), optionsModel(node) {
        bilingual_str error;
        QVERIFY(optionsModel.Init(error));
        clientModel = std::make_unique<ClientModel>(node, &optionsModel);
    }

    void initModelForWallet(interfaces::Node& node, const std::shared_ptr<CWallet>& wallet, const PlatformStyle* platformStyle, bool unload = true)
    {
        WalletContext& context = *node.walletLoader().context();
        AddWallet(context, wallet);
        walletModel = std::make_unique<WalletModel>(interfaces::MakeWallet(context, wallet), *clientModel, platformStyle);
        if (unload) RemoveWallet(context, wallet, /* load_on_start= */ std::nullopt);
        sendCoinsDialog.setModel(walletModel.get());
        transactionView.setModel(walletModel.get());
    }

};

//! Simple qt wallet tests.
//
// Test widgets can be debugged interactively calling show() on them and
// manually running the event loop, e.g.:
//
//     sendCoinsDialog.show();
//     QEventLoop().exec();
//
// This also requires overriding the default minimal Qt platform:
//
//     QT_QPA_PLATFORM=xcb     build/bin/connectcoin-test-qt  # Linux
//     QT_QPA_PLATFORM=windows build/bin/connectcoin-test-qt  # Windows
//     QT_QPA_PLATFORM=cocoa   build/bin/connectcoin-test-qt  # macOS
void TestGUI(interfaces::Node& node, const std::shared_ptr<CWallet>& wallet)
{
    // The default amount-field step is a fraction of a coin, not a fixed
    // number of atomic units. This guards the 10-decimal denomination.
    BitcoinAmountField amount_field;
    amount_field.setValue(0);
    auto* amount_spin_box = amount_field.findChild<QAbstractSpinBox*>();
    QVERIFY(amount_spin_box);
    QTest::keyClick(amount_spin_box, Qt::Key_Up);
    QCOMPARE(amount_field.value(), COIN / 1000);

    // Create widgets for sending coins and listing transactions.
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(node, platformStyle.get());
    mini_gui.initModelForWallet(node, wallet, platformStyle.get());
    WalletModel& walletModel = *mini_gui.walletModel;
    SendCoinsDialog& sendCoinsDialog = mini_gui.sendCoinsDialog;
    TransactionView& transactionView = mini_gui.transactionView;

    // Closing the last wallet must preserve the selected node mining controls.
    // Reopening a wallet keeps Mining selected, and closing all views destroys
    // their polling timers before the client model can be torn down.
    {
        WalletFrame frame{platformStyle.get(), nullptr};
        frame.setClientModel(mini_gui.clientModel.get());
        auto* fallback = frame.findChild<MiningPage*>("walletlessMiningPage");
        auto* stack = frame.findChild<QStackedWidget*>();
        QVERIFY(fallback);
        QVERIFY(stack);
        frame.gotoMiningPage();
        QCOMPARE(stack->currentWidget(), fallback);
        QVERIFY(fallback->findChild<QPushButton*>("startMining")->isEnabled());
        QVERIFY(!fallback->findChild<QPushButton*>("newMiningAddress")->isEnabled());
        for (const bool close_all : {false, true}) {
            auto* view = new WalletView(&walletModel, platformStyle.get(), &frame);
            QVERIFY(frame.addView(view));
            frame.setCurrentWallet(&walletModel);
            QCOMPARE(frame.currentWalletView(), view);
            QVERIFY(qobject_cast<MiningPage*>(view->currentWidget()));
            if (close_all) {
                frame.removeAllWallets();
            } else {
                frame.removeWallet(&walletModel);
            }
            QVERIFY(!frame.currentWalletView());
            QCOMPARE(stack->currentWidget(), fallback);
            QCOMPARE(frame.findChildren<WalletView*>().size(), 0);
            QCOMPARE(frame.findChildren<MiningPage*>().size(), 1);
            QVERIFY(fallback->findChild<QPushButton*>("startMining")->isEnabled());
            // A wallet can still emit keypool notifications after its view is
            // gone. All callbacks capturing the deleted receive page must
            // have been disconnected with that page.
            QVERIFY(QMetaObject::invokeMethod(&walletModel, "canGetAddressesChanged", Qt::DirectConnection));
        }
        frame.setClientModel(nullptr);
        QVERIFY(!fallback->findChild<QPushButton*>("startMining")->isEnabled());
        QVERIFY(!fallback->findChild<QPushButton*>("stopMining")->isEnabled());
        frame.gotoOverviewPage();
        QVERIFY(stack->currentWidget() != fallback);
        frame.setClientModel(mini_gui.clientModel.get());
        auto* overview_view = new WalletView(&walletModel, platformStyle.get(), &frame);
        QVERIFY(frame.addView(overview_view));
        frame.setCurrentWallet(&walletModel);
        QVERIFY(!qobject_cast<MiningPage*>(overview_view->currentWidget()));
        frame.removeWallet(&walletModel);
        QVERIFY(stack->currentWidget() != fallback);
        QVERIFY(QMetaObject::invokeMethod(&walletModel, "canGetAddressesChanged", Qt::DirectConnection));
        frame.setClientModel(nullptr);
    }

    // A fresh wallet can send automatically without fee history/fallbackfee,
    // and the UI must not claim to have estimated a confirmation deadline.
    wallet->m_fallback_fee = CFeeRate{0};
    sendCoinsDialog.findChild<QRadioButton*>("radioSmartFee")->setChecked(true);
    QVERIFY(QMetaObject::invokeMethod(&sendCoinsDialog, "updateSmartFeeLabel"));
    QVERIFY(sendCoinsDialog.findChild<QLabel*>("fallbackFeeWarningLabel")->isHidden());
    QCOMPARE(sendCoinsDialog.findChild<QLabel*>("labelFeeEstimation")->text(),
             QString("Using the current minimum fee. Confirmation time is not estimated."));

    // Update walletModel cached balance which will trigger an update for the 'labelBalance' QLabel.
    walletModel.pollBalanceChanged();
    // Check balance in send dialog
    CompareBalance(walletModel, walletModel.wallet().getBalance(), sendCoinsDialog.findChild<QLabel*>("labelBalance"));

    // Check 'UseAvailableBalance' functionality
    VerifyUseAvailableBalance(sendCoinsDialog, walletModel);

    // Send two transactions, and verify they are added to transaction list.
    TransactionTableModel* transactionTableModel = walletModel.getTransactionTableModel();
    QCOMPARE(transactionTableModel->rowCount({}), 105);
    Txid txid1 = SendCoins(*wallet.get(), sendCoinsDialog, ExternalTestDestination(), 5 * COIN);
    Txid txid2 = SendCoins(*wallet.get(), sendCoinsDialog, ExternalTestDestination(), 10 * COIN);
    // Transaction table model updates on a QueuedConnection, so process events to ensure it's updated.
    qApp->processEvents();
    QCOMPARE(transactionTableModel->rowCount({}), 107);
    QVERIFY(FindTx(*transactionTableModel, txid1).isValid());
    QVERIFY(FindTx(*transactionTableModel, txid2).isValid());
    QCOMPARE(FindTx(*transactionTableModel, txid1).data(TransactionTableModel::AddressRole).toString(),
             QString::fromStdString(EncodeDestination(ExternalTestDestination())));

    // Call bumpfee. Test canceled fullrbf bump, canceled bip-125-rbf bump, passing bump, and then failing bump.
    BumpFee(transactionView, txid1, /*expectDisabled=*/false, /*expectError=*/{}, /*cancel=*/true);
    BumpFee(transactionView, txid2, /*expectDisabled=*/false, /*expectError=*/{}, /*cancel=*/true);
    BumpFee(transactionView, txid2, /*expectDisabled=*/false, /*expectError=*/{}, /*cancel=*/false);
    BumpFee(transactionView, txid2, /*expectDisabled=*/true, /*expectError=*/"already bumped", /*cancel=*/false);

    // Check current balance on OverviewPage
    OverviewPage overviewPage(platformStyle.get());
    overviewPage.setWalletModel(&walletModel);
    walletModel.pollBalanceChanged(); // Manual balance polling update
    CompareBalance(walletModel, walletModel.wallet().getBalance(), overviewPage.findChild<QLabel*>("labelBalance"));

    // Check Request Payment button
    ReceiveCoinsDialog receiveCoinsDialog(platformStyle.get());
    receiveCoinsDialog.setModel(&walletModel);
    // Receiving must not offer the unsupported Bitcoin address formats.
    QVERIFY(!receiveCoinsDialog.findChild<QComboBox*>("addressType"));
    RecentRequestsTableModel* requestTableModel = walletModel.getRecentRequestsTableModel();

    // Label input
    QLineEdit* labelInput = receiveCoinsDialog.findChild<QLineEdit*>("reqLabel");
    labelInput->setText("TEST_LABEL_1");

    // Amount input
    BitcoinAmountField* amountInput = receiveCoinsDialog.findChild<BitcoinAmountField*>("reqAmount");
    amountInput->setValue(1);

    // Message input
    QLineEdit* messageInput = receiveCoinsDialog.findChild<QLineEdit*>("reqMessage");
    messageInput->setText("TEST_MESSAGE_1");
    int initialRowCount = requestTableModel->rowCount({});
    QPushButton* requestPaymentButton = receiveCoinsDialog.findChild<QPushButton*>("receiveButton");
    requestPaymentButton->click();
    QString address;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("ReceiveRequestDialog")) {
            ReceiveRequestDialog* receiveRequestDialog = qobject_cast<ReceiveRequestDialog*>(widget);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("payment_header")->text(), QString("Payment information"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("uri_tag")->text(), QString("URI:"));
            QString uri = receiveRequestDialog->QObject::findChild<QLabel*>("uri_content")->text();
            QCOMPARE(uri.count("connectcoin:"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("address_tag")->text(), QString("Address:"));
            QVERIFY(address.isEmpty());
            address = receiveRequestDialog->QObject::findChild<QLabel*>("address_content")->text();
            QVERIFY(!address.isEmpty());

            QCOMPARE(uri.count("amount=0.0000000001"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("amount_tag")->text(), QString("Amount:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("amount_content")->text(), QString::fromStdString("0.0000000001 " + CURRENCY_UNIT));

            QCOMPARE(uri.count("label=TEST_LABEL_1"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("label_tag")->text(), QString("Label:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("label_content")->text(), QString("TEST_LABEL_1"));

            QCOMPARE(uri.count("message=TEST_MESSAGE_1"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("message_tag")->text(), QString("Message:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("message_content")->text(), QString("TEST_MESSAGE_1"));
        }
    }

    // The generated address must represent a valid ConnectCoin type-1 P2PK output.
    QVERIFY(!address.isEmpty());
    const CTxDestination receive_dest{DecodeDestination(address.toStdString())};
    QVERIFY(IsValidDestination(receive_dest));
    const CTxOut receive_output{1, GetScriptForDestination(receive_dest)};
    QVERIFY(receive_output.GetP2PKPubKey().has_value());

    // Clear button
    QPushButton* clearButton = receiveCoinsDialog.findChild<QPushButton*>("clearButton");
    clearButton->click();
    QCOMPARE(labelInput->text(), QString(""));
    QCOMPARE(amountInput->value(), CAmount(0));
    QCOMPARE(messageInput->text(), QString(""));

    // Check addition to history
    int currentRowCount = requestTableModel->rowCount({});
    QCOMPARE(currentRowCount, initialRowCount+1);

    // Check addition to wallet
    std::vector<std::string> requests = walletModel.wallet().getAddressReceiveRequests();
    QCOMPARE(requests.size(), size_t{1});
    RecentRequestEntry entry;
    SpanReader{MakeByteSpan(requests[0])} >> entry;
    QCOMPARE(entry.nVersion, int{1});
    QCOMPARE(entry.id, int64_t{1});
    QVERIFY(entry.date.isValid());
    QCOMPARE(entry.recipient.address, address);
    QCOMPARE(entry.recipient.label, QString{"TEST_LABEL_1"});
    QCOMPARE(entry.recipient.amount, CAmount{1});
    QCOMPARE(entry.recipient.message, QString{"TEST_MESSAGE_1"});
    QCOMPARE(entry.recipient.sPaymentRequest, std::string{});
    QCOMPARE(entry.recipient.authenticatedMerchant, QString{});

    // Check Remove button
    QTableView* table = receiveCoinsDialog.findChild<QTableView*>("recentRequestsView");
    table->selectRow(currentRowCount-1);
    QPushButton* removeRequestButton = receiveCoinsDialog.findChild<QPushButton*>("removeRequestButton");
    removeRequestButton->click();
    QCOMPARE(requestTableModel->rowCount({}), currentRowCount-1);

    // Check removal from wallet
    QCOMPARE(walletModel.wallet().getAddressReceiveRequests().size(), size_t{0});
}

void TestGUIWatchOnly(interfaces::Node& node, TestChain100Setup& test)
{
    const std::shared_ptr<CWallet>& wallet = SetupDescriptorsWallet(node, test, /*watch_only=*/true);

    // Create widgets and init models
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(node, platformStyle.get());
    mini_gui.initModelForWallet(node, wallet, platformStyle.get());
    WalletModel& walletModel = *mini_gui.walletModel;
    SendCoinsDialog& sendCoinsDialog = mini_gui.sendCoinsDialog;

    // Update walletModel cached balance which will trigger an update for the 'labelBalance' QLabel.
    walletModel.pollBalanceChanged();
    // Check balance in send dialog
    CompareBalance(walletModel, walletModel.wallet().getBalances().balance,
                   sendCoinsDialog.findChild<QLabel*>("labelBalance"));

    // Set change address
    sendCoinsDialog.getCoinControl()->destChange = TestDestination(test.coinbaseKey);

    // Time to reject "save" PSBT dialog ('SendCoins' locks the main thread until the dialog receives the event).
    QTimer timer;
    timer.setInterval(500);
    QObject::connect(&timer, &QTimer::timeout, [&](){
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->inherits("QMessageBox") && widget->objectName().compare("psbt_copied_message") == 0) {
                QMessageBox* dialog = qobject_cast<QMessageBox*>(widget);
                QAbstractButton* button = dialog->button(QMessageBox::Discard);
                button->setEnabled(true);
                button->click();
                timer.stop();
                break;
            }
        }
    });
    timer.start(500);

    // Send tx and verify PSBT copied to the clipboard.
    SendCoins(*wallet.get(), sendCoinsDialog, ExternalTestDestination(), 5 * COIN, QMessageBox::Save);
    const std::string& psbt_string = QApplication::clipboard()->text().toStdString();
    QVERIFY(!psbt_string.empty());

    // Decode psbt
    std::optional<std::vector<unsigned char>> decoded_psbt = DecodeBase64(psbt_string);
    QVERIFY(decoded_psbt);
    util::Result<PartiallySignedTransaction> psbt = DecodeRawPSBT(MakeByteSpan(*decoded_psbt));
    QVERIFY(psbt);
}

void TestGUI(interfaces::Node& node)
{
    // Set up wallet and chain with 105 blocks (5 mature blocks for spending).
    TestChain100Setup test;
    const CScript coinbase_script{GetScriptForDestination(TestDestination(test.coinbaseKey))};
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, coinbase_script);
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    node.setContext(&test.m_node);

    // "Full" GUI tests, use descriptor wallet
    const std::shared_ptr<CWallet>& desc_wallet = SetupDescriptorsWallet(node, test);
    TestGUI(node, desc_wallet);

    // Legacy watch-only wallet test
    // Verify PSBT creation.
    TestGUIWatchOnly(node, test);
}

void CheckP2CHistory(TransactionTableModel& history, const QString& domain, int expected_count)
{
    TransactionFilterProxy filtered;
    filtered.setSourceModel(&history);
    filtered.setSearchString(domain.toUpper()); // Domain search is case-insensitive.
    QCOMPARE(filtered.rowCount(), expected_count);
    const QString label = "P2C: " + domain;
    for (int row = 0; row < filtered.rowCount(); ++row) {
        const auto index = filtered.index(row, TransactionTableModel::ToAddress);
        QCOMPARE(index.data(Qt::DisplayRole).toString(), label);
        QCOMPARE(index.data(Qt::EditRole).toString(), label);
        QCOMPARE(index.data(TransactionTableModel::LabelRole).toString(), label);
        // A domain must not enable address-book editing or "Copy address".
        QVERIFY(index.data(TransactionTableModel::AddressRole).toString().isEmpty());
        QVERIFY(index.data(Qt::ToolTipRole).toString().contains(domain));
        QVERIFY(index.data(TransactionTableModel::TxPlainTextRole).toString().contains(label));
    }
    QVERIFY(filtered.index(0, 0).data(TransactionTableModel::LongDescriptionRole).toString().contains(domain));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("p2c-history.csv");
    CSVModelWriter writer(path);
    writer.setModel(&filtered);
    writer.addColumn("Label", 0, TransactionTableModel::LabelRole);
    writer.addColumn("Address", 0, TransactionTableModel::AddressRole);
    QVERIFY(writer.write());
    QFile csv(path);
    QVERIFY(csv.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(csv.readAll()).count(label), expected_count);
    filtered.setSearchString("absent.example");
    QCOMPARE(filtered.rowCount(), 0);
}

void TestP2CGUI(interfaces::Node& node)
{
    TestChain100Setup test;
    const CScript coinbase_script{GetScriptForDestination(TestDestination(test.coinbaseKey))};
    // Fund independent inputs for the single send, partial-commit batch,
    // 1000-output batch and encrypted sends; unconfirmed change cannot fund
    // independent split transactions. Do not depend on coin-selection luck.
    for (int i = 0; i < 12; ++i) test.CreateAndProcessBlock({}, coinbase_script);
    auto loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = loader.get();
    node.setContext(&test.m_node);
    auto database = std::make_unique<FailingP2CTestDatabase>();
    auto* failing_database = database.get();
    auto wallet = SetupDescriptorsWallet(node, test, false, std::move(database));
    struct UnloadWallet {
        WalletContext& context;
        std::shared_ptr<CWallet> wallet;
        void Unload()
        {
            if (wallet) RemoveWallet(context, std::exchange(wallet, {}), /*load_on_start=*/std::nullopt);
        }
        ~UnloadWallet() { Unload(); }
    } unload_wallet{*node.walletLoader().context(), wallet};
    std::unique_ptr<const PlatformStyle> style(PlatformStyle::instantiate("other"));
    MiniGUI gui(node, style.get());
    gui.initModelForWallet(node, wallet, style.get(), /*unload=*/false);
    P2CCreateDialog page;
    struct ProbeFixture {
        enum class Outcome { FAILURE, SUCCESS, PENDING, LATE_SUCCESS, EXCEPTION };
        std::atomic<Outcome> outcome{Outcome::FAILURE};
        std::atomic<int> started{0};
        std::atomic<int> finished{0};
        std::atomic<int> cancelled{0};
    };
    const auto probe = std::make_shared<ProbeFixture>();
    const P2CCreateDialog::RsaProbe mock_probe = [probe](const std::string&, uint32_t,
                                                       const std::function<bool()>& cancelled,
                                                       std::chrono::steady_clock::time_point deadline) {
        const auto outcome = probe->outcome.load();
        ++probe->started;
        if (outcome == ProbeFixture::Outcome::PENDING || outcome == ProbeFixture::Outcome::LATE_SUCCESS) {
            // Bound even a failed test's worker lifetime. A late result
            // deliberately ignores cancellation to test the GUI's own fence.
            while (std::chrono::steady_clock::now() < deadline &&
                   (outcome == ProbeFixture::Outcome::LATE_SUCCESS || !cancelled())) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            if (outcome == ProbeFixture::Outcome::LATE_SUCCESS) std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        if (cancelled()) ++probe->cancelled;
        ++probe->finished;
        if (outcome == ProbeFixture::Outcome::EXCEPTION) throw std::runtime_error("Injected probe failure");
        return outcome == ProbeFixture::Outcome::SUCCESS || outcome == ProbeFixture::Outcome::LATE_SUCCESS;
    };
    // Never use DNS or real TLS in the normal Qt test suite.
    page.setRsaProbeForTest(mock_probe);
    page.setModel(gui.walletModel.get());
    auto* claim_rate = page.findChild<QSpinBox*>("p2cClaimRate");
    auto* claim_unlimited = page.findChild<QCheckBox*>("p2cClaimUnlimited");
    auto* claim_concurrency = page.findChild<QSpinBox*>("p2cClaimConcurrency");
    auto* claim_stop = page.findChild<QPushButton*>("p2cClaimStop");
    auto* claim_status = page.findChild<QLabel*>("p2cClaimStatus");
    auto* claim_address = page.findChild<QLineEdit*>("p2cClaimAddress");
    auto* claim_reward_status = page.findChild<QLabel*>("p2cClaimRewardStatus");
    QVERIFY(claim_address && claim_reward_status);
    QVERIFY(claim_address->text().isEmpty());
    QTRY_VERIFY(claim_reward_status->text().contains("This wallet (default)"));
    QVERIFY(!page.findChild<QLabel*>("p2cClaimRoundHint"));
    QVERIFY(!page.findChild<QSpinBox*>("p2cClaimRoundSeconds"));
    QVERIFY(claim_rate && claim_unlimited && claim_concurrency && claim_stop && claim_status);
    QCOMPARE(claim_concurrency->value(), 1000);
    QCOMPARE(gui.walletModel->wallet().getP2CClaimStatus()["concurrency"].getInt<int>(), 1000);
    QTRY_VERIFY(claim_status->text().contains("Concurrency: 1000"));
    QCOMPARE(claim_concurrency->maximum(), std::numeric_limits<int>::max());
    claim_concurrency->selectAll();
    QTest::keyClicks(claim_concurrency, "128");
    QTest::keyClick(claim_concurrency, Qt::Key_Tab);
    QCOMPARE(claim_concurrency->value(), 128);
    QCOMPARE(claim_rate->value(), 0);
    QCOMPARE(claim_rate->text(), QStringLiteral("0"));
    claim_rate->selectAll();
    QTest::keyClicks(claim_rate, "10");
    QTest::keyClick(claim_rate, Qt::Key_Tab);
    QCOMPARE(claim_rate->value(), 10);
    QCOMPARE(claim_rate->text(), QStringLiteral("10"));
    QVERIFY(!claim_unlimited->isChecked());
    claim_unlimited->setChecked(true);
    QVERIFY(!claim_rate->isEnabled());
    // An unmatched allowlist keeps this UI test entirely offline.
    auto configure_claims = gui.walletModel->wallet().configureP2CClaiming(-1, 128, {"unfunded.example"});
    const auto configure_error = configure_claims.get();
    QVERIFY2(configure_error.empty(), configure_error.c_str());
    QTRY_VERIFY(claim_status->text().contains("Active rate: Unlimited"));
    QVERIFY(!claim_status->text().contains("Active rate: -1"));
    claim_stop->click(); // Zero must disable, never mean unlimited.
    QTRY_VERIFY(claim_stop->isEnabled());
    QCOMPARE(claim_rate->value(), 0);
    QCOMPARE(claim_rate->text(), QStringLiteral("0"));
    QVERIFY(!claim_unlimited->isChecked());
    QCOMPARE(gui.walletModel->wallet().getP2CClaimStatus()["connections_per_second"].getInt<int>(), 0);
    QTRY_VERIFY(claim_status->text().contains("Active rate: Disabled (0)"));
    const auto external{EncodeDestination(ExternalTestDestination())};
    auto configure_limited = gui.walletModel->wallet().configureP2CClaiming(10, 4, {"unfunded.example"}, external);
    QVERIFY(configure_limited.get().empty());
    QTRY_VERIFY(claim_status->text().contains("Active rate: 10 |"));
    QTRY_VERIFY(claim_reward_status->text().contains(QString::fromStdString(external)));
    claim_stop->click();
    QTRY_VERIFY(claim_stop->isEnabled());
    QTRY_VERIFY(claim_reward_status->text().contains("This wallet (default)"));
    claim_address->setText(QString::fromStdString(external));
    claim_rate->setValue(1);
    page.findChild<QLineEdit*>("p2cClaimDomains")->setText("unfunded.example");
    QString confirmation_text;
    QTimer::singleShot(0, [&confirmation_text] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog{qobject_cast<QMessageBox*>(widget)}) {
                confirmation_text = dialog->text();
                dialog->button(QMessageBox::Yes)->click();
            }
        }
    });
    page.findChild<QPushButton*>("p2cClaimStart")->click();
    QVERIFY(confirmation_text.contains(QString::fromStdString(external)));
    QTRY_VERIFY(claim_stop->isEnabled());
    QCOMPARE(gui.walletModel->wallet().getP2CClaimStatus()["reward_address"].get_str(), external);
    claim_stop->click();
    QTRY_VERIFY(claim_stop->isEnabled());

    // Destruction during confirmation must never start HTTPS or double-delete
    // a stack-owned dialog when its wallet page disappears.
    auto* closing_claim_page{new P2CCreateDialog};
    const QPointer<P2CCreateDialog> closing_claim_guard{closing_claim_page};
    closing_claim_page->setModel(gui.walletModel.get());
    closing_claim_page->findChild<QSpinBox*>("p2cClaimRate")->setValue(1);
    closing_claim_page->findChild<QLineEdit*>("p2cClaimDomains")->setText("unfunded.example");
    QTimer::singleShot(0, [closing_claim_page] { delete closing_claim_page; });
    closing_claim_page->findChild<QPushButton*>("p2cClaimStart")->click();
    QVERIFY(closing_claim_guard.isNull());
    QCOMPARE(gui.walletModel->wallet().getP2CClaimStatus()["connections_per_second"].getInt<int>(), 0);

    {
        // Exercise the actual Mining page start button without hashing: a
        // header ahead keeps the miner waiting and avoids RandomX allocation.
        CBlockIndex ahead;
        auto* previous{WITH_LOCK(cs_main, return test.m_node.chainman->m_best_header)};
        ahead.nChainWork = previous->nChainWork + 1;
        struct RestoreMiner {
            node::NodeContext& node;
            CBlockIndex* previous;
            std::unique_ptr<interfaces::Mining> previous_mining;
            ~RestoreMiner()
            {
                node.cpu_miner.reset();
                node.mining = std::move(previous_mining);
                WITH_LOCK(cs_main, node.chainman->m_best_header = previous);
            }
        } restore_miner{test.m_node, previous, std::move(test.m_node.mining)};
        WITH_LOCK(cs_main, test.m_node.chainman->m_best_header = &ahead);
        test.m_node.mining = interfaces::MakeMining(test.m_node);
        test.m_node.cpu_miner = std::make_unique<node::CpuMiner>(test.m_node);
        ClientModel client{node, nullptr};
        MiningPage mining{gui.walletModel.get()};
        mining.setClientModel(&client);
        auto* reward_address{mining.findChild<QLineEdit*>("miningAddress")};
        QVERIFY(reward_address->text().isEmpty());
        std::string last_address;
        for (const auto& target : {std::string{}, std::string{}, external}) {
            reward_address->setText(QString::fromStdString(target));
            mining.findChild<QPushButton*>("startMining")->click();
            QTRY_VERIFY(node.getCpuMiningStatus().state == "waiting");
            const auto status{node.getCpuMiningStatus()};
            QCOMPARE(status.hashes, uint64_t{0});
            QCOMPARE(reward_address->text(), QString::fromStdString(target));
            if (target.empty()) {
                QVERIFY(WITH_LOCK(wallet->cs_wallet, return wallet->IsMine(CTxOut{0, GetScriptForDestination(DecodeDestination(status.address))})));
                QVERIFY(status.address != last_address);
            } else {
                QCOMPARE(status.address, external);
            }
            last_address = status.address;
            node.stopCpuMining();
            QTRY_VERIFY(!node.getCpuMiningStatus().running);
            mining.setClientModel(&client); // Refresh Start availability.
        }
    }
    unload_wallet.Unload();
    auto* domain = page.findChild<QLineEdit*>("p2cDomain");
    auto* amount = page.findChild<BitcoinAmountField*>("p2cAmount");
    auto* count = page.findChild<QSpinBox*>("p2cOutputCount");
    auto* mode = page.findChild<QComboBox*>("p2cWorkMode");
    auto* bits = page.findChild<QSpinBox*>("p2cWorkBits");
    auto* target = page.findChild<QLineEdit*>("p2cTarget");
    auto* custom_fee = page.findChild<QCheckBox*>("p2cCustomFee");
    auto* fee_rate = page.findChild<BitcoinAmountField*>("p2cFeeRate");
    auto* create = page.findChild<QPushButton*>("p2cCreateButton");
    QVERIFY(domain && amount && count && mode && bits && target && custom_fee && fee_rate && create);
    QCOMPARE(count->maximum(), 1000);
    QString error;
    QObject::connect(&page, &P2CCreateDialog::message, [&error](const QString&, const QString& text, unsigned int) { error = text; });
    std::vector<Txid> sent;
    QObject::connect(&page, &P2CCreateDialog::coinsSent, [&sent](const Txid& txid) { sent.push_back(txid); });
    amount->setValue(COIN);
    domain->setText("https://example.com");
    create->click();
    QVERIFY(!error.isEmpty());
    QVERIFY(sent.empty());
    QVERIFY(!page.findChild<QMessageBox*>("p2cConfirmation"));

    domain->setText("example.com");
    mode->setCurrentIndex(1);
    target->setText("ff");
    error.clear();
    create->click();
    QVERIFY(error.contains("64"));
    QVERIFY(sent.empty());
    mode->setCurrentIndex(0);
    bits->setValue(256);
    QCOMPARE(target->text(), QString(64, '0'));
    bits->setValue(0);
    QCOMPARE(target->text(), QString(64, 'f'));
    bits->setValue(10);
    QCOMPARE(target->text(), QString("003") + QString(61, 'f'));

    count->setValue(2);
    amount->setValue(MAX_MONEY);
    error.clear();
    create->click();
    QVERIFY(error.contains("money limit"));
    amount->setValue(COIN);
    custom_fee->setChecked(true);
    fee_rate->setValue(2'000'000);

    // Reject extreme rates before fee multiplication for a large request.
    count->setValue(1000);
    amount->setValue(COIN / 1000);
    fee_rate->setValue(MAX_MONEY);
    error.clear();
    create->click();
    QVERIFY(error.contains("Fee rate"));
    QVERIFY(!page.findChild<QMessageBox*>("p2cConfirmation"));
    QVERIFY(sent.empty());
    count->setValue(2);
    amount->setValue(COIN);
    fee_rate->setValue(2'000'000);

    // The GUI must honor the wallet's avoid-reuse preference.
    {
        LOCK(wallet->cs_wallet);
        wallet->SetWalletFlag(WALLET_FLAG_AVOID_REUSE);
        WalletBatch db_batch(wallet->GetDatabase());
        QVERIFY(wallet->SetAddressPreviouslySpent(db_batch, TestDestination(test.coinbaseKey), true));
    }
    error.clear();
    create->click();
    QVERIFY(!error.isEmpty());
    QVERIFY(!page.findChild<QMessageBox*>("p2cConfirmation"));
    QVERIFY(sent.empty());
    {
        LOCK(wallet->cs_wallet);
        WalletBatch db_batch(wallet->GetDatabase());
        QVERIFY(wallet->SetAddressPreviouslySpent(db_batch, TestDestination(test.coinbaseKey), false));
        wallet->UnsetWalletFlag(WALLET_FLAG_AVOID_REUSE);
    }
    page.setModel(gui.walletModel.get());

    // Optional offscreen render for visual review; regular CI needs no files.
    const QString screenshot_path = qEnvironmentVariable("CONNECTCOIN_TEST_P2C_SCREENSHOT");
    if (!screenshot_path.isEmpty()) {
        page.resize(850, 700);
        QVERIFY(page.grab().save(screenshot_path));
    }

    // Cancel a real signed proposal; retain an unrelated user reservation.
    auto coins = gui.walletModel->wallet().listCoins();
    QVERIFY(!coins.empty());
    const COutPoint user_locked = std::get<0>(coins.begin()->second.front());
    QVERIFY(gui.walletModel->wallet().lockCoin(user_locked, false));
    error.clear();
    const auto initial_probe = probe->started.load();
    create->click();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    auto* confirmation = page.findChild<QMessageBox*>("p2cConfirmation");
    QVERIFY(confirmation);
    QVERIFY(confirmation->text().contains("Total fees"));
    QVERIFY(confirmation->text().contains("Total debit"));
    QVERIFY(confirmation->text().contains("example.com"));
    QVERIFY(!confirmation->button(QMessageBox::Yes)->isEnabled());
    QTRY_COMPARE(probe->started.load(), initial_probe + 1);
    std::vector<COutPoint> locks;
    gui.walletModel->wallet().listLockedCoins(locks);
    QVERIFY(locks.size() > 1);
    confirmation->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    locks.clear();
    gui.walletModel->wallet().listLockedCoins(locks);
    QCOMPARE(locks.size(), size_t{1});
    QVERIFY(locks.front() == user_locked);
    QVERIFY(sent.empty());
    QVERIFY(gui.walletModel->wallet().unlockCoin(user_locked));

    // Neither a timer nor a programmatic early approval may submit a batch.
    const auto early_probe = probe->started.load();
    create->click();
    confirmation = page.findChild<QMessageBox*>("p2cConfirmation");
    QVERIFY(confirmation);
    QTRY_COMPARE(probe->started.load(), early_probe + 1);
    QVERIFY(!confirmation->button(QMessageBox::Yes)->isEnabled());
    confirmation->done(QMessageBox::Yes);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(sent.empty());
    locks.clear();
    gui.walletModel->wallet().listLockedCoins(locks);
    QVERIFY(locks.empty());

    // Incomplete, late and exceptional probes all freeze the conservative mask.
    for (const auto outcome : {ProbeFixture::Outcome::PENDING, ProbeFixture::Outcome::LATE_SUCCESS, ProbeFixture::Outcome::EXCEPTION}) {
        QTRY_COMPARE(probe->started.load(), probe->finished.load());
        probe->outcome = outcome;
        const auto completed = probe->finished.load();
        create->click();
        confirmation = page.findChild<QMessageBox*>("p2cConfirmation");
        QVERIFY(confirmation);
        QVERIFY(!confirmation->button(QMessageBox::Yes)->isEnabled());
        QTRY_VERIFY(confirmation->button(QMessageBox::Yes)->isEnabled());
        QVERIFY(confirmation->informativeText().contains("mask 7"));
        const auto frozen = confirmation->informativeText();
        QTRY_COMPARE(probe->finished.load(), completed + 1);
        QCOMPARE(confirmation->informativeText(), frozen);
        QVERIFY(sent.empty());
        confirmation->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    probe->outcome = ProbeFixture::Outcome::FAILURE;

    // A resolver may ignore cancellation. Repeated review/cancel must not
    // create unbounded workers across the process, and saturation stays safe.
    {
        struct BlockedProbes {
            std::atomic<bool> release{false};
            std::atomic<int> entered{0};
            std::atomic<int> exited{0};
        };
        const auto blocked = std::make_shared<BlockedProbes>();
        struct ReleaseProbes {
            std::shared_ptr<BlockedProbes> state;
            ~ReleaseProbes() { state->release = true; }
        } release{blocked};
        page.setRsaProbeForTest([blocked](const std::string&, uint32_t, const std::function<bool()>&,
                                         std::chrono::steady_clock::time_point deadline) {
            ++blocked->entered;
            while (!blocked->release && std::chrono::steady_clock::now() < deadline + std::chrono::seconds{30}) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            ++blocked->exited;
            return true;
        });
        for (int i = 1; i <= 4; ++i) {
            create->click();
            confirmation = page.findChild<QMessageBox*>("p2cConfirmation");
            QVERIFY(confirmation);
            QTRY_COMPARE(blocked->entered.load(), i);
            confirmation->reject();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        create->click();
        confirmation = page.findChild<QMessageBox*>("p2cConfirmation");
        QVERIFY(confirmation);
        QTRY_VERIFY(confirmation->button(QMessageBox::Yes)->isEnabled());
        QVERIFY(confirmation->informativeText().contains("mask 7"));
        QCOMPARE(blocked->entered.load(), 4);
        QCOMPARE(blocked->exited.load(), 0);
        QVERIFY(sent.empty());
        confirmation->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        blocked->release = true;
        QTRY_COMPARE(blocked->exited.load(), 4);
        page.setRsaProbeForTest(mock_probe);
    }

    // A manual unlock/relock during review belongs to the user, not the batch.
    create->click();
    confirmation = page.findChild<QMessageBox*>("p2cConfirmation");
    QVERIFY(confirmation);
    locks.clear();
    gui.walletModel->wallet().listLockedCoins(locks);
    QVERIFY(!locks.empty());
    const COutPoint replaced_lock{locks.front()};
    QVERIFY(gui.walletModel->wallet().unlockCoin(replaced_lock));
    QVERIFY(gui.walletModel->wallet().lockCoin(replaced_lock, /*write_to_db=*/true));
    QTRY_VERIFY(confirmation->button(QMessageBox::Yes)->isEnabled());
    QVERIFY(confirmation->informativeText().contains("mask 7"));
    confirmation->done(QMessageBox::Yes);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(error.contains("reservation changed"));
    QVERIFY(sent.empty());
    QVERIFY(gui.walletModel->wallet().isLockedCoin(replaced_lock));
    QVERIFY(gui.walletModel->wallet().unlockCoin(replaced_lock));
    error.clear();

    // Unloading the page cancels an in-flight probe without keeping a wallet.
    QTRY_COMPARE(probe->started.load(), probe->finished.load());
    probe->outcome = ProbeFixture::Outcome::PENDING;
    const auto cancellations = probe->cancelled.load();
    const auto started = probe->started.load();
    create->click();
    QVERIFY(page.findChild<QMessageBox*>("p2cConfirmation"));
    QTRY_COMPARE(probe->started.load(), started + 1);
    page.setModel(nullptr);
    QTRY_COMPARE(probe->cancelled.load(), cancellations + 1);
    QTRY_COMPARE(probe->started.load(), probe->finished.load());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    locks.clear();
    gui.walletModel->wallet().listLockedCoins(locks);
    QVERIFY(locks.empty());
    QVERIFY(sent.empty());
    page.setModel(gui.walletModel.get());

    // Destroying a page while the worker is active must also release every
    // reservation; the detached callback cannot refer to any deleted QObject.
    auto closing_page = std::make_unique<P2CCreateDialog>();
    closing_page->setRsaProbeForTest(mock_probe);
    closing_page->setModel(gui.walletModel.get());
    closing_page->findChild<QLineEdit*>("p2cDomain")->setText("example.com");
    closing_page->findChild<BitcoinAmountField*>("p2cAmount")->setValue(COIN);
    closing_page->findChild<QCheckBox*>("p2cCustomFee")->setChecked(true);
    closing_page->findChild<BitcoinAmountField*>("p2cFeeRate")->setValue(2'000'000);
    const auto closing_started = probe->started.load();
    const auto closing_cancelled = probe->cancelled.load();
    closing_page->findChild<QPushButton*>("p2cCreateButton")->click();
    QVERIFY(closing_page->findChild<QMessageBox*>("p2cConfirmation"));
    QTRY_COMPARE(probe->started.load(), closing_started + 1);
    closing_page.reset();
    QTRY_COMPARE(probe->cancelled.load(), closing_cancelled + 1);
    QTRY_COMPARE(probe->started.load(), probe->finished.load());
    locks.clear();
    gui.walletModel->wallet().listLockedCoins(locks);
    QVERIFY(locks.empty());
    QVERIFY(sent.empty());

    // Submit exactly the reviewed output after a timely verified RSA probe,
    // even if the disabled form is subsequently changed.
    probe->outcome = ProbeFixture::Outcome::SUCCESS;
    const auto successful_probe = probe->finished.load();
    create->click();
    confirmation = page.findChild<QMessageBox*>("p2cConfirmation");
    QVERIFY(confirmation);
    QTRY_COMPARE(probe->finished.load(), successful_probe + 1);
    QVERIFY(!confirmation->button(QMessageBox::Yes)->isEnabled());
    QTRY_VERIFY(confirmation->button(QMessageBox::Yes)->isEnabled());
    QVERIFY(confirmation->informativeText().contains("mask 6"));
    QVERIFY(confirmation->detailedText().contains("mask 6"));
    domain->setText("changed.example");
    confirmation->done(QMessageBox::Yes);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(sent.size(), size_t{1});
    QVERIFY(node.context()->chain->isInMempool(sent.front()));
    {
        LOCK(wallet->cs_wallet);
        const auto* wtx = wallet->GetWalletTx(sent.front());
        QVERIFY(wtx);
        int p2c_count{0};
        for (const auto& out : wtx->GetTx()->vout) {
            if (const auto p2c = out.GetPayToDomain()) {
                QCOMPARE(p2c->domain, std::string{"example.com"});
                QCOMPARE(p2c->signature_algorithms_mask, PayToDomainOutput::SIGNATURE_ALGORITHMS_RSA);
                QCOMPARE(p2c->connection_work_target.GetHex(), std::string{"003"} + std::string(61, 'f'));
                QCOMPARE(out.nValue, COIN);
                ++p2c_count;
            }
        }
        QCOMPARE(p2c_count, 2);
    }
    qApp->processEvents();
    CheckP2CHistory(*gui.walletModel->getTransactionTableModel(), "example.com", 2);
    {
        // Reopening history reconstructs labels from existing outputs, without
        // needing any address-book entries or GUI-specific transaction metadata.
        TransactionTableModel reloaded(style.get(), gui.walletModel.get());
        CheckP2CHistory(reloaded, "example.com", 2);
    }
    {
        auto varied_wtx = gui.walletModel->wallet().getWalletTx(sent.front());
        CMutableTransaction varied{*varied_wtx.tx};
        int changed{0};
        for (auto& output : varied.vout) {
            if (auto p2c = output.GetPayToDomain()) {
                p2c->domain = changed++ == 0 ? "one.example" : "two.example";
                output = CTxOut{output.nValue, *p2c};
            }
        }
        QCOMPARE(changed, 2);
        varied_wtx.tx = MakeTransactionRef(std::move(varied));
        varied_wtx.comment_to = "not-the-output-domain.example";
        const auto records = TransactionRecord::decomposeTransaction(varied_wtx);
        QCOMPARE(records.size(), 2);
        std::set<std::string> domains;
        for (const auto& record : records) {
            const auto p2c = varied_wtx.tx->vout.at(record.idx).GetPayToDomain();
            QVERIFY(p2c);
            QCOMPARE(record.p2c_domain, p2c->domain);
            QVERIFY(record.address.empty());
            domains.insert(record.p2c_domain);
        }
        QVERIFY(domains == (std::set<std::string>{"one.example", "two.example"}));
    }
    QVERIFY(QMetaObject::invokeMethod(&page, "finishConfirmation", Q_ARG(int, static_cast<int>(QMessageBox::Yes))));
    QCOMPARE(sent.size(), size_t{1});

    // A real storage failure on the second transaction must report only the
    // first committed transaction, release remaining reservations, and never
    // automatically retry the original batch.
    const std::string partial_domain = std::string(63, 'p') + "." + std::string(63, 'q') + "." + std::string(63, 'r') + "." + std::string(61, 's');
    probe->outcome = ProbeFixture::Outcome::SUCCESS;
    domain->setText(QString::fromStdString(partial_domain));
    count->setValue(1000);
    amount->setValue(COIN / 1000);
    error.clear();
    create->click();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    confirmation = page.findChild<QMessageBox*>("p2cConfirmation");
    QVERIFY(confirmation);
    QTRY_VERIFY(confirmation->button(QMessageBox::Yes)->isEnabled());
    QVERIFY(confirmation->informativeText().contains("mask 6"));
    sent.clear();
    wallet->SetBroadcastTransactions(false);
    failing_database->batches_until_failure = 1;
    confirmation->done(QMessageBox::Yes);
    failing_database->batches_until_failure = -1;
    wallet->SetBroadcastTransactions(true);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(error.contains("Injected P2C database failure"));
    QCOMPARE(sent.size(), size_t{1});
    {
        LOCK(wallet->cs_wallet);
        const auto* wtx = wallet->GetWalletTx(sent.front());
        QVERIFY(wtx);
        size_t partial_count{0};
        for (const auto& out : wtx->GetTx()->vout) {
            if (const auto p2c = out.GetPayToDomain()) {
                QCOMPARE(p2c->domain, partial_domain);
                QCOMPARE(p2c->signature_algorithms_mask, PayToDomainOutput::SIGNATURE_ALGORITHMS_RSA);
                ++partial_count;
            }
        }
        QVERIFY(partial_count > 0 && partial_count < 1000);
    }
    QVERIFY(QMetaObject::invokeMethod(&page, "finishConfirmation", Q_ARG(int, static_cast<int>(QMessageBox::Yes))));
    QCOMPARE(sent.size(), size_t{1});
    locks.clear();
    gui.walletModel->wallet().listLockedCoins(locks);
    QVERIFY(locks.empty());

    // A long-domain 1000-output request must split, using disjoint inputs.
    const std::string long_domain = std::string(63, 'a') + "." + std::string(63, 'b') + "." + std::string(63, 'c') + "." + std::string(61, 'd');
    domain->setText(QString::fromStdString(long_domain));
    mode->setCurrentIndex(1);
    target->setText(QString(64, 'f'));
    count->setValue(1000);
    amount->setValue(COIN / 1000);
    error.clear();
    create->click();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    confirmation = page.findChild<QMessageBox*>("p2cConfirmation");
    QVERIFY(confirmation);
    QTRY_VERIFY(confirmation->button(QMessageBox::Yes)->isEnabled());
    QVERIFY(confirmation->informativeText().contains("mask 6"));
    sent.clear();
    confirmation->done(QMessageBox::Yes);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(sent.size() > 1);
    int p2c_count{0};
    std::set<COutPoint> inputs;
    {
        LOCK(wallet->cs_wallet);
        for (const auto& txid : sent) {
            QVERIFY(node.context()->chain->isInMempool(txid));
            const auto* wtx = wallet->GetWalletTx(txid);
            QVERIFY(wtx);
            for (const auto& input : wtx->GetTx()->vin) QVERIFY(inputs.insert(input.prevout).second);
            for (const auto& out : wtx->GetTx()->vout) {
                if (const auto p2c = out.GetPayToDomain()) {
                    QCOMPARE(p2c->domain, long_domain);
                    QCOMPARE(p2c->signature_algorithms_mask, PayToDomainOutput::SIGNATURE_ALGORITHMS_RSA);
                    QCOMPARE(out.nValue, COIN / 1000);
                    ++p2c_count;
                }
            }
        }
    }
    QCOMPARE(p2c_count, 1000);
    qApp->processEvents();
    CheckP2CHistory(*gui.walletModel->getTransactionTableModel(), QString::fromStdString(long_domain), 1000);
    locks.clear();
    gui.walletModel->wallet().listLockedCoins(locks);
    QVERIFY(locks.empty());

    // Insufficient funds must neither send nor leave reservations behind.
    amount->setValue(MAX_MONEY / 1000);
    sent.clear();
    error.clear();
    create->click();
    QVERIFY(!error.isEmpty());
    QVERIFY(sent.empty());
    locks.clear();
    gui.walletModel->wallet().listLockedCoins(locks);
    QVERIFY(locks.empty());

    // Unlock only to sign; review and cancellation must leave it locked.
    const SecureString passphrase{"test-p2c-wallet-passphrase"};
    QVERIFY(wallet->EncryptWallet(passphrase));
    QVERIFY(wallet->IsLocked());
    count->setValue(1);
    amount->setValue(COIN / 1000);
    error.clear();
    create->click(); // No unlock handler: equivalent to cancelling unlock.
    QVERIFY(!page.findChild<QMessageBox*>("p2cConfirmation"));
    QVERIFY(wallet->IsLocked());
    QVERIFY(sent.empty());
    auto unlock_connection = QObject::connect(gui.walletModel.get(), &WalletModel::requireUnlock, [&] {
        QVERIFY(wallet->Unlock(passphrase));
        page.setModel(nullptr);
    });
    create->click();
    QVERIFY(!page.findChild<QMessageBox*>("p2cConfirmation"));
    QVERIFY(wallet->IsLocked());
    QVERIFY(sent.empty());
    QObject::disconnect(unlock_connection);
    page.setModel(gui.walletModel.get());

    unlock_connection = QObject::connect(gui.walletModel.get(), &WalletModel::requireUnlock, [&] {
        QVERIFY(wallet->Unlock(passphrase));
        failing_database->batches_until_failure = 0;
    });
    create->click();
    failing_database->batches_until_failure = -1;
    QVERIFY(error.contains("Injected P2C database failure"));
    QVERIFY(!page.findChild<QMessageBox*>("p2cConfirmation"));
    QVERIFY(wallet->IsLocked());
    QVERIFY(sent.empty());
    locks.clear();
    gui.walletModel->wallet().listLockedCoins(locks);
    QVERIFY(locks.empty());
    QObject::disconnect(unlock_connection);
    error.clear();

    unlock_connection = QObject::connect(gui.walletModel.get(), &WalletModel::requireUnlock, [&] {
        QVERIFY(wallet->Unlock(passphrase));
        // A nested unlock callback must not change what the confirmation says
        // about the already captured request.
        domain->setText("changed-during-unlock.example");
        target->setText(QString(64, '0'));
    });
    domain->setText("original.example");
    target->setText(QString(64, 'f'));
    const auto encrypted_cancel_probe = probe->started.load();
    create->click();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    confirmation = page.findChild<QMessageBox*>("p2cConfirmation");
    QVERIFY(confirmation);
    QVERIFY(wallet->IsLocked());
    QVERIFY(confirmation->text().contains("original.example"));
    QVERIFY(!confirmation->text().contains("changed-during-unlock.example"));
    QTRY_COMPARE(probe->started.load(), encrypted_cancel_probe + 1);
    QVERIFY(confirmation->detailedText().contains(QString(64, 'f')));
    confirmation->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(wallet->IsLocked());
    QVERIFY(sent.empty());
    locks.clear();
    gui.walletModel->wallet().listLockedCoins(locks);
    QVERIFY(locks.empty());
    QObject::disconnect(unlock_connection);

    // The RSA alternative was already signed before review: choosing and
    // submitting it after relock must not request another password.
    int unlock_requests{0};
    unlock_connection = QObject::connect(gui.walletModel.get(), &WalletModel::requireUnlock, [&] {
        ++unlock_requests;
        QVERIFY(wallet->Unlock(passphrase));
    });
    probe->outcome = ProbeFixture::Outcome::SUCCESS;
    domain->setText("original.example");
    target->setText(QString(64, 'f'));
    create->click();
    confirmation = page.findChild<QMessageBox*>("p2cConfirmation");
    QVERIFY(confirmation);
    QVERIFY(wallet->IsLocked());
    QCOMPARE(unlock_requests, 1);
    QTRY_VERIFY(confirmation->button(QMessageBox::Yes)->isEnabled());
    QVERIFY(wallet->IsLocked());
    QVERIFY(confirmation->informativeText().contains("mask 6"));
    confirmation->done(QMessageBox::Yes);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(wallet->IsLocked());
    QCOMPARE(unlock_requests, 1);
    QCOMPARE(sent.size(), size_t{1});
    {
        LOCK(wallet->cs_wallet);
        const auto* wtx = wallet->GetWalletTx(sent.front());
        QVERIFY(wtx);
        int rewards{0};
        for (const auto& out : wtx->GetTx()->vout) {
            if (const auto p2c = out.GetPayToDomain()) {
                QCOMPARE(p2c->signature_algorithms_mask, PayToDomainOutput::SIGNATURE_ALGORITHMS_RSA);
                QCOMPARE(p2c->domain, std::string{"original.example"});
                ++rewards;
            }
        }
        QCOMPARE(rewards, 1);
    }
    QObject::disconnect(unlock_connection);
    page.setModel(nullptr);
    QVERIFY(!create->isEnabled());

    auto watch_wallet = SetupDescriptorsWallet(node, test, /*watch_only=*/true);
    MiniGUI watch_gui(node, style.get());
    watch_gui.initModelForWallet(node, watch_wallet, style.get());
    page.setModel(watch_gui.walletModel.get());
    QVERIFY(!create->isEnabled());
}

} // namespace

void WalletTests::walletTests()
{
    TestGUI(m_node);
}

void WalletTests::miningPage()
{
    TestingSetup test{ChainType::REGTEST};
    test.m_node.cpu_miner = std::make_unique<node::CpuMiner>(test.m_node);
    auto node{interfaces::MakeNode(test.m_node)};
    ClientModel client{*node, nullptr};
    MiningPage page{nullptr};
    auto* threads{page.findChild<QSpinBox*>("miningThreads")};
    const auto* thread_warning{page.findChild<QLabel*>("miningThreadWarning")};
    QVERIFY(threads);
    QVERIFY(thread_warning);
    QCOMPARE(threads->value(), 1);
    QCOMPARE(threads->minimum(), 1);
    QCOMPARE(threads->maximum(), 1024);
    QVERIFY(thread_warning->isHidden());
    QVERIFY(page.findChild<QLineEdit*>("miningAddress")->text().isEmpty());
    QVERIFY(!page.findChild<QPushButton*>("startMining")->isEnabled());
    QVERIFY(!page.findChild<QPushButton*>("stopMining")->isEnabled());
    QVERIFY(!page.findChild<QLabel*>("miningStatus")->text().isEmpty());
    page.setClientModel(&client);
    QCOMPARE(threads->value(), 1);
    QVERIFY(thread_warning->isHidden());
    const int logical_cpus{node->getCpuMiningStatus().logical_cpus};
    QVERIFY(logical_cpus >= 1);
    threads->setValue(logical_cpus);
    QVERIFY(thread_warning->isHidden());
    if (logical_cpus < threads->maximum()) {
        threads->setValue(logical_cpus + 1);
        QVERIFY(!thread_warning->isHidden());
        QVERIFY(thread_warning->text().contains(QString::number(logical_cpus)));
        // The warning is advisory: oversubscription must not disable Start.
        QVERIFY(page.findChild<QPushButton*>("startMining")->isEnabled());
    }
    threads->setValue(1024);
    QCOMPARE(threads->value(), 1024);
    page.setClientModel(&client); // Polling must not reset a stopped selection.
    QCOMPARE(threads->value(), 1024);
    QCOMPARE(thread_warning->isHidden(), logical_cpus >= 1024);
    threads->setValue(1025);
    QCOMPARE(threads->value(), 1024);
    threads->setValue(1);
    QVERIFY(thread_warning->isHidden());
    page.setClientModel(nullptr);
    QVERIFY(thread_warning->isHidden());
    QVERIFY(!page.findChild<QPushButton*>("startMining")->isEnabled());
    // Destroying a Mining page must also safely dispose of its error modal.
    auto* unloaded_page{new MiningPage{nullptr}};
    const QPointer<MiningPage> unloaded_guard{unloaded_page};
    unloaded_page->setClientModel(&client);
    unloaded_page->findChild<QLineEdit*>("miningAddress")->setText("invalid-address");
    QTimer::singleShot(0, [unloaded_page] { delete unloaded_page; });
    unloaded_page->findChild<QPushButton*>("startMining")->click();
    QTRY_VERIFY(unloaded_guard.isNull());
    struct PaletteGuard {
        QPalette original{QApplication::palette()};
        ~PaletteGuard() { QApplication::setPalette(original); }
    } palette_guard;
    for (const auto text_color : {Qt::black, Qt::white}) {
        QPalette palette{palette_guard.original};
        palette.setColor(QPalette::WindowText, text_color);
        QApplication::setPalette(palette);
        for (const auto* platform : {"windows", "other", "macosx"}) {
            const std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate(platform)};
            const auto icon{style->MiningIcon()};
            QVERIFY(!icon.isNull());
            for (const auto size : {16, 32, 128}) {
                const auto image{icon.pixmap(size, size).toImage()};
                QVERIFY(!image.isNull());
                bool opaque_ink{false};
                for (int y{0}; y < image.height(); ++y) {
                    for (int x{0}; x < image.width(); ++x) {
                        if (qAlpha(image.pixel(x, y)) == 255) {
                            opaque_ink = true;
                            // Overlapping antialiased strokes can round a color
                            // channel by one even when their combined alpha is 255.
                            const auto actual{image.pixelColor(x, y)};
                            const auto expected{style->SingleColor()};
                            QVERIFY(qAbs(actual.red() - expected.red()) <= 1);
                            QVERIFY(qAbs(actual.green() - expected.green()) <= 1);
                            QVERIFY(qAbs(actual.blue() - expected.blue()) <= 1);
                        }
                    }
                }
                QVERIFY(opaque_ink);
                QCOMPARE(qAlpha(image.pixel(0, 0)), 0);
            }
        }
    }
}

void WalletTests::p2cTests()
{
    TestP2CGUI(m_node);
}

void WalletTests::p2cTranslations()
{
    // Test the actual compiled resources, not only the editable TS catalogs.
    const auto locales = QDir(":/translations").entryList(QDir::Files);
    QVERIFY(!locales.isEmpty());
    for (const auto& locale : locales) {
        QTranslator translator;
        QVERIFY2(translator.load(":/translations/" + locale), qPrintable(locale));
        for (const auto* source : {"Create bounties", "Automatic claims", "Confirm P2C creation", "Send P2C"}) {
            QVERIFY2(!translator.translate("P2CCreateDialog", source).isEmpty(), qPrintable(locale + ": " + source));
        }
        for (const auto* source : {"Disabled", "Unlimited", "Waiting for bounties", "Simultaneous connections:", "Enable automatic P2C claiming?",
                 "Optional: empty uses this wallet", "Reward address:", "Reward target: %1", "This wallet (default)",
                 "An optional reward address overrides this wallet. Completed proofs keep their original destination when you change it."}) {
            QVERIFY2(!translator.translate("P2CClaimDialog", source).isEmpty(), qPrintable(locale + ": " + source));
        }
        QVERIFY2(!translator.translate("MiningPage", "Optional: empty uses this wallet").isEmpty(), qPrintable(locale));
    }
    for (const auto& locale : {QStringLiteral("pt"), QStringLiteral("pt_BR")}) {
        struct ScopedTranslator {
            QTranslator value;
            ~ScopedTranslator() { QCoreApplication::removeTranslator(&value); }
        } translator;
        QVERIFY(translator.value.load(":/translations/" + locale));
        QVERIFY(QCoreApplication::installTranslator(&translator.value));
        for (const auto* source : {"Checking RSA signature support…", "RSA-PSS with SHA-256",
                 "All supported algorithms", "Allowed signatures: %1 (mask %2)",
                 "Fund independent rewards for valid TLS connection proofs. During the three-second confirmation, this page checks the domain's RSA signature support with a TLS handshake. No HTTP request is sent."}) {
            QVERIFY2(!translator.value.translate("P2CCreateDialog", source).isEmpty(), qPrintable(locale + ": " + source));
        }
        P2CCreateDialog page;
        const auto* tabs = page.findChild<QTabWidget*>();
        QVERIFY(tabs);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Criar recompensas"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("Resgates automáticos"));
        const auto* status = page.findChild<QLabel*>("p2cClaimStatus");
        QVERIFY(status);
        QCOMPARE(status->text(), QStringLiteral("Desativado"));
        const auto* start = page.findChild<QPushButton*>("p2cClaimStart");
        QVERIFY(start);
        QCOMPARE(start->text(), QStringLiteral("Aplicar / iniciar resgates automáticos"));
        QVERIFY(!translator.value.translate("MiningPage", "Warning: %1 mining threads exceed the %2 logical CPUs detected. This can reduce hashrate and slow down the node.").isEmpty());
    }
}

void WalletTests::connectcoinTranslations()
{
    // Check compiled .qm resources too: a populated TS alone does not prove
    // that the translations are available in a shipped wallet.
    struct SourceMessage {
        QByteArray context;
        QByteArray source;
        QByteArray comment;
        bool numerus{false};
    };
    std::vector<SourceMessage> messages;
    QFile source_catalog(":/translation-test/source.ts");
    QVERIFY(source_catalog.open(QIODevice::ReadOnly));
    QXmlStreamReader xml(&source_catalog);
    QVERIFY(xml.readNextStartElement());
    QCOMPARE(xml.name(), QStringLiteral("TS"));
    while (xml.readNextStartElement()) {
        if (xml.name() != QStringLiteral("context")) {
            xml.skipCurrentElement();
            continue;
        }
        QByteArray context;
        while (xml.readNextStartElement()) {
            if (xml.name() == QStringLiteral("name")) {
                context = xml.readElementText().toUtf8();
            } else if (xml.name() == QStringLiteral("message")) {
                SourceMessage message{context, {}, {}, xml.attributes().value("numerus") == QStringLiteral("yes")};
                bool active{true};
                while (xml.readNextStartElement()) {
                    if (xml.name() == QStringLiteral("source")) {
                        message.source = xml.readElementText().toUtf8();
                    } else if (xml.name() == QStringLiteral("comment")) {
                        message.comment = xml.readElementText().toUtf8();
                    } else {
                        if (xml.name() == QStringLiteral("translation")) {
                            const auto type = xml.attributes().value("type");
                            active = type != QStringLiteral("vanished") && type != QStringLiteral("obsolete");
                        }
                        xml.skipCurrentElement();
                    }
                }
                if (active) messages.push_back(std::move(message));
            } else {
                xml.skipCurrentElement();
            }
        }
    }
    QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
    QVERIFY(messages.size() > 1000);
    const auto locales = QDir(":/translations").entryList(QDir::Files);
    QVERIFY(!locales.isEmpty());
    for (const auto& locale : locales) {
        QTranslator translator;
        QVERIFY2(translator.load(":/translations/" + locale), qPrintable(locale));
        for (const auto& message : messages) {
            // These counts cover all plural branches in the bundled Qt locales.
            // Query QTranslator directly, without base-language or English fallback.
            for (const int count : {0, 1, 2, 3, 4, 5, 6, 11, 21, 100}) {
                const auto text = translator.translate(message.context.constData(), message.source.constData(),
                    message.comment.isEmpty() ? nullptr : message.comment.constData(), message.numerus ? count : -1);
                QVERIFY2(!text.isEmpty(), qPrintable(locale + ": " + QString::fromUtf8(message.context) + ": " +
                    QString::fromUtf8(message.source) + " (n=" + QString::number(count) + ")"));
                if (!message.numerus) break;
            }
        }
        for (const auto* source : {"Mining", "Start mining", "Stop mining", "Reward address:", "CPU threads:",
                 "Waiting for the node to catch up", "Stopping (waiting for current work)",
                 "State: %1\nHashrate: %2 H/s\nHashes: %3 | Accepted blocks: %4\nActive reward address: %5"}) {
            const auto text = translator.translate("MiningPage", source);
            QVERIFY2(!text.isEmpty(), qPrintable(locale + ": " + source));
            if (QString::fromUtf8(source).contains("%5")) {
                for (int index{1}; index <= 5; ++index) {
                    QVERIFY2(text.contains("%" + QString::number(index)), qPrintable(locale + ": " + text));
                }
            }
        }
        QVERIFY2(!translator.translate("BitcoinGUI", "Control CPU mining").isEmpty(), qPrintable(locale));
        QVERIFY2(!translator.translate("OptionsDialog", "Enable pop-up notifications").isEmpty(), qPrintable(locale));
        // These translations are executable UI literals, not just labels.
        const auto psbt_filter = translator.translate("WalletFrame", "Partially Signed Transaction (*.psbt)");
        QVERIFY2(psbt_filter.contains("(*.psbt)"), qPrintable(locale + ": " + psbt_filter));
        const auto uri_error = translator.translate("PaymentServer", "'connectcoin://' is not a valid URI. Use 'connectcoin:' instead.");
        QCOMPARE(uri_error.count("connectcoin://"), 1);
        QCOMPARE(uri_error.count("connectcoin:"), 2);
        QVERIFY2(!translator.translate("OptionsDialog", "Show desktop pop-up notifications, including incoming and sent transactions. Disabled by default. Error and confirmation dialogs remain enabled.").isEmpty(), qPrintable(locale));
        QVERIFY2(!translator.translate("bitcoin-core", "ConnectCoin supports only type-1 P2PK (bech32m) addresses").isEmpty(), qPrintable(locale));
        QVERIFY2(!translator.translate("bitcoin-core", "Mainnet has not been launched: no genesis block is defined. Use -testnet4 for public testing or -regtest for local testing.").isEmpty(), qPrintable(locale));
    }
    QTranslator portuguese;
    QVERIFY(portuguese.load(":/translations/pt_BR"));
    QVERIFY(!portuguese.translate("bitcoin-core", "ConnectCoin supports only type-1 P2PK (bech32m) addresses").isEmpty());
    QVERIFY(!portuguese.translate("BitcoinGUI", "%n active connection(s) to the ConnectCoin network.", nullptr, 1).isEmpty());
    QVERIFY(!portuguese.translate("BitcoinGUI", "%n active connection(s) to the ConnectCoin network.", nullptr, 2).isEmpty());
    QTranslator arabic;
    QVERIFY(arabic.load(":/translations/ar"));
    for (const int count : {0, 1, 2, 3, 11, 100}) {
        QVERIFY(!arabic.translate("BitcoinGUI", "%n active connection(s) to the ConnectCoin network.", nullptr, count).isEmpty());
    }
}
