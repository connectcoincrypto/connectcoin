// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/coincontroldialog.h>
#include <qt/forms/ui_coincontroldialog.h>

#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <interfaces/node.h>
#include <key_io.h>
#include <policy/policy.h>
#include <wallet/coincontrol.h>
#include <wallet/coinselection.h>
#include <wallet/wallet.h>

#include <chrono>
#include <future>
#include <optional>
#include <set>

#include <QApplication>
#include <QCheckBox>
#include <QCursor>
#include <QDebug>
#include <QDialogButtonBox>
#include <QFlags>
#include <QIcon>
#include <QPointer>
#include <QScopeGuard>
#include <qt/guipreferences.h>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>

using wallet::CCoinControl;

QList<CAmount> CoinControlDialog::payAmounts;
bool CoinControlDialog::fSubtractFeeFromAmount = false;

namespace {
struct CoinControlInputs {
    CCoinControl control;
    QList<CAmount> amounts;
    bool subtract_fee{false};

    bool matches(const CoinControlInputs& other) const
    {
        return amounts == other.amounts && subtract_fee == other.subtract_fee &&
            control.ListSelected() == other.control.ListSelected() &&
            control.m_feerate == other.control.m_feerate &&
            control.fOverrideFeeRate == other.control.fOverrideFeeRate &&
            control.m_confirm_target == other.control.m_confirm_target &&
            control.m_signal_bip125_rbf == other.control.m_signal_bip125_rbf &&
            control.m_fee_mode == other.control.m_fee_mode;
    }
};

struct CoinControlStats {
    CAmount pay_amount{0}, amount{0}, fee{0}, after_fee{0}, change{0};
    unsigned int bytes{0}, quantity{0};
    std::vector<COutPoint> spent;
};

CoinControlStats CalculateCoinControlStats(interfaces::Wallet& wallet, interfaces::Node& node, const CoinControlInputs& inputs);

// This state belongs to the dialog, not to a CCoinControl reference: the send
// page can replace its coin control or wallet while a query is in flight.
class CoinControlLabelState : public QObject
{
public:
    explicit CoinControlLabelState(QDialog* dialog) : QObject(dialog), timer(this)
    {
        setObjectName("coinControlLabelState");
        timer.setInterval(100);
        connect(&timer, &QTimer::timeout, this, [this] { poll(); });
    }

    QPointer<WalletModel> model;
    CoinControlInputs inputs;
    bool initialized{false}, dirty{false}, delivering{false};
    uint64_t revision{0}, inflight_revision{0};
    std::optional<CoinControlStats> result;
    std::future<CoinControlStats> query;
    QTimer timer;

    void request(WalletModel* next_model, CoinControlInputs next_inputs)
    {
        if (model != next_model) {
            model = next_model;
            query = {}; // Packaged-task futures never wait on destruction.
            initialized = false;
            delivering = false;
        }
        if (!initialized || !inputs.matches(next_inputs)) {
            inputs = std::move(next_inputs);
            initialized = true;
            ++revision;
            result.reset();
            dirty = true;
            delivering = false;
        }
        if (delivering) {
            delivering = false;
            return; // Consume the result without starting a refresh loop.
        }
        if (!query.valid()) dirty = true;
        if (!timer.isActive()) timer.start();
        if (!query.valid()) poll();
    }

