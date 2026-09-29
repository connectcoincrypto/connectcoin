// Copyright (c) 2017-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/addressbooktests.h>
#include <qt/test/util.h>
#include <test/util/setup_common.h>

#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <qt/addressbookpage.h>
#include <qt/addresstablemodel.h>
#include <qt/clientmodel.h>
#include <qt/editaddressdialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/walletmodel.h>

#include <key.h>
#include <key_io.h>
#include <outputtype.h>
#include <wallet/wallet.h>
#include <wallet/test/util.h>
#include <walletinitinterface.h>

#include <chrono>
#include <atomic>
#include <future>
#include <functional>
#include <thread>

#include <QApplication>
#include <QAbstractButton>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QTableView>
#include <QTimer>
#include <QPushButton>

using wallet::AddWallet;
using wallet::CWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::RemoveWallet;
using wallet::WALLET_FLAG_DESCRIPTORS;
using wallet::WalletContext;

namespace
{

/**
 * Fill the edit address dialog box with data, submit it, and ensure that
 * the resulting message meets expectations.
 */
void EditAddressAndSubmit(
        EditAddressDialog* dialog,
        const QString& label, const QString& address, QString expected_msg)
{
    QString warning_text;

    dialog->findChild<QLineEdit*>("labelEdit")->setText(label);
    dialog->findChild<QValidatedLineEdit*>("addressEdit")->setText(address);

    // Address mutations can now wait responsively for the wallet worker.
    // Keep looking until the actual warning appears, not just the wait UI.
    QTimer confirm;
    QObject::connect(&confirm, &QTimer::timeout, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* message = qobject_cast<QMessageBox*>(widget); message && message->isVisible()) {
                warning_text = message->text();
                message->defaultButton()->click();
            }
        }
    });
    confirm.start(5);
    dialog->accept();
    QCOMPARE(warning_text, expected_msg);
}

void CheckLaterAddressUpdate(WalletModel& wallet_model, AddressTableModel& addresses,
                            const CTxDestination& destination, const std::string& submitted_label,
                            ChangeType submitted_status, const std::function<bool()>& action)
{
    const auto address = QString::fromStdString(EncodeDestination(destination));
    const QString later_label = QString{"later backend label (%1:%2)"}.arg(QString::fromStdString(submitted_label)).arg(int(submitted_status));
    auto& backend = wallet_model.wallet();
    std::promise<void> delivered;
    auto delivery = delivered.get_future();
    std::promise<void> continuation_entered;
    auto continuation = continuation_entered.get_future();
    std::atomic<bool> triggered{false}, updated{false}, timed_out{false};
    bool observed{false};
    QObject observer;
    QObject::connect(&addresses, &AddressTableModel::labelsChanged, &observer, [&](const QString& changed) {
        if (observed || changed != address || addresses.labelForAddress(address) != later_label) return;
        observed = true;
        delivered.set_value();
    });
    // Enter after the model's own subscriber. The notification can run inside
    // an open SQLite transaction, so never write another batch from it. Enter
    // a GUI continuation first, then queue the later update on the same worker:
    // it runs only after the original mutation has returned and committed.
    // The nested wait keeps the original GUI action from resuming until the
    // later notification was consumed. No elapsed-time scheduling decides order.
    auto handler = backend.handleAddressBookChanged(
        [&](const CTxDestination& changed, const std::string& label, bool, wallet::AddressPurpose, ChangeType status) {
            if (changed != destination || label != submitted_label || status != submitted_status || triggered.exchange(true)) return;
            QMetaObject::invokeMethod(&observer, [&] {
                auto update = wallet_model.requestWalletData([&](interfaces::Wallet& wallet) {
                    updated = wallet.setAddressBook(destination, later_label.toStdString(), wallet::AddressPurpose::REFUND);
                    if (delivery.wait_for(std::chrono::seconds{5}) != std::future_status::ready) timed_out = true;
                });
                continuation_entered.set_value();
                GUIUtil::WaitForBackendTask(std::move(update));
            }, Qt::QueuedConnection);
            if (continuation.wait_for(std::chrono::seconds{5}) != std::future_status::ready) timed_out = true;
        });
    const bool succeeded = action();
    handler->disconnect();
    QVERIFY(succeeded);
    QVERIFY(triggered.load());
    QVERIFY(updated.load());
    QVERIFY(observed);
    QVERIFY(!timed_out.load());
    QCOMPARE(addresses.labelForAddress(address), later_label);
    QVERIFY(addresses.purposeForAddress(address) == wallet::AddressPurpose::REFUND);
    std::string backend_label;
    wallet::AddressPurpose backend_purpose;
    QVERIFY(backend.getAddress(destination, &backend_label, &backend_purpose));
    QCOMPARE(QString::fromStdString(backend_label), later_label);
    QVERIFY(backend_purpose == wallet::AddressPurpose::REFUND);
    QApplication::processEvents();
    QCOMPARE(addresses.labelForAddress(address), later_label);
    QVERIFY(addresses.purposeForAddress(address) == wallet::AddressPurpose::REFUND);
}

