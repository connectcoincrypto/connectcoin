// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/transactionview.h>

#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/csvmodelwriter.h>
#include <qt/editaddressdialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactiondescdialog.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <node/interface_ui.h>

#include <chrono>
#include <optional>

#include <QApplication>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDoubleValidator>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPoint>
#include <QPointer>
#include <QScrollBar>
#include <qt/guipreferences.h>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

TransactionView::TransactionView(const PlatformStyle *platformStyle, QWidget *parent)
    : QWidget(parent), m_platform_style{platformStyle}
{
    // Build filter row
    setContentsMargins(0,0,0,0);

    QHBoxLayout *hlayout = new QHBoxLayout();
    hlayout->setContentsMargins(0,0,0,0);

    if (platformStyle->getUseExtraSpacing()) {
        hlayout->setSpacing(5);
        hlayout->addSpacing(26);
    } else {
        hlayout->setSpacing(0);
        hlayout->addSpacing(23);
    }

    dateWidget = new QComboBox(this);
    if (platformStyle->getUseExtraSpacing()) {
        dateWidget->setFixedWidth(121);
    } else {
        dateWidget->setFixedWidth(120);
    }
    dateWidget->addItem(tr("All"), All);
    dateWidget->addItem(tr("Today"), Today);
    dateWidget->addItem(tr("This week"), ThisWeek);
    dateWidget->addItem(tr("This month"), ThisMonth);
    dateWidget->addItem(tr("Last month"), LastMonth);
    dateWidget->addItem(tr("This year"), ThisYear);
    dateWidget->addItem(tr("Range…"), Range);
    hlayout->addWidget(dateWidget);

    typeWidget = new QComboBox(this);
    if (platformStyle->getUseExtraSpacing()) {
        typeWidget->setFixedWidth(121);
    } else {
        typeWidget->setFixedWidth(120);
    }

    typeWidget->addItem(tr("All"), TransactionFilterProxy::ALL_TYPES);
    typeWidget->addItem(tr("Received with"), TransactionFilterProxy::TYPE(TransactionRecord::RecvWithAddress) |
                                        TransactionFilterProxy::TYPE(TransactionRecord::RecvFromOther));
    typeWidget->addItem(tr("Sent to"), TransactionFilterProxy::TYPE(TransactionRecord::SendToAddress) |
                                  TransactionFilterProxy::TYPE(TransactionRecord::SendToOther));
    typeWidget->addItem(tr("Mined"), TransactionFilterProxy::TYPE(TransactionRecord::Generated));
    typeWidget->addItem(tr("Other"), TransactionFilterProxy::TYPE(TransactionRecord::Other));

    hlayout->addWidget(typeWidget);

    search_widget = new QLineEdit(this);
    search_widget->setPlaceholderText(tr("Enter address, P2C domain, transaction id, or label to search"));
    hlayout->addWidget(search_widget);

    amountWidget = new QLineEdit(this);
    amountWidget->setPlaceholderText(tr("Min amount"));
    if (platformStyle->getUseExtraSpacing()) {
        amountWidget->setFixedWidth(97);
    } else {
        amountWidget->setFixedWidth(100);
    }
    QDoubleValidator *amountValidator = new QDoubleValidator(0, 1e20, 8, this);
    QLocale amountLocale(QLocale::C);
    amountLocale.setNumberOptions(QLocale::RejectGroupSeparator);
    amountValidator->setLocale(amountLocale);
    amountWidget->setValidator(amountValidator);
    hlayout->addWidget(amountWidget);

    // Delay before filtering transactions
    static constexpr auto input_filter_delay{200ms};

    QTimer* amount_typing_delay = new QTimer(this);
    amount_typing_delay->setSingleShot(true);
    amount_typing_delay->setInterval(input_filter_delay);

    QTimer* prefix_typing_delay = new QTimer(this);
    prefix_typing_delay->setSingleShot(true);
    prefix_typing_delay->setInterval(input_filter_delay);

    QVBoxLayout *vlayout = new QVBoxLayout(this);
    vlayout->setContentsMargins(0,0,0,0);
    vlayout->setSpacing(0);

    transactionView = new QTableView(this);
    transactionView->setObjectName("transactionView");
    vlayout->addLayout(hlayout);
    vlayout->addWidget(createDateRangeWidget());
    vlayout->addWidget(transactionView);
    vlayout->setSpacing(0);
    int width = transactionView->verticalScrollBar()->sizeHint().width();
    // Cover scroll bar width with spacing
    if (platformStyle->getUseExtraSpacing()) {
        hlayout->addSpacing(width+2);
    } else {
        hlayout->addSpacing(width);
    }
    transactionView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    transactionView->setTabKeyNavigation(false);
    transactionView->setContextMenuPolicy(Qt::CustomContextMenu);
    transactionView->installEventFilter(this);
    transactionView->setAlternatingRowColors(true);
    transactionView->setSelectionBehavior(QAbstractItemView::SelectRows);
    transactionView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    transactionView->setSortingEnabled(true);
    transactionView->verticalHeader()->hide();

    GuiSettings settings;
    if (!transactionView->horizontalHeader()->restoreState(settings.value("TransactionViewHeaderState-2025").toByteArray())) {
        transactionView->setColumnWidth(TransactionTableModel::Status, STATUS_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Date, DATE_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Type, TYPE_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Amount, AMOUNT_MINIMUM_COLUMN_WIDTH);
        transactionView->horizontalHeader()->setMinimumSectionSize(MINIMUM_COLUMN_WIDTH);
        transactionView->horizontalHeader()->setStretchLastSection(true);
    }

    contextMenu = new QMenu(this);
    contextMenu->setObjectName("contextMenu");
    copyAddressAction = contextMenu->addAction(tr("&Copy address"), this, &TransactionView::copyAddress);
    copyLabelAction = contextMenu->addAction(tr("Copy &label"), this, &TransactionView::copyLabel);
    contextMenu->addAction(tr("Copy &amount"), this, &TransactionView::copyAmount);
    contextMenu->addAction(tr("Copy transaction &ID"), this, &TransactionView::copyTxID);
    contextMenu->addAction(tr("Copy &raw transaction"), this, &TransactionView::copyTxHex);
    contextMenu->addAction(tr("Copy full transaction &details"), this, &TransactionView::copyTxPlainText);
    contextMenu->addAction(tr("&Show transaction details"), this, &TransactionView::showDetails);
    contextMenu->addSeparator();
    bumpFeeAction = contextMenu->addAction(tr("Increase transaction &fee"));
    GUIUtil::ExceptionSafeConnect(bumpFeeAction, &QAction::triggered, this, &TransactionView::bumpFee);
    bumpFeeAction->setObjectName("bumpFeeAction");
    abandonAction = contextMenu->addAction(tr("A&bandon transaction"), this, &TransactionView::abandonTx);
    contextMenu->addAction(tr("&Edit address label"), this, &TransactionView::editLabel);

    connect(dateWidget, qOverload<int>(&QComboBox::activated), this, &TransactionView::chooseDate);
    connect(typeWidget, qOverload<int>(&QComboBox::activated), this, &TransactionView::chooseType);
    connect(amountWidget, &QLineEdit::textChanged, amount_typing_delay, qOverload<>(&QTimer::start));
    connect(amount_typing_delay, &QTimer::timeout, this, &TransactionView::changedAmount);
    connect(search_widget, &QLineEdit::textChanged, prefix_typing_delay, qOverload<>(&QTimer::start));
    connect(prefix_typing_delay, &QTimer::timeout, this, &TransactionView::changedSearch);

    connect(transactionView, &QTableView::doubleClicked, this, &TransactionView::doubleClicked);
    connect(transactionView, &QTableView::customContextMenuRequested, this, &TransactionView::contextualMenu);

    // Double-clicking on a transaction on the transaction history page shows details
    connect(this, &TransactionView::doubleClicked, this, &TransactionView::showDetails);
    // Highlight transaction after fee bump
    connect(this, &TransactionView::bumpedFee, [this](const Txid& txid) {
      focusTransaction(txid);
    });
}