    void poll()
    {
        if (!model) {
            query = {};
            timer.stop();
            return;
        }
        bool publish{false};
        if (query.valid()) {
            if (query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
            try {
                auto snapshot{query.get()};
                if (inflight_revision == revision) {
                    result = std::move(snapshot);
                    publish = true;
                }
            } catch (const std::exception& error) {
                qWarning() << "Coin control statistics failed:" << error.what();
                dirty = true;
            }
        }
        if (dirty) {
            try {
                query = model->requestWalletData([inputs = inputs, node = &model->node()](interfaces::Wallet& wallet) {
                    return CalculateCoinControlStats(wallet, *node, inputs);
                });
                inflight_revision = revision;
                dirty = false;
            } catch (const std::exception& error) {
                qWarning() << "Unable to request coin control statistics:" << error.what();
            }
        } else {
            timer.stop();
        }
        if (publish) {
            delivering = true;
            // Resolve the current control/model on the GUI thread. No worker
            // captures this dialog, its controls, or its CCoinControl object.
            QMetaObject::invokeMethod(parent(), "coinControlUpdateLabels", Qt::QueuedConnection);
        }
    }
};
} // namespace

class CoinControlViewState
{
public:
    struct Coin {
        COutPoint outpoint;
        CAmount amount;
        int64_t time;
        int depth;
        QString address;
        QString txid;
    };
    struct Group {
        QString address;
        std::vector<Coin> coins;
    };
    struct Snapshot {
        std::vector<Group> groups;
        std::set<COutPoint> locked;
    };
    explicit CoinControlViewState(QObject* parent) : timer(parent) {}
    std::optional<Snapshot> cached;
    std::future<Snapshot> query;
    uint64_t revision{0}, inflight_revision{0};
    bool dirty{false}, mutation_active{false}, render_pending{false};
    QTimer timer;
};

bool CCoinControlWidgetItem::operator<(const QTreeWidgetItem &other) const {
    int column = treeWidget()->sortColumn();
    if (column == CoinControlDialog::COLUMN_AMOUNT || column == CoinControlDialog::COLUMN_DATE || column == CoinControlDialog::COLUMN_CONFIRMATIONS)
        return data(column, Qt::UserRole).toLongLong() < other.data(column, Qt::UserRole).toLongLong();
    return QTreeWidgetItem::operator<(other);
}

CoinControlDialog::CoinControlDialog(CCoinControl& coin_control, WalletModel* _model, const PlatformStyle *_platformStyle, QWidget *parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::CoinControlDialog),
    m_coin_control(coin_control),
    model(_model),
    platformStyle(_platformStyle),
    m_view(std::make_unique<CoinControlViewState>(this))
{
    ui->setupUi(this);
    m_view->timer.setInterval(100);
    connect(&m_view->timer, &QTimer::timeout, this, &CoinControlDialog::pollView);

    // context menu
    contextMenu = new QMenu(this);
    contextMenu->addAction(tr("&Copy address"), this, &CoinControlDialog::copyAddress);
    contextMenu->addAction(tr("Copy &label"), this, &CoinControlDialog::copyLabel);
    contextMenu->addAction(tr("Copy &amount"), this, &CoinControlDialog::copyAmount);
    m_copy_transaction_outpoint_action = contextMenu->addAction(tr("Copy transaction &ID and output index"), this, &CoinControlDialog::copyTransactionOutpoint);
    contextMenu->addSeparator();
    lockAction = contextMenu->addAction(tr("L&ock unspent"), this, &CoinControlDialog::lockCoin);
    unlockAction = contextMenu->addAction(tr("&Unlock unspent"), this, &CoinControlDialog::unlockCoin);
    connect(ui->treeWidget, &QWidget::customContextMenuRequested, this, &CoinControlDialog::showMenu);

    // clipboard actions
    QAction *clipboardQuantityAction = new QAction(tr("Copy quantity"), this);
    QAction *clipboardAmountAction = new QAction(tr("Copy amount"), this);
    QAction *clipboardFeeAction = new QAction(tr("Copy fee"), this);
    QAction *clipboardAfterFeeAction = new QAction(tr("Copy after fee"), this);
    QAction *clipboardBytesAction = new QAction(tr("Copy bytes"), this);
    QAction *clipboardChangeAction = new QAction(tr("Copy change"), this);

    connect(clipboardQuantityAction, &QAction::triggered, this, &CoinControlDialog::clipboardQuantity);
    connect(clipboardAmountAction, &QAction::triggered, this, &CoinControlDialog::clipboardAmount);
    connect(clipboardFeeAction, &QAction::triggered, this, &CoinControlDialog::clipboardFee);
    connect(clipboardAfterFeeAction, &QAction::triggered, this, &CoinControlDialog::clipboardAfterFee);
    connect(clipboardBytesAction, &QAction::triggered, this, &CoinControlDialog::clipboardBytes);
    connect(clipboardChangeAction, &QAction::triggered, this, &CoinControlDialog::clipboardChange);

    ui->labelCoinControlQuantity->addAction(clipboardQuantityAction);
    ui->labelCoinControlAmount->addAction(clipboardAmountAction);
    ui->labelCoinControlFee->addAction(clipboardFeeAction);
    ui->labelCoinControlAfterFee->addAction(clipboardAfterFeeAction);
    ui->labelCoinControlBytes->addAction(clipboardBytesAction);
    ui->labelCoinControlChange->addAction(clipboardChangeAction);

    // toggle tree/list mode
    connect(ui->radioTreeMode, &QRadioButton::toggled, this, &CoinControlDialog::radioTreeMode);
    connect(ui->radioListMode, &QRadioButton::toggled, this, &CoinControlDialog::radioListMode);

    // click on checkbox
    connect(ui->treeWidget, &QTreeWidget::itemChanged, this, &CoinControlDialog::viewItemChanged);

    // click on header
    ui->treeWidget->header()->setSectionsClickable(true);
    connect(ui->treeWidget->header(), &QHeaderView::sectionClicked, this, &CoinControlDialog::headerSectionClicked);

    // ok button
    connect(ui->buttonBox, &QDialogButtonBox::clicked, this, &CoinControlDialog::buttonBoxClicked);

    // (un)select all
    connect(ui->pushButtonSelectAll, &QPushButton::clicked, this, &CoinControlDialog::buttonSelectAllClicked);

    ui->treeWidget->setColumnWidth(COLUMN_CHECKBOX, 84);
    ui->treeWidget->setColumnWidth(COLUMN_AMOUNT, 110);
    ui->treeWidget->setColumnWidth(COLUMN_LABEL, 190);
    ui->treeWidget->setColumnWidth(COLUMN_ADDRESS, 320);
    ui->treeWidget->setColumnWidth(COLUMN_DATE, 130);
    ui->treeWidget->setColumnWidth(COLUMN_CONFIRMATIONS, 110);

    // default view is sorted by amount desc
    sortView(COLUMN_AMOUNT, Qt::DescendingOrder);

    // restore list mode and sortorder as a convenience feature
    GuiSettings settings;
    if (settings.contains("nCoinControlMode") && !settings.value("nCoinControlMode").toBool())
        ui->radioTreeMode->click();
    if (settings.contains("nCoinControlSortColumn") && settings.contains("nCoinControlSortOrder"))
        sortView(settings.value("nCoinControlSortColumn").toInt(), (static_cast<Qt::SortOrder>(settings.value("nCoinControlSortOrder").toInt())));

    GUIUtil::handleCloseWindowShortcut(this);

    if(_model->getOptionsModel() && _model->getAddressTableModel())
    {
        updateView();
        updateLabelLocked();
        CoinControlDialog::updateLabels(m_coin_control, _model, this);
    }
}

CoinControlDialog::~CoinControlDialog()
{
    GuiSettings settings;
    settings.setValue("nCoinControlMode", ui->radioListMode->isChecked());
    settings.setValue("nCoinControlSortColumn", sortColumn);
    settings.setValue("nCoinControlSortOrder", (int)sortOrder);

    delete ui;
}

// ok button
void CoinControlDialog::buttonBoxClicked(QAbstractButton* button)
{
    if (ui->buttonBox->buttonRole(button) == QDialogButtonBox::AcceptRole)
        done(QDialog::Accepted); // closes the dialog
}

// (un)select all
void CoinControlDialog::buttonSelectAllClicked()
{
    Qt::CheckState state = Qt::Checked;
    for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++)
    {
        if (ui->treeWidget->topLevelItem(i)->checkState(COLUMN_CHECKBOX) != Qt::Unchecked)
        {
            state = Qt::Unchecked;
            break;
        }
    }
    ui->treeWidget->setEnabled(false);
    for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++)
            if (ui->treeWidget->topLevelItem(i)->checkState(COLUMN_CHECKBOX) != state)
                ui->treeWidget->topLevelItem(i)->setCheckState(COLUMN_CHECKBOX, state);
    ui->treeWidget->setEnabled(true);
    if (state == Qt::Unchecked)
        m_coin_control.UnSelectAll(); // just to be sure
    CoinControlDialog::updateLabels(m_coin_control, model, this);
}