/**
 * Test adding various send addresses to the address book.
 *
 * There are three cases tested:
 *
 *   - new_address: a new address which should add as a send address successfully.
 *   - existing_s_address: an existing sending address which won't add successfully.
 *   - existing_r_address: an existing receiving address which won't add successfully.
 *
 * In each case, verify the resulting state of the address book and optionally
 * the warning message presented to the user.
 */
void TestAddAddressesToSendBook(interfaces::Node& node)
{
    TestChain100Setup test;
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    node.setContext(&test.m_node);
    const std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockableWalletDatabase());
    wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    {
        LOCK(wallet->cs_wallet);
        wallet->SetupDescriptorScriptPubKeyMans();
    }

    auto build_address{[]() {
        const WitnessV0KeyHash dest{GenerateRandomKey().GetPubKey()};
        return std::make_pair(dest, QString::fromStdString(EncodeDestination(dest)));
    }};

    CTxDestination r_key_dest, s_key_dest;

    // Add a preexisting "receive" entry in the address book.
    QString preexisting_r_address;
    QString r_label("already here (r)");

    // Add a preexisting "send" entry in the address book.
    QString preexisting_s_address;
    QString s_label("already here (s)");

    // Define a new address (which should add to the address book successfully).
    QString new_address_a;
    QString new_address_b;

    std::tie(r_key_dest, preexisting_r_address) = build_address();
    std::tie(s_key_dest, preexisting_s_address) = build_address();
    std::tie(std::ignore, new_address_a) = build_address();
    std::tie(std::ignore, new_address_b) = build_address();

    {
        LOCK(wallet->cs_wallet);
        wallet->SetAddressBook(r_key_dest, r_label.toStdString(), wallet::AddressPurpose::RECEIVE);
        wallet->SetAddressBook(s_key_dest, s_label.toStdString(), wallet::AddressPurpose::SEND);
    }

    auto check_addbook_size = [&wallet](int expected_size) {
        LOCK(wallet->cs_wallet);
        QCOMPARE(static_cast<int>(wallet->m_address_book.size()), expected_size);
    };

    // We should start with the two addresses we added earlier and nothing else.
    check_addbook_size(2);

    // Initialize relevant QT models.
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel optionsModel(node);
    bilingual_str error;
    QVERIFY(optionsModel.Init(error));
    ClientModel clientModel(node, &optionsModel);
    WalletContext& context = *node.walletLoader().context();
    AddWallet(context, wallet);
    WalletModel walletModel(interfaces::MakeWallet(context, wallet), clientModel, platformStyle.get());
    RemoveWallet(context, wallet, /* load_on_start= */ std::nullopt);
    EditAddressDialog editAddressDialog(EditAddressDialog::NewSendingAddress);
    editAddressDialog.setModel(walletModel.getAddressTableModel());

    AddressBookPage address_book{platformStyle.get(), AddressBookPage::ForEditing, AddressBookPage::SendingTab};
    address_book.setModel(walletModel.getAddressTableModel());
    auto table_view = address_book.findChild<QTableView*>("tableView");
    QTRY_COMPARE(table_view->model()->rowCount(), 1);

    EditAddressAndSubmit(
        &editAddressDialog, QString("uhoh"), preexisting_r_address,
        QString(
            "Address \"%1\" already exists as a receiving address with label "
            "\"%2\" and so cannot be added as a sending address."
            ).arg(preexisting_r_address).arg(r_label));
    check_addbook_size(2);
    QCOMPARE(table_view->model()->rowCount(), 1);

    EditAddressAndSubmit(
        &editAddressDialog, QString("uhoh, different"), preexisting_s_address,
        QString(
            "The entered address \"%1\" is already in the address book with "
            "label \"%2\"."
            ).arg(preexisting_s_address).arg(s_label));
    check_addbook_size(2);
    QCOMPARE(table_view->model()->rowCount(), 1);

    // Submit a new address which should add successfully - we expect the
    // warning message to be blank.
    EditAddressAndSubmit(
        &editAddressDialog, QString("io - new A"), new_address_a, QString(""));
    check_addbook_size(3);
    QTRY_COMPARE(table_view->model()->rowCount(), 2);

    EditAddressAndSubmit(
        &editAddressDialog, QString("io - new B"), new_address_b, QString(""));
    check_addbook_size(4);
    QTRY_COMPARE(table_view->model()->rowCount(), 3);

    auto search_line = address_book.findChild<QLineEdit*>("searchLineEdit");

    search_line->setText(r_label);
    QCOMPARE(table_view->model()->rowCount(), 0);

    search_line->setText(s_label);
    QCOMPARE(table_view->model()->rowCount(), 1);

    search_line->setText("io");
    QCOMPARE(table_view->model()->rowCount(), 2);

    // Check wildcard "?".
    search_line->setText("io?new");
    QCOMPARE(table_view->model()->rowCount(), 0);
    search_line->setText("io???new");
    QCOMPARE(table_view->model()->rowCount(), 2);

    // Check wildcard "*".
    search_line->setText("io*new");
    QCOMPARE(table_view->model()->rowCount(), 2);
    search_line->setText("*");
    QCOMPARE(table_view->model()->rowCount(), 3);

    search_line->setText(preexisting_r_address);
    QCOMPARE(table_view->model()->rowCount(), 0);

    search_line->setText(preexisting_s_address);
    QCOMPARE(table_view->model()->rowCount(), 1);

    search_line->setText(new_address_a);
    QCOMPARE(table_view->model()->rowCount(), 1);

    search_line->setText(new_address_b);
    QCOMPARE(table_view->model()->rowCount(), 1);

    search_line->setText("");
    QCOMPARE(table_view->model()->rowCount(), 3);

    auto* address_model = walletModel.getAddressTableModel();
    // Lookups use canonical destinations, not display spelling. Hidden and
    // empty-label entries must retain their actual address-book purpose.
    QCOMPARE(address_model->labelForAddress(preexisting_r_address.toUpper()), r_label);
    QVERIFY(address_model->purposeForAddress(preexisting_r_address.toUpper()) == wallet::AddressPurpose::RECEIVE);
    auto mixed_case = preexisting_r_address.toUpper();
    mixed_case[0] = mixed_case[0].toLower();
    QVERIFY(address_model->labelForAddress(mixed_case).isEmpty());
    QVERIFY(!address_model->purposeForAddress(mixed_case));
    QVERIFY(address_model->labelForAddress("not an address").isEmpty());
    QVERIFY(!address_model->purposeForAddress("not an address"));

    const auto [refund_dest, refund_address] = build_address();
    QVERIFY(wallet->SetAddressBook(refund_dest, "hidden refund", wallet::AddressPurpose::REFUND));
    QTRY_COMPARE(address_model->labelForAddress(refund_address), QString("hidden refund"));
    QVERIFY(address_model->purposeForAddress(refund_address) == wallet::AddressPurpose::REFUND);
    const int refund_row = address_model->lookupAddress(refund_address);
    QVERIFY(refund_row >= 0);
    QVERIFY(!address_model->data(address_model->index(refund_row, AddressTableModel::Label, {}), AddressTableModel::TypeRole).isValid());
    QVERIFY(address_model->setData(address_model->index(refund_row, AddressTableModel::Label, {}), "renamed refund", Qt::EditRole));
    QCOMPARE(address_model->labelForAddress(refund_address), QString("renamed refund"));
    QVERIFY(address_model->purposeForAddress(refund_address) == wallet::AddressPurpose::REFUND);

    // Successful mutations are immediately visible to lookups, even before
    // their queued notifications update table rows. Consecutive label/address
    // edits mirror QDataWidgetMapper::submit and must preserve the new label.
    const auto [immediate_dest, immediate_address] = build_address();
    const auto [moved_dest, moved_address] = build_address();
    QVERIFY(!address_model->addRow(AddressTableModel::Send, "immediate", immediate_address, OutputType::BECH32).isEmpty());
    QCOMPARE(address_model->labelForAddress(immediate_address), QString("immediate"));
    QVERIFY(address_model->purposeForAddress(immediate_address) == wallet::AddressPurpose::SEND);
    QTRY_VERIFY(address_model->lookupAddress(immediate_address) >= 0);
    const int immediate_row = address_model->lookupAddress(immediate_address);
    QVERIFY(address_model->setData(address_model->index(immediate_row, AddressTableModel::Label, {}), "preserved label", Qt::EditRole));
    QCOMPARE(address_model->labelForAddress(immediate_address), QString("preserved label"));
    QVERIFY(address_model->setData(address_model->index(immediate_row, AddressTableModel::Address, {}), moved_address, Qt::EditRole));
    QVERIFY(address_model->labelForAddress(immediate_address).isEmpty());
    QVERIFY(!address_model->purposeForAddress(immediate_address));
    QCOMPARE(address_model->labelForAddress(moved_address), QString("preserved label"));
    std::string moved_label;
    wallet::AddressPurpose moved_purpose;
    QVERIFY(walletModel.wallet().getAddress(moved_dest, &moved_label, &moved_purpose));
    QCOMPARE(QString::fromStdString(moved_label), QString("preserved label"));
    QVERIFY(moved_purpose == wallet::AddressPurpose::SEND);
    QTRY_VERIFY(address_model->lookupAddress(moved_address) >= 0);
    QVERIFY(address_model->removeRows(address_model->lookupAddress(moved_address), 1));
    QVERIFY(address_model->labelForAddress(moved_address).isEmpty());
    QVERIFY(!address_model->purposeForAddress(moved_address));
    QTRY_COMPARE(address_model->lookupAddress(moved_address), -1);

    // A backend duplicate can exist before its notification is consumed.
    // Duplicate checks remain authoritative and populate warning metadata.
    const auto [duplicate_dest, duplicate_address] = build_address();
    QVERIFY(wallet->SetAddressBook(duplicate_dest, "queued receive", wallet::AddressPurpose::RECEIVE));
    QVERIFY(address_model->addRow(AddressTableModel::Send, "ignored", duplicate_address, OutputType::BECH32).isEmpty());
    QCOMPARE(address_model->getEditStatus(), AddressTableModel::DUPLICATE_ADDRESS);
    QCOMPARE(address_model->labelForAddress(duplicate_address), QString("queued receive"));
    QVERIFY(address_model->purposeForAddress(duplicate_address) == wallet::AddressPurpose::RECEIVE);
    const auto [edit_duplicate_dest, edit_duplicate_address] = build_address();
    QVERIFY(wallet->SetAddressBook(edit_duplicate_dest, "queued edit duplicate", wallet::AddressPurpose::RECEIVE));
    QVERIFY(!address_model->setData(address_model->index(address_model->lookupAddress(preexisting_s_address), AddressTableModel::Address, {}), edit_duplicate_address, Qt::EditRole));
    QCOMPARE(address_model->getEditStatus(), AddressTableModel::DUPLICATE_ADDRESS);
    QCOMPARE(address_model->labelForAddress(edit_duplicate_address), QString("queued edit duplicate"));
    QVERIFY(address_model->purposeForAddress(edit_duplicate_address) == wallet::AddressPurpose::RECEIVE);

    // An older background notification must not overwrite a newer GUI edit.
    bool earlier_updated{false};
    std::thread earlier_update([&] {
        earlier_updated = wallet->SetAddressBook(s_key_dest, "earlier queued label", wallet::AddressPurpose::SEND);
    });
    earlier_update.join();
    QVERIFY(earlier_updated);
    QVERIFY(address_model->setData(address_model->index(address_model->lookupAddress(preexisting_s_address), AddressTableModel::Label, {}), "newest label", Qt::EditRole));
    QCOMPARE(address_model->labelForAddress(preexisting_s_address), QString("newest label"));
    QApplication::processEvents();
    QCOMPARE(address_model->labelForAddress(preexisting_s_address), QString("newest label"));

    // A later notification delivered inside the responsive wait must survive
    // the GUI continuation for edits, additions, removals and replacements.
    const auto [racing_dest, racing_address] = build_address();
    QVERIFY(wallet->SetAddressBook(racing_dest, "before edit", wallet::AddressPurpose::SEND));
    QTRY_VERIFY(address_model->lookupAddress(racing_address) >= 0);
    CheckLaterAddressUpdate(walletModel, *address_model, racing_dest, "submitted label", CT_UPDATED, [&] {
        return address_model->setData(address_model->index(address_model->lookupAddress(racing_address), AddressTableModel::Label, {}),
                                      "submitted label", Qt::EditRole);
    });
    const auto [racing_add_dest, racing_add_address] = build_address();
    CheckLaterAddressUpdate(walletModel, *address_model, racing_add_dest, "submitted addition", CT_NEW, [&] {
        return address_model->addRow(AddressTableModel::Send, "submitted addition", racing_add_address, OutputType::BECH32) == racing_add_address;
    });
    QTRY_VERIFY(address_model->lookupAddress(racing_add_address) >= 0);
    CheckLaterAddressUpdate(walletModel, *address_model, racing_add_dest, "", CT_DELETED, [&] {
        return address_model->removeRows(address_model->lookupAddress(racing_add_address), 1);
    });
    const auto [racing_move_dest, racing_move_address] = build_address();
    const auto [racing_target_dest, racing_target_address] = build_address();
    QVERIFY(wallet->SetAddressBook(racing_move_dest, "before replacement", wallet::AddressPurpose::SEND));
    QTRY_VERIFY(address_model->lookupAddress(racing_move_address) >= 0);
    CheckLaterAddressUpdate(walletModel, *address_model, racing_move_dest, "", CT_DELETED, [&] {
        return address_model->setData(address_model->index(address_model->lookupAddress(racing_move_address), AddressTableModel::Address, {}),
                                      racing_target_address, Qt::EditRole);
    });
    QCOMPARE(address_model->labelForAddress(racing_target_address), QString{"before replacement"});

    // Opening the PKHash-only selector leaves the old model attached to its
    // views. Both models must keep full lookup data and receive later changes,
    // while the selector must keep filtering newly notified non-PKHash rows.
    const CTxDestination legacy_dest{PKHash{GenerateRandomKey().GetPubKey()}};
    const auto legacy_address = QString::fromStdString(EncodeDestination(legacy_dest));
    QVERIFY(wallet->SetAddressBook(legacy_dest, "legacy", wallet::AddressPurpose::SEND));
    walletModel.refresh(/*pk_hash_only=*/true);
    auto* filtered_model = walletModel.getAddressTableModel();
    QVERIFY(filtered_model != address_model);
    QTRY_COMPARE(filtered_model->rowCount({}), 1);
    QTRY_COMPARE(filtered_model->labelForAddress(" \t" + legacy_address + " \n"), QString("legacy"));
    QVERIFY(filtered_model->labelForAddress(" " + preexisting_s_address + " ").isEmpty());
    // A queued row notification can arrive before the complete lookup snapshot.
    QTRY_COMPARE(filtered_model->labelForAddress(refund_address), QString("renamed refund"));
    QVERIFY(filtered_model->purposeForAddress(refund_address) == wallet::AddressPurpose::REFUND);
    QCOMPARE(filtered_model->labelForAddress(preexisting_s_address.toUpper()), QString("newest label"));
    QVERIFY(wallet->SetAddressBook(refund_dest, "updated hidden refund", wallet::AddressPurpose::REFUND));
    QTRY_COMPARE(address_model->labelForAddress(refund_address), QString("updated hidden refund"));
    QTRY_COMPARE(filtered_model->labelForAddress(refund_address), QString("updated hidden refund"));
    const auto [filtered_dest, filtered_address] = build_address();
    QVERIFY(wallet->SetAddressBook(filtered_dest, "", wallet::AddressPurpose::SEND));
    QTRY_VERIFY(filtered_model->purposeForAddress(filtered_address) == wallet::AddressPurpose::SEND);
    QVERIFY(filtered_model->labelForAddress(filtered_address).isEmpty());
    QVERIFY(filtered_model->lookupAddress(filtered_address) < 0);
    QCOMPARE(filtered_model->rowCount({}), 1);
    QVERIFY(wallet->DelAddressBook(refund_dest));
    QTRY_VERIFY(!address_model->purposeForAddress(refund_address));
    QTRY_VERIFY(!filtered_model->purposeForAddress(refund_address));
    QVERIFY(wallet->DelAddressBook(legacy_dest));
    QTRY_COMPARE(filtered_model->rowCount({}), 0);
    QVERIFY(!filtered_model->purposeForAddress(legacy_address));

    // Both opening another address selector and writing an address must let
    // the GUI run while validation owns cs_wallet. A timeout releases the
    // lock on regression so the test fails instead of hanging indefinitely.
    std::promise<void> locked;
    std::promise<void> release;
    auto released = release.get_future();
    std::atomic<bool> timed_out{false};
    std::thread holder([&] {
        LOCK(wallet->cs_wallet);
        locked.set_value();
        timed_out = released.wait_for(std::chrono::seconds{2}) != std::future_status::ready;
    });
    locked.get_future().wait();
    AddressTableModel background_model(&walletModel);
    bool gui_tick{false};
    QTimer release_timer;
    release_timer.setSingleShot(true);
    QObject::connect(&release_timer, &QTimer::timeout, [&] {
        gui_tick = true;
        release.set_value();
    });
    release_timer.start(20);
    const auto [responsive_dest, responsive_address] = build_address();
    const auto added = address_model->addRow(AddressTableModel::Send, "responsive", responsive_address, OutputType::BECH32);
    holder.join();
    QVERIFY(!timed_out.load());
    QVERIFY(gui_tick);
    QCOMPARE(added, responsive_address);
    QTRY_COMPARE(background_model.labelForAddress(responsive_address), QString("responsive"));
}

} // namespace

void AddressBookTests::addressBookTests()
{
    TestAddAddressesToSendBook(m_node);
}