TransactionView::~TransactionView()
{
    closeOpenedDialogs();
    GuiSettings settings;
    // Rename this cache when adding or removing columns.
    settings.setValue("TransactionViewHeaderState-2025", transactionView->horizontalHeader()->saveState());
}

void TransactionView::setModel(WalletModel *_model)
{
    this->model = _model;
    if(_model)
    {
        transactionProxyModel = new TransactionFilterProxy(this);
        transactionProxyModel->setSourceModel(_model->getTransactionTableModel());
        transactionProxyModel->setDynamicSortFilter(true);
        transactionProxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
        transactionProxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
        transactionProxyModel->setSortRole(Qt::EditRole);
        transactionView->setModel(transactionProxyModel);
        transactionView->sortByColumn(TransactionTableModel::Date, Qt::DescendingOrder);
        connect(_model->getTransactionTableModel(), &TransactionTableModel::confirmationsChanged,
                transactionView->viewport(), QOverload<>::of(&QWidget::update));

        if (_model->getOptionsModel())
        {
            // Add third party transaction URLs to context menu
            QStringList listUrls = _model->getOptionsModel()->getThirdPartyTxUrls().split("|", Qt::SkipEmptyParts);
            bool actions_created = false;
            for (int i = 0; i < listUrls.size(); ++i)
            {
                QString url = listUrls[i].trimmed();
                QString host = QUrl(url, QUrl::StrictMode).host();
                if (!host.isEmpty())
                {
                    if (!actions_created) {
                        contextMenu->addSeparator();
                        actions_created = true;
                    }
                    /*: Transactions table context menu action to show the
                        selected transaction in a third-party block explorer.
                        %1 is a stand-in argument for the URL of the explorer. */
                    contextMenu->addAction(tr("Show in %1").arg(host), [this, url] { openThirdPartyTxUrl(url); });
                }
            }
        }
    }
}