// context menu
void CoinControlDialog::showMenu(const QPoint &point)
{
    const QPointer<CoinControlDialog> guard{this};
    QTreeWidgetItem *item = ui->treeWidget->itemAt(point);
    if(item)
    {
        contextMenuItem = item;

        // disable some items (like Copy Transaction ID, lock, unlock) for tree roots in context menu
        auto txid{Txid::FromHex(item->data(COLUMN_ADDRESS, TxHashRole).toString().toStdString())};
        if (txid) { // a valid txid means this is a child node, and not a parent node in tree mode
            m_copy_transaction_outpoint_action->setEnabled(true);
            if (m_view->cached && m_view->cached->locked.contains(COutPoint(*txid, item->data(COLUMN_ADDRESS, VOutRole).toUInt()))) {
                lockAction->setEnabled(false);
                unlockAction->setEnabled(true);
            } else {
                lockAction->setEnabled(true);
                unlockAction->setEnabled(false);
            }
        } else { // this means click on parent node in tree mode -> disable all
            m_copy_transaction_outpoint_action->setEnabled(false);
            lockAction->setEnabled(false);
            unlockAction->setEnabled(false);
        }

        // show context menu
        contextMenu->exec(QCursor::pos());
        if (!guard) return;
        // Copying a value must not rebuild and sort every UTXO. Only a
        // snapshot or appearance change deferred by the open menu needs it.
        if (m_view->render_pending) renderView();
    }
}

// context menu action: copy amount
void CoinControlDialog::copyAmount()
{
    GUIUtil::setClipboard(BitcoinUnits::removeSpaces(contextMenuItem->text(COLUMN_AMOUNT)));
}

// context menu action: copy label
void CoinControlDialog::copyLabel()
{
    if (ui->radioTreeMode->isChecked() && contextMenuItem->text(COLUMN_LABEL).length() == 0 && contextMenuItem->parent())
        GUIUtil::setClipboard(contextMenuItem->parent()->text(COLUMN_LABEL));
    else
        GUIUtil::setClipboard(contextMenuItem->text(COLUMN_LABEL));
}

// context menu action: copy address
void CoinControlDialog::copyAddress()
{
    if (ui->radioTreeMode->isChecked() && contextMenuItem->text(COLUMN_ADDRESS).length() == 0 && contextMenuItem->parent())
        GUIUtil::setClipboard(contextMenuItem->parent()->text(COLUMN_ADDRESS));
    else
        GUIUtil::setClipboard(contextMenuItem->text(COLUMN_ADDRESS));
}

// context menu action: copy transaction id and vout index
void CoinControlDialog::copyTransactionOutpoint()
{
    const QString address = contextMenuItem->data(COLUMN_ADDRESS, TxHashRole).toString();
    const QString vout = contextMenuItem->data(COLUMN_ADDRESS, VOutRole).toString();
    const QString outpoint = QString("%1:%2").arg(address).arg(vout);

    GUIUtil::setClipboard(outpoint);
}

// context menu action: lock coin
void CoinControlDialog::lockCoin()
{
    if (!model || !contextMenuItem) return;
    GUIUtil::BackendOperationGuard operation;
    const QPointer<CoinControlDialog> guard{this};
    if (contextMenuItem->checkState(COLUMN_CHECKBOX) == Qt::Checked)
        contextMenuItem->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);

    COutPoint outpt(Txid::FromHex(contextMenuItem->data(COLUMN_ADDRESS, TxHashRole).toString().toStdString()).value(), contextMenuItem->data(COLUMN_ADDRESS, VOutRole).toUInt());
    {
        m_view->mutation_active = true;
        const auto mutation = qScopeGuard([guard] { if (guard) guard->m_view->mutation_active = false; });
        ++m_view->revision;
        const bool locked = GUIUtil::WaitForBackendTask(model->requestWalletData([outpt](interfaces::Wallet& wallet) {
            return wallet.lockCoin(outpt, /*write_to_db=*/true);
        }), this);
        if (!guard) return;
        if (locked && m_view->cached) m_view->cached->locked.insert(outpt);
    }
    updateView();
}

// context menu action: unlock coin
void CoinControlDialog::unlockCoin()
{
    if (!model || !contextMenuItem) return;
    GUIUtil::BackendOperationGuard operation;
    const QPointer<CoinControlDialog> guard{this};
    COutPoint outpt(Txid::FromHex(contextMenuItem->data(COLUMN_ADDRESS, TxHashRole).toString().toStdString()).value(), contextMenuItem->data(COLUMN_ADDRESS, VOutRole).toUInt());
    {
        m_view->mutation_active = true;
        const auto mutation = qScopeGuard([guard] { if (guard) guard->m_view->mutation_active = false; });
        ++m_view->revision;
        const bool unlocked = GUIUtil::WaitForBackendTask(model->requestWalletData([outpt](interfaces::Wallet& wallet) {
            return wallet.unlockCoin(outpt);
        }), this);
        if (!guard) return;
        if (unlocked && m_view->cached) m_view->cached->locked.erase(outpt);
    }
    updateView();
}

// copy label "Quantity" to clipboard
void CoinControlDialog::clipboardQuantity()
{
    GUIUtil::setClipboard(ui->labelCoinControlQuantity->text());
}

// copy label "Amount" to clipboard
void CoinControlDialog::clipboardAmount()
{
    GUIUtil::setClipboard(ui->labelCoinControlAmount->text().left(ui->labelCoinControlAmount->text().indexOf(" ")));
}