void TransactionView::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
}

void TransactionView::chooseDate(int idx)
{
    if (!transactionProxyModel) return;
    QDate current = QDate::currentDate();
    dateRangeWidget->setVisible(false);
    switch(dateWidget->itemData(idx).toInt())
    {
    case All:
        transactionProxyModel->setDateRange(
                std::nullopt,
                std::nullopt);
        break;
    case Today:
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(current),
                std::nullopt);
        break;
    case ThisWeek: {
        // Find last Monday
        QDate startOfWeek = current.addDays(-(current.dayOfWeek()-1));
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(startOfWeek),
                std::nullopt);

        } break;
    case ThisMonth:
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(QDate(current.year(), current.month(), 1)),
                std::nullopt);
        break;
    case LastMonth:
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(QDate(current.year(), current.month(), 1).addMonths(-1)),
                GUIUtil::StartOfDay(QDate(current.year(), current.month(), 1)));
        break;
    case ThisYear:
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(QDate(current.year(), 1, 1)),
                std::nullopt);
        break;
    case Range:
        dateRangeWidget->setVisible(true);
        dateRangeChanged();
        break;
    }
}

void TransactionView::chooseType(int idx)
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setTypeFilter(
        typeWidget->itemData(idx).toInt());
}

void TransactionView::changedSearch()
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setSearchString(search_widget->text());
}

void TransactionView::changedAmount()
{
    if(!transactionProxyModel)
        return;
    CAmount amount_parsed = 0;
    if (BitcoinUnits::parse(model->getOptionsModel()->getDisplayUnit(), amountWidget->text(), &amount_parsed)) {
        transactionProxyModel->setMinAmount(amount_parsed);
    }
    else
    {
        transactionProxyModel->setMinAmount(0);
    }
}

void TransactionView::exportClicked()
{
    if (!model || !model->getOptionsModel()) {
        return;
    }

    const QPointer<TransactionView> guard{this};
    GUIUtil::BackendOperationGuard operation;
    // CSV is currently the only supported format
    QString filename = GUIUtil::getSaveFileName(this,
        tr("Export Transaction History"), QString(),
        /*: Expanded name of the CSV file format.
            See: https://en.wikipedia.org/wiki/Comma-separated_values. */
        tr("Comma separated file") + QLatin1String(" (*.csv)"), nullptr);

    if (!guard || filename.isNull())
        return;

    CSVModelWriter writer(filename);

    // name, column, role
    writer.setModel(transactionProxyModel);
    writer.addColumn(tr("Confirmed"), 0, TransactionTableModel::ConfirmedRole);
    writer.addColumn(tr("Date"), 0, TransactionTableModel::DateRole);
    writer.addColumn(tr("Type"), TransactionTableModel::Type, Qt::EditRole);
    writer.addColumn(tr("Label"), 0, TransactionTableModel::LabelRole);
    writer.addColumn(tr("Address"), 0, TransactionTableModel::AddressRole);
    writer.addColumn(BitcoinUnits::getAmountColumnTitle(model->getOptionsModel()->getDisplayUnit()), 0, TransactionTableModel::FormattedAmountRole);
    writer.addColumn(tr("ID"), 0, TransactionTableModel::TxHashRole);

    const bool written = writer.write();
    if (!guard) return;
    if (!written) {
        Q_EMIT message(tr("Exporting Failed"), tr("There was an error trying to save the transaction history to %1.").arg(filename),
            CClientUIInterface::MSG_ERROR);
    }
    else {
        Q_EMIT message(tr("Exporting Successful"), tr("The transaction history was successfully saved to %1.").arg(filename),
            CClientUIInterface::MSG_INFORMATION);
    }
}

void TransactionView::contextualMenu(const QPoint &point)
{
    if (!model || !transactionView->selectionModel() || !transactionView->indexAt(point).isValid()) return;
    GUIUtil::BackendOperationGuard operation;
    const QPointer<TransactionView> guard{this};
    auto selection = GUIUtil::firstSelectedRow(transactionView);
    if (!selection.isValid())
        return;

    // If the hash from the TxHashRole (QVariant / QString) is invalid, exit
    QString hashQStr = selection.data(TransactionTableModel::TxHashRole).toString();
    std::optional<Txid> maybeHash = Txid::FromHex(hashQStr.toStdString());
    if (!maybeHash)
        return;

    Txid hash = *maybeHash;
    const auto capabilities = GUIUtil::WaitForBackendTask(model->requestWalletData([hash](interfaces::Wallet& wallet) {
        return std::pair{wallet.transactionCanBeAbandoned(hash), wallet.transactionCanBeBumped(hash)};
    }), this);
    // The event loop remained live while querying. Do not attach capabilities
    // for a removed/replaced transaction to a different current selection.
    if (!guard) return;
    selection = GUIUtil::firstSelectedRow(transactionView);
    if (!selection.isValid() || selection.data(TransactionTableModel::TxHashRole).toString() != hashQStr) return;
    abandonAction->setEnabled(capabilities.first);
    bumpFeeAction->setEnabled(capabilities.second);
    copyAddressAction->setEnabled(GUIUtil::hasEntryData(transactionView, 0, TransactionTableModel::AddressRole));
    copyLabelAction->setEnabled(GUIUtil::hasEntryData(transactionView, 0, TransactionTableModel::LabelRole));

    GUIUtil::PopupMenu(contextMenu, transactionView->viewport()->mapToGlobal(point));
}

void TransactionView::abandonTx()
{
    if(!model || !transactionView || !transactionView->selectionModel())
        return;
    GUIUtil::BackendOperationGuard operation;
    const auto selection = GUIUtil::firstSelectedRow(transactionView);
    if (!selection.isValid()) return;

    // get the hash from the TxHashRole (QVariant / QString)
    QString hashQStr = selection.data(TransactionTableModel::TxHashRole).toString();
    Txid hash = Txid::FromHex(hashQStr.toStdString()).value();

    // Abandon the wallet transaction over the walletModel
    GUIUtil::WaitForBackendTask(model->requestWalletData([hash](interfaces::Wallet& wallet) {
        return wallet.abandonTransaction(hash);
    }), this);
}

void TransactionView::bumpFee([[maybe_unused]] bool checked)
{
    if(!model || !transactionView || !transactionView->selectionModel())
        return;
    GUIUtil::BackendOperationGuard operation;
    const QPointer<TransactionView> guard{this};
    const auto selection = GUIUtil::firstSelectedRow(transactionView);
    if (!selection.isValid()) return;

    // get the hash from the TxHashRole (QVariant / QString)
    QString hashQStr = selection.data(TransactionTableModel::TxHashRole).toString();
    Txid hash = Txid::FromHex(hashQStr.toStdString()).value();

    // Bump tx fee over the walletModel
    Txid newHash;
    if (model->bumpFee(hash, newHash) && guard) {
        // Update the table
        transactionView->selectionModel()->clearSelection();
        model->getTransactionTableModel()->updateTransaction(hashQStr, CT_UPDATED, true);

        qApp->processEvents();
        if (!guard) return;
        Q_EMIT bumpedFee(newHash);
    }
}