// copy label "Fee" to clipboard
void CoinControlDialog::clipboardFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlFee->text().left(ui->labelCoinControlFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// copy label "After fee" to clipboard
void CoinControlDialog::clipboardAfterFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlAfterFee->text().left(ui->labelCoinControlAfterFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// copy label "Bytes" to clipboard
void CoinControlDialog::clipboardBytes()
{
    GUIUtil::setClipboard(ui->labelCoinControlBytes->text().replace(ASYMP_UTF8, ""));
}

// copy label "Change" to clipboard
void CoinControlDialog::clipboardChange()
{
    GUIUtil::setClipboard(ui->labelCoinControlChange->text().left(ui->labelCoinControlChange->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// treeview: sort
void CoinControlDialog::sortView(int column, Qt::SortOrder order)
{
    sortColumn = column;
    sortOrder = order;
    ui->treeWidget->sortItems(column, order);
    ui->treeWidget->header()->setSortIndicator(sortColumn, sortOrder);
}

// treeview: clicked on header
void CoinControlDialog::headerSectionClicked(int logicalIndex)
{
    if (logicalIndex == COLUMN_CHECKBOX) // click on most left column -> do nothing
    {
        ui->treeWidget->header()->setSortIndicator(sortColumn, sortOrder);
    }
    else
    {
        if (sortColumn == logicalIndex)
            sortOrder = ((sortOrder == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder);
        else
        {
            sortColumn = logicalIndex;
            sortOrder = ((sortColumn == COLUMN_LABEL || sortColumn == COLUMN_ADDRESS) ? Qt::AscendingOrder : Qt::DescendingOrder); // if label or address then default => asc, else default => desc
        }

        sortView(sortColumn, sortOrder);
    }
}

// toggle tree mode
void CoinControlDialog::radioTreeMode(bool checked)
{
    if (checked && model)
        renderView();
}

// toggle list mode
void CoinControlDialog::radioListMode(bool checked)
{
    if (checked && model)
        renderView();
}

// checkbox clicked by user
void CoinControlDialog::viewItemChanged(QTreeWidgetItem* item, int column)
{
    if (column != COLUMN_CHECKBOX) return;
    auto txid{Txid::FromHex(item->data(COLUMN_ADDRESS, TxHashRole).toString().toStdString())};
    if (txid) { // a valid txid means this is a child node, and not a parent node in tree mode
        COutPoint outpt(*txid, item->data(COLUMN_ADDRESS, VOutRole).toUInt());

        if (item->checkState(COLUMN_CHECKBOX) == Qt::Unchecked)
            m_coin_control.UnSelect(outpt);
        else if (item->isDisabled()) // locked (this happens if "check all" through parent node)
            item->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);
        else
            m_coin_control.Select(outpt);

        // Checking an auto-tristate group emits one change per child. Taking
        // a full selected-coin snapshot for every child makes one click
        // quadratic; deliver only the final selection in this GUI turn.
        if (ui->treeWidget->isEnabled() && !m_labels_queued) {
            m_labels_queued = true;
            QMetaObject::invokeMethod(this, [this] {
                if (!m_labels_queued) return;
                coinControlUpdateLabels();
            }, Qt::QueuedConnection);
        }
    }
}

// shows count of locked unspent outputs
void CoinControlDialog::updateLabelLocked()
{
    const size_t count{m_view->cached ? m_view->cached->locked.size() : 0};
    if (count > 0)
    {
       ui->labelLocked->setText(tr("(%1 locked)").arg(count));
       ui->labelLocked->setVisible(true);
    }
    else ui->labelLocked->setVisible(false);
}

namespace {
CoinControlStats CalculateCoinControlStats(interfaces::Wallet& wallet, interfaces::Node& node, const CoinControlInputs& inputs)
{
    // nPayAmount
    CAmount nPayAmount = 0;
    for (const CAmount &amount : inputs.amounts) {
        nPayAmount += amount;
    }

    CAmount nAmount             = 0;
    CAmount nPayFee             = 0;
    CAmount nAfterFee           = 0;
    CAmount nChange             = 0;
    unsigned int nBytes         = 0;
    unsigned int nBytesInputs   = 0;
    unsigned int nQuantity      = 0;
    bool fWitness               = false;

    auto vCoinControl{inputs.control.ListSelected()};
    std::vector<COutPoint> spent;

    size_t i = 0;
    for (const auto& out : wallet.getCoins(vCoinControl)) {
        const COutPoint& outpt = vCoinControl[i++];
        if (out.depth_in_main_chain < 0) continue;

        // unselect already spent, very unlikely scenario, this could happen
        // when selected are spent elsewhere, like rpc or another computer
        if (out.is_spent)
        {
            spent.push_back(outpt);
            continue;
        }

        // Quantity
        nQuantity++;

        // Amount
        nAmount += out.txout.nValue;

        // Bytes
        CTxDestination address;
        int witnessversion = 0;
        std::vector<unsigned char> witnessprogram;
        if (out.txout.scriptPubKey.IsWitnessProgram(witnessversion, witnessprogram))
        {
            // add input skeleton bytes (outpoint, scriptSig size, nSequence)
            nBytesInputs += (32 + 4 + 1 + 4);

            if (witnessversion == 0) { // P2WPKH
                // 1 WU (witness item count) + 72 WU (ECDSA signature with len byte) + 34 WU (pubkey with len byte)
                nBytesInputs += 107 / WITNESS_SCALE_FACTOR;
            } else if (witnessversion == 1) { // P2TR key-path spend
                // 1 WU (witness item count) + 65 WU (Schnorr signature with len byte)
                nBytesInputs += 66 / WITNESS_SCALE_FACTOR;
            } else {
                // not supported, should be unreachable
                throw std::runtime_error("Trying to spend future segwit version script");
            }
            fWitness = true;
        }
        else if(ExtractDestination(out.txout.scriptPubKey, address))
        {
            CPubKey pubkey;
            PKHash* pkhash = std::get_if<PKHash>(&address);
            if (pkhash && wallet.getPubKey(out.txout.scriptPubKey, ToKeyID(*pkhash), pubkey))
            {
                nBytesInputs += (pubkey.IsCompressed() ? 148 : 180);
            }
            else
                nBytesInputs += 148; // in all error cases, simply assume 148 here
        }
        else nBytesInputs += 148;
    }

    // calculation
    if (nQuantity > 0)
    {
        // Bytes
        nBytes = nBytesInputs + ((inputs.amounts.size() > 0 ? inputs.amounts.size() + 1 : 2) * 34) + 10; // always assume +1 output for change here
        if (fWitness)
        {
            // there is some fudging in these numbers related to the actual virtual transaction size calculation that will keep this estimate from being exact.
            // Usually, the result will be an overestimate within a couple of connects so that the confirmation dialog displays a slightly smaller fee.
            // also, the witness stack size value is a variable sized integer. usually, the number of stack items will be well under the single byte var int limit.
            nBytes += 2; // account for the serialized marker and flag bytes
            nBytes += nQuantity; // account for the witness byte that holds the number of stack items for each input.
        }

        // in the subtract fee from amount case, we can tell if zero change already and subtract the bytes, so that fee calculation afterwards is accurate
        if (inputs.subtract_fee)
            if (nAmount - nPayAmount == 0)
                nBytes -= 34;

        // Fee
        nPayFee = wallet.getMinimumFee(nBytes, inputs.control, /*returned_target=*/nullptr, /*reason=*/nullptr);

        if (nPayAmount > 0)
        {
            nChange = nAmount - nPayAmount;
            if (!inputs.subtract_fee)
                nChange -= nPayFee;

            if (nChange > 0) {
                // Assumes a p2pkh script size
                CTxOut txout(nChange, CScript() << std::vector<unsigned char>(24, 0));
                // Never create dust outputs; if we would, just add the dust to the fee.
                if (IsDust(txout, node.getDustRelayFee()))
                {
                    nPayFee += nChange;
                    nChange = 0;
                    if (inputs.subtract_fee)
                        nBytes -= 34; // we didn't detect lack of change above
                }
            }

            if (nChange == 0 && !inputs.subtract_fee)
                nBytes -= 34;
        }

        // after fee
        nAfterFee = std::max<CAmount>(nAmount - nPayFee, 0);
    }

    return {nPayAmount, nAmount, nPayFee, nAfterFee, nChange, nBytes, nQuantity, std::move(spent)};
}
} // namespace

void CoinControlDialog::updateLabels(CCoinControl& coin_control, WalletModel* model, QDialog* dialog)
{
    if (!model) return;
    auto* state = static_cast<CoinControlLabelState*>(dialog->findChild<QObject*>("coinControlLabelState", Qt::FindDirectChildrenOnly));
    if (!state) state = new CoinControlLabelState(dialog);
    state->request(model, CoinControlInputs{coin_control, payAmounts, fSubtractFeeFromAmount});
    if (!state->result) {
        for (const char* name : {"labelCoinControlQuantity", "labelCoinControlAmount", "labelCoinControlFee",
                                "labelCoinControlAfterFee", "labelCoinControlBytes", "labelCoinControlChange"}) {
            dialog->findChild<QLabel*>(name)->setText(tr("Calculating…"));
        }
        if (auto* label = dialog->findChild<QLabel*>("labelCoinControlInsuffFunds")) label->hide();
        return;
    }
    const auto& stats{*state->result};
    for (const auto& outpoint : stats.spent) coin_control.UnSelect(outpoint);
    const auto nPayAmount{stats.pay_amount}, nAmount{stats.amount}, nPayFee{stats.fee};
    const auto nAfterFee{stats.after_fee}, nChange{stats.change};
    const auto nBytes{stats.bytes}, nQuantity{stats.quantity};

    // actually update labels
    BitcoinUnit nDisplayUnit = BitcoinUnit::BTC;
    if (model && model->getOptionsModel())
        nDisplayUnit = model->getOptionsModel()->getDisplayUnit();

    QLabel *l1 = dialog->findChild<QLabel *>("labelCoinControlQuantity");
    QLabel *l2 = dialog->findChild<QLabel *>("labelCoinControlAmount");
    QLabel *l3 = dialog->findChild<QLabel *>("labelCoinControlFee");
    QLabel *l4 = dialog->findChild<QLabel *>("labelCoinControlAfterFee");
    QLabel *l5 = dialog->findChild<QLabel *>("labelCoinControlBytes");
    QLabel *l8 = dialog->findChild<QLabel *>("labelCoinControlChange");

    // enable/disable "change"
    dialog->findChild<QLabel *>("labelCoinControlChangeText")   ->setEnabled(nPayAmount > 0);
    dialog->findChild<QLabel *>("labelCoinControlChange")       ->setEnabled(nPayAmount > 0);

    // stats
    l1->setText(QString::number(nQuantity));                                 // Quantity
    l2->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, nAmount));        // Amount
    l3->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, nPayFee));        // Fee
    l4->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, nAfterFee));      // After Fee
    l5->setText(((nBytes > 0) ? ASYMP_UTF8 : "") + QString::number(nBytes));        // Bytes
    l8->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, nChange));        // Change
    if (nPayFee > 0)
    {
        l3->setText(ASYMP_UTF8 + l3->text());
        l4->setText(ASYMP_UTF8 + l4->text());
        if (nChange > 0 && !CoinControlDialog::fSubtractFeeFromAmount)
            l8->setText(ASYMP_UTF8 + l8->text());
    }

    // How many connects the estimated fee can vary per byte we guess wrong.
    double dFeeVary = (nBytes != 0) ? (double)nPayFee / nBytes : 0;

    QString toolTip4 = tr("Can vary +/- %1 connect(s) per input.").arg(dFeeVary);

    l3->setToolTip(toolTip4);
    l4->setToolTip(toolTip4);
    l8->setToolTip(toolTip4);
    dialog->findChild<QLabel *>("labelCoinControlFeeText")      ->setToolTip(l3->toolTip());
    dialog->findChild<QLabel *>("labelCoinControlAfterFeeText") ->setToolTip(l4->toolTip());
    dialog->findChild<QLabel *>("labelCoinControlBytesText")    ->setToolTip(l5->toolTip());
    dialog->findChild<QLabel *>("labelCoinControlChangeText")   ->setToolTip(l8->toolTip());

    // Insufficient funds
    QLabel *label = dialog->findChild<QLabel *>("labelCoinControlInsuffFunds");
    if (label)
        label->setVisible(nChange < 0);
}