void TransactionView::copyAddress()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::AddressRole);
}

void TransactionView::copyLabel()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::LabelRole);
}

void TransactionView::copyAmount()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::FormattedAmountRole);
}

void TransactionView::copyTxID()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxHashRole);
}

void TransactionView::copyTxHex()
{
    GUIUtil::BackendOperationGuard operation;
    if (!model || !transactionView->selectionModel()) return;
    const auto selection = GUIUtil::firstSelectedRow(transactionView);
    if (!selection.isValid()) return;
    auto query = model->getTransactionTableModel()->requestTxHex(transactionProxyModel->mapToSource(selection));
    GUIUtil::setClipboard(GUIUtil::WaitForBackendTask(std::move(query), this));
}

void TransactionView::copyTxPlainText()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxPlainTextRole);
}

void TransactionView::editLabel()
{
    if(!transactionView->selectionModel() ||!model)
        return;
    const auto selection = GUIUtil::firstSelectedRow(transactionView);
    if (selection.isValid())
    {
        AddressTableModel *addressBook = model->getAddressTableModel();
        if(!addressBook)
            return;
        QString address = selection.data(TransactionTableModel::AddressRole).toString();
        if(address.isEmpty())
        {
            // If this transaction has no associated address, exit
            return;
        }
        // Is address in address book? Address book can miss address when a transaction is
        // sent from outside the UI.
        int idx = addressBook->lookupAddress(address);
        if(idx != -1)
        {
            // Edit sending / receiving address
            QModelIndex modelIdx = addressBook->index(idx, 0, QModelIndex());
            // Determine type of address, launch appropriate editor dialog type
            QString type = modelIdx.data(AddressTableModel::TypeRole).toString();

            auto dlg = new EditAddressDialog(
                type == AddressTableModel::Receive
                ? EditAddressDialog::EditReceivingAddress
                : EditAddressDialog::EditSendingAddress, this);
            dlg->setModel(addressBook);
            dlg->loadRow(idx);
            GUIUtil::ShowModalDialogAsynchronously(dlg);
        }
        else
        {
            // Add sending address
            auto dlg = new EditAddressDialog(EditAddressDialog::NewSendingAddress,
                this);
            dlg->setModel(addressBook);
            dlg->setAddress(address);
            GUIUtil::ShowModalDialogAsynchronously(dlg);
        }
    }
}

void TransactionView::showDetails()
{
    GUIUtil::BackendOperationGuard operation;
    const QPointer<TransactionView> guard{this};
    if(!model || !transactionView->selectionModel())
        return;
    const auto selection = GUIUtil::firstSelectedRow(transactionView);
    if (selection.isValid())
    {
        const auto txid = selection.data(TransactionTableModel::TxHashRole).toString();
        auto query = model->getTransactionTableModel()->requestTxDescription(transactionProxyModel->mapToSource(selection));
        const auto description = GUIUtil::WaitForBackendTask(std::move(query), this);
        if (!guard) return;
        TransactionDescDialog *dlg = new TransactionDescDialog(txid, description, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        m_opened_dialogs.append(dlg);
        connect(dlg, &QObject::destroyed, this, [this, dlg] {
            m_opened_dialogs.removeOne(dlg);
        });
        dlg->show();
    }
}

void TransactionView::openThirdPartyTxUrl(QString url)
{
    if(!transactionView || !transactionView->selectionModel())
        return;
    const auto selection = GUIUtil::firstSelectedRow(transactionView);
    if (selection.isValid())
         QDesktopServices::openUrl(QUrl::fromUserInput(url.replace("%s", selection.data(TransactionTableModel::TxHashRole).toString())));
}

QWidget *TransactionView::createDateRangeWidget()
{
    dateRangeWidget = new QFrame();
    dateRangeWidget->setFrameStyle(static_cast<int>(QFrame::Panel) | static_cast<int>(QFrame::Raised));
    dateRangeWidget->setContentsMargins(1,1,1,1);
    QHBoxLayout *layout = new QHBoxLayout(dateRangeWidget);
    layout->setContentsMargins(0,0,0,0);
    layout->addSpacing(23);
    layout->addWidget(new QLabel(tr("Range:")));

    dateFrom = new QDateTimeEdit(this);
    dateFrom->setDisplayFormat("dd/MM/yy");
    dateFrom->setCalendarPopup(true);
    dateFrom->setMinimumWidth(100);
    dateFrom->setDate(QDate::currentDate().addDays(-7));
    layout->addWidget(dateFrom);
    layout->addWidget(new QLabel(tr("to")));

    dateTo = new QDateTimeEdit(this);
    dateTo->setDisplayFormat("dd/MM/yy");
    dateTo->setCalendarPopup(true);
    dateTo->setMinimumWidth(100);
    dateTo->setDate(QDate::currentDate());
    layout->addWidget(dateTo);
    layout->addStretch();

    // Hide by default
    dateRangeWidget->setVisible(false);

    // Notify on change
    connect(dateFrom, &QDateTimeEdit::dateChanged, this, &TransactionView::dateRangeChanged);
    connect(dateTo, &QDateTimeEdit::dateChanged, this, &TransactionView::dateRangeChanged);

    return dateRangeWidget;
}

void TransactionView::dateRangeChanged()
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setDateRange(
            GUIUtil::StartOfDay(dateFrom->date()),
            GUIUtil::StartOfDay(dateTo->date()).addDays(1));
}

void TransactionView::focusTransaction(const QModelIndex &idx)
{
    if(!transactionProxyModel)
        return;
    QModelIndex targetIdx = transactionProxyModel->mapFromSource(idx);
    transactionView->scrollTo(targetIdx);
    transactionView->setCurrentIndex(targetIdx);
    transactionView->setFocus();
}

void TransactionView::focusTransaction(const Txid& txid)
{
    if (!transactionProxyModel)
        return;

    const QModelIndexList results = model->getTransactionTableModel()->indexesForTransaction(txid);
    QList<int> rows;
    rows.reserve(results.size());
    QModelIndex first_target;
    QModelIndex last_target;
    for (const QModelIndex& index : results) {
        const QModelIndex targetIndex = transactionProxyModel->mapFromSource(index);
        if (!targetIndex.isValid()) continue; // An output can be filtered out.
        if (!first_target.isValid()) first_target = targetIndex;
        last_target = targetIndex;
        rows.append(targetIndex.row());
    }

    // One transaction can have thousands of output rows. Select once instead of
    // repeatedly merging a growing selection and relaying one signal per row.
    // Proxy ordering can interleave unrelated transactions, so merge only
    // increasing adjacent rows, retaining the original selection order for
    // first-row copy actions instead of sorting or spanning unrelated rows.
    QItemSelection selection;
    for (qsizetype begin{0}; begin < rows.size();) {
        qsizetype end = begin + 1;
        while (end < rows.size() && rows[end] == rows[end - 1] + 1) ++end;
        selection.select(transactionProxyModel->index(rows[begin], 0),
                         transactionProxyModel->index(rows[end - 1], 0));
        begin = end;
    }
    transactionView->setFocus();
    transactionView->selectionModel()->select(selection,
        QItemSelectionModel::Rows | QItemSelectionModel::ClearAndSelect);
    if (first_target.isValid()) {
        // Scroll to the original endpoint rows, not once per destination.
        // The first scroll needs repeating for ascending date ordering.
        transactionView->scrollTo(first_target);
        transactionView->scrollTo(first_target);
        if (last_target != first_target) transactionView->scrollTo(last_target);
    }
}

// Need to override default Ctrl+C action for amount as default behaviour is just to copy DisplayRole text
bool TransactionView::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_C && ke->modifiers().testFlag(Qt::ControlModifier))
        {
             GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxPlainTextRole);
             return true;
        }
    }
    if (event->type() == QEvent::EnabledChange) {
        if (!isEnabled()) {
            closeOpenedDialogs();
        }
    }
    return QWidget::eventFilter(obj, event);
}

void TransactionView::closeOpenedDialogs()
{
    // close all dialogs opened from this view
    const auto dialogs = m_opened_dialogs;
    m_opened_dialogs.clear();
    for (QDialog* dlg : dialogs) {
        dlg->close();
    }
}