void CoinControlDialog::coinControlUpdateLabels()
{
    m_labels_queued = false;
    updateLabels(m_coin_control, model, this);
}

void CoinControlDialog::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
        renderView();
    }

    QDialog::changeEvent(e);
}

void CoinControlDialog::updateView()
{
    if (!model) return;
    ++m_view->revision;
    m_view->dirty = true;
    renderView();
    if (!m_view->timer.isActive()) m_view->timer.start();
    if (!m_view->query.valid()) pollView();
}

void CoinControlDialog::pollView()
{
    if (!model) {
        m_view->query = {};
        m_view->timer.stop();
        return;
    }
    if (m_view->query.valid()) {
        if (m_view->query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
        try {
            auto snapshot{m_view->query.get()};
            if (m_view->inflight_revision == m_view->revision) {
                m_view->cached = std::move(snapshot);
                renderView();
                coinControlUpdateLabels();
            }
        } catch (const std::exception& error) {
            qWarning() << "Coin control view refresh failed:" << error.what();
            m_view->dirty = true;
        }
    }
    if (m_view->dirty) {
        try {
            m_view->query = model->requestWalletData([](interfaces::Wallet& wallet) {
                CoinControlViewState::Snapshot snapshot;
                for (const auto& [destination, coins] : wallet.listCoins()) {
                    CoinControlViewState::Group group{QString::fromStdString(EncodeDestination(destination)), {}};
                    group.coins.reserve(coins.size());
                    for (const auto& [outpoint, coin] : coins) {
                        CTxDestination output_address;
                        QString address;
                        if (ExtractDestination(coin.txout.scriptPubKey, output_address)) {
                            address = QString::fromStdString(EncodeDestination(output_address));
                        }
                        group.coins.push_back({outpoint, coin.txout.nValue, coin.time, coin.depth_in_main_chain,
                                               std::move(address), QString::fromStdString(outpoint.hash.GetHex())});
                    }
                    snapshot.groups.push_back(std::move(group));
                }
                std::vector<COutPoint> locked;
                wallet.listLockedCoins(locked);
                snapshot.locked.insert(locked.begin(), locked.end());
                return snapshot;
            });
            m_view->inflight_revision = m_view->revision;
            m_view->dirty = false;
        } catch (const std::exception& error) {
            qWarning() << "Unable to request coin control view:" << error.what();
        }
    } else {
        m_view->timer.stop();
    }
}

void CoinControlDialog::renderView()
{
    if (!model || !model->getOptionsModel() || !model->getAddressTableModel())
        return;
    // Context-menu actions retain an item until the menu closes. A nested
    // responsive backend wait must not replace those items underneath them.
    if (!m_view->cached) return;
    if (m_view->mutation_active || contextMenu->isVisible()) {
        m_view->render_pending = true;
        return;
    }
    m_view->render_pending = false;

    bool treeMode = ui->radioTreeMode->isChecked();

    contextMenuItem = nullptr;
    // Programmatic item initialization must not run selection handlers for
    // every checkbox, and painting waits until the complete snapshot is ready.
    const QSignalBlocker signals{ui->treeWidget};
    const bool updates_enabled = ui->treeWidget->updatesEnabled();
    ui->treeWidget->setUpdatesEnabled(false);
    ui->treeWidget->clear();
    ui->treeWidget->setEnabled(false); // performance, otherwise updateLabels would be called for every checked checkbox
    ui->treeWidget->setAlternatingRowColors(!treeMode);
    QFlags<Qt::ItemFlag> flgCheckbox = Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
    QFlags<Qt::ItemFlag> flgTristate = Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate;

    BitcoinUnit nDisplayUnit = model->getOptionsModel()->getDisplayUnit();
    // Recolor the shared resource once, not once for every locked output.
    const QIcon locked_icon = m_view->cached->locked.empty() ? QIcon{} : platformStyle->SingleColorIcon(":/icons/lock_closed");

    for (const auto& group : m_view->cached->groups) {
        CCoinControlWidgetItem* itemWalletAddress{nullptr};
        const QString& sWalletAddress = group.address;
        QString sWalletLabel = model->getAddressTableModel()->labelForAddress(sWalletAddress);
        if (sWalletLabel.isEmpty())
            sWalletLabel = tr("(no label)");

        if (treeMode)
        {
            // wallet address
            itemWalletAddress = new CCoinControlWidgetItem(ui->treeWidget);

            itemWalletAddress->setFlags(flgTristate);
            itemWalletAddress->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);

            // label
            itemWalletAddress->setText(COLUMN_LABEL, sWalletLabel);

            // address
            itemWalletAddress->setText(COLUMN_ADDRESS, sWalletAddress);
        }

        CAmount nSum = 0;
        int nChildren = 0;
        for (const auto& out : group.coins) {
            const COutPoint& output = out.outpoint;
            nSum += out.amount;
            nChildren++;

            CCoinControlWidgetItem *itemOutput;
            if (treeMode)    itemOutput = new CCoinControlWidgetItem(itemWalletAddress);
            else             itemOutput = new CCoinControlWidgetItem(ui->treeWidget);
            itemOutput->setFlags(flgCheckbox);
            itemOutput->setCheckState(COLUMN_CHECKBOX,Qt::Unchecked);

            // address
            const QString& sAddress = out.address;
            // In tree mode, do not repeat the parent address for direct outputs.
            if (!treeMode || sAddress != sWalletAddress)
                itemOutput->setText(COLUMN_ADDRESS, sAddress);

            // label
            if (!(sAddress == sWalletAddress)) // change
            {
                // tooltip from where the change comes from
                itemOutput->setToolTip(COLUMN_LABEL, tr("change from %1 (%2)").arg(sWalletLabel).arg(sWalletAddress));
                itemOutput->setText(COLUMN_LABEL, tr("(change)"));
            }
            else if (!treeMode)
            {
                itemOutput->setText(COLUMN_LABEL, sWalletLabel);
            }

            // amount
            itemOutput->setText(COLUMN_AMOUNT, BitcoinUnits::format(nDisplayUnit, out.amount));
            itemOutput->setData(COLUMN_AMOUNT, Qt::UserRole, QVariant((qlonglong)out.amount)); // padding so that sorting works correctly

            // date
            itemOutput->setText(COLUMN_DATE, GUIUtil::dateTimeStr(out.time));
            itemOutput->setData(COLUMN_DATE, Qt::UserRole, QVariant((qlonglong)out.time));

            // confirmations
            itemOutput->setText(COLUMN_CONFIRMATIONS, QString::number(out.depth));
            itemOutput->setData(COLUMN_CONFIRMATIONS, Qt::UserRole, QVariant((qlonglong)out.depth));

            // transaction hash
            itemOutput->setData(COLUMN_ADDRESS, TxHashRole, out.txid);

            // vout index
            itemOutput->setData(COLUMN_ADDRESS, VOutRole, output.n);

             // disable locked coins
            if (m_view->cached->locked.contains(output))
            {
                m_coin_control.UnSelect(output); // just to be sure
                itemOutput->setDisabled(true);
                itemOutput->setIcon(COLUMN_CHECKBOX, locked_icon);
            }

            // set checkbox
            if (m_coin_control.IsSelected(output))
                itemOutput->setCheckState(COLUMN_CHECKBOX, Qt::Checked);
        }

        // amount
        if (treeMode)
        {
            itemWalletAddress->setText(COLUMN_CHECKBOX, "(" + QString::number(nChildren) + ")");
            itemWalletAddress->setText(COLUMN_AMOUNT, BitcoinUnits::format(nDisplayUnit, nSum));
            itemWalletAddress->setData(COLUMN_AMOUNT, Qt::UserRole, QVariant((qlonglong)nSum));
        }
    }

    // expand all partially selected
    if (treeMode)
    {
        for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++)
            if (ui->treeWidget->topLevelItem(i)->checkState(COLUMN_CHECKBOX) == Qt::PartiallyChecked)
                ui->treeWidget->topLevelItem(i)->setExpanded(true);
    }

    // sort view
    sortView(sortColumn, sortOrder);
    ui->treeWidget->setEnabled(true);
    ui->treeWidget->setUpdatesEnabled(updates_enabled);
    updateLabelLocked();
}
