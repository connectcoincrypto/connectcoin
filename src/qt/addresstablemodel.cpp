// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/addresstablemodel.h>

#include <qt/guiutil.h>
#include <qt/walletmodel.h>

#include <key_io.h>
#include <outputtype.h>
#include <wallet/types.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>

#include <QFont>
#include <QDebug>
#include <QPointer>
#include <QTimer>

const QString AddressTableModel::Send = "S";
const QString AddressTableModel::Receive = "R";

struct AddressTableEntry
{
    enum Type {
        Sending,
        Receiving,
        Hidden /* QSortFilterProxyModel will filter these out */
    };

    Type type;
    QString label;
    QString address;

    AddressTableEntry() = default;
    AddressTableEntry(Type _type, const QString &_label, const QString &_address):
        type(_type), label(_label), address(_address) {}
};

struct AddressTableEntryLessThan
{
    bool operator()(const AddressTableEntry &a, const AddressTableEntry &b) const
    {
        return a.address < b.address;
    }
    bool operator()(const AddressTableEntry &a, const QString &b) const
    {
        return a.address < b;
    }
    bool operator()(const QString &a, const AddressTableEntry &b) const
    {
        return a < b.address;
    }
};

/* Determine address type from address purpose */
constexpr AddressTableEntry::Type translateTransactionType(wallet::AddressPurpose purpose, bool isMine)
{
    // "refund" addresses aren't shown, and change addresses aren't returned by getAddresses at all.
    switch (purpose) {
    case wallet::AddressPurpose::SEND: return AddressTableEntry::Sending;
    case wallet::AddressPurpose::RECEIVE: return AddressTableEntry::Receiving;
    case wallet::AddressPurpose::REFUND: return AddressTableEntry::Hidden;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

// Private implementation
class AddressTablePriv
{
public:
    QList<AddressTableEntry> cachedAddressTable;
    AddressTableModel *parent;
    struct AddressData {
        QString label;
        wallet::AddressPurpose purpose;
    };
    // Keep lookup data independently of the visible rows. For example, the
    // signing address selector filters out non-PKHash destinations, but their
    // labels and purposes are still needed by transaction and send views.
    std::map<CTxDestination, AddressData> cachedAddressData;
    std::map<QString, AddressData> cachedAddressTextData;
    std::map<CTxDestination, uint64_t> notification_revisions;
    bool m_pk_hash_only{false};
    OutputType m_default_address_type{OutputType::BECH32M};
    struct Snapshot {
        QList<AddressTableEntry> rows;
        std::map<CTxDestination, AddressData> addresses;
        std::map<QString, AddressData> address_text;
        OutputType address_type;
    };
    struct Update {
        QString label;
        bool is_mine;
        wallet::AddressPurpose purpose;
        int status;
    };
    std::future<Snapshot> m_snapshot;
    std::map<QString, Update> m_pending_updates;
    bool m_ready{false};

    explicit AddressTablePriv(AddressTableModel *_parent):
        parent(_parent) {}

    static Snapshot readAddressTable(interfaces::Wallet& wallet, bool pk_hash_only)
    {
        Snapshot snapshot{{}, {}, {}, wallet.getDefaultAddressType()};
        {
            for (const auto& address : wallet.getAddresses())
            {
                const auto text = QString::fromStdString(EncodeDestination(address.dest));
                const AddressData data{QString::fromStdString(address.name), address.purpose};
                snapshot.addresses.insert_or_assign(address.dest, data);
                snapshot.address_text.insert_or_assign(text, data);
                if (pk_hash_only && !std::holds_alternative<PKHash>(address.dest)) {
                    continue;
                }
                AddressTableEntry::Type addressType = translateTransactionType(
                        address.purpose, address.is_mine);
                snapshot.rows.append(AddressTableEntry(addressType,
                                  QString::fromStdString(address.name),
                                  text));
            }
        }
        // std::lower_bound() and std::upper_bound() require our cachedAddressTable list to be sorted in asc order
        // Even though the map is already sorted this re-sorting step is needed because the originating map
        // is sorted by binary address, not by base58() address.
        std::sort(snapshot.rows.begin(), snapshot.rows.end(), AddressTableEntryLessThan());
        return snapshot;
    }

    void cacheAddress(const CTxDestination& destination, const QString& label, wallet::AddressPurpose purpose)
    {
        const auto old = cachedAddressData.find(destination);
        const bool changed = old == cachedAddressData.end() || old->second.label != label || old->second.purpose != purpose;
        const auto text = QString::fromStdString(EncodeDestination(destination));
        // A newer live notification or completed GUI edit supersedes an
        // older update still awaiting replay after the initial snapshot.
        if (m_ready) m_pending_updates.erase(text);
        cachedAddressData.insert_or_assign(destination, AddressData{label, purpose});
        cachedAddressTextData.insert_or_assign(text, AddressData{label, purpose});
        if (changed) Q_EMIT parent->labelsChanged(text);
    }

    void eraseAddress(const CTxDestination& destination)
    {
        cachedAddressData.erase(destination);
        const auto text = QString::fromStdString(EncodeDestination(destination));
        cachedAddressTextData.erase(text);
        if (m_ready) m_pending_updates.erase(text);
    }

    std::optional<AddressData> refreshAddress(const CTxDestination& destination)
    {
        // A responsive mutation wait can deliver a later RPC edit before the
        // GUI action resumes. Read back the authoritative result instead of
        // reinstating the submitted label/removal. Notifications delivered
        // during this read take precedence over its potentially older result.
        const auto revision = notification_revisions[destination];
        const QPointer<AddressTableModel> guard{parent};
        const auto result = GUIUtil::WaitForBackendTask(parent->walletModel->requestWalletData(
            [destination](interfaces::Wallet& wallet) -> std::optional<AddressData> {
                std::string label;
                wallet::AddressPurpose purpose;
                if (!wallet.getAddress(destination, &label, &purpose)) return {};
                return AddressData{QString::fromStdString(label), purpose};
            }));
        if (guard && notification_revisions[destination] == revision) {
            if (result) cacheAddress(destination, result->label, result->purpose);
            else eraseAddress(destination);
        }
        return result;
    }

    void replayPendingUpdates()
    {
        const QPointer<AddressTableModel> guard{parent};
        for (int count{0}; count < 64 && !m_pending_updates.empty(); ++count) {
            auto pending = m_pending_updates.extract(m_pending_updates.begin());
            const auto& update = pending.mapped();
            parent->updateEntry(pending.key(), update.label, update.is_mine, update.purpose, update.status);
            if (!guard) return;
        }
        if (!m_pending_updates.empty()) {
            QMetaObject::invokeMethod(parent, [this] { replayPendingUpdates(); }, Qt::QueuedConnection);
        }
    }

    void updateEntry(const QString &updated_address, const QString &label, bool isMine, wallet::AddressPurpose purpose, int status)
    {
        const auto destination = DecodeDestination(updated_address.toStdString());
        if (!IsValidDestination(destination)) return;
        ++notification_revisions[destination];
        if (status == CT_NEW || status == CT_UPDATED) {
            cacheAddress(destination, label, purpose);
        } else if (status == CT_DELETED) {
            eraseAddress(destination);
        } else {
            return;
        }
        if (m_pk_hash_only && !std::holds_alternative<PKHash>(destination)) return;
        const auto address = QString::fromStdString(EncodeDestination(destination));
        // Find address / label in model
        QList<AddressTableEntry>::iterator lower = std::lower_bound(
            cachedAddressTable.begin(), cachedAddressTable.end(), address, AddressTableEntryLessThan());
        QList<AddressTableEntry>::iterator upper = std::upper_bound(
            cachedAddressTable.begin(), cachedAddressTable.end(), address, AddressTableEntryLessThan());
        int lowerIndex = (lower - cachedAddressTable.begin());
        int upperIndex = (upper - cachedAddressTable.begin());
        bool inModel = (lower != upper);
        AddressTableEntry::Type newEntryType = translateTransactionType(purpose, isMine);

        switch(status)
        {
        case CT_NEW:
        case CT_UPDATED:
            if(inModel)
            {
                // A notification may already be reflected in a fresh snapshot.
                lower->type = newEntryType;
                lower->label = label;
                parent->emitDataChanged(lowerIndex);
                break;
            }
            parent->beginInsertRows(QModelIndex(), lowerIndex, lowerIndex);
            cachedAddressTable.insert(lowerIndex, AddressTableEntry(newEntryType, label, address));
            parent->endInsertRows();
            break;
        case CT_DELETED:
            if(!inModel)
            {
                break;
            }
            parent->beginRemoveRows(QModelIndex(), lowerIndex, upperIndex-1);
            cachedAddressTable.erase(lower, upper);
            parent->endRemoveRows();
            break;
        }
    }

    int size() const
    {
        return cachedAddressTable.size();
    }

    const AddressTableEntry* index(int idx) const
    {
        if(idx >= 0 && idx < cachedAddressTable.size())
        {
            return &cachedAddressTable[idx];
        }
        else
        {
            return nullptr;
        }
    }
};

AddressTableModel::AddressTableModel(WalletModel *parent, bool pk_hash_only) :
    QAbstractTableModel(parent), walletModel(parent)
{
    columns << tr("Label") << tr("Address");
    priv = new AddressTablePriv(this);
    priv->m_pk_hash_only = pk_hash_only;
    // WalletModel::refresh can replace the primary address cache while an
    // older address-book dialog still uses its original model. Route label
    // changes from every cache through their common owner.
    connect(this, &AddressTableModel::labelsChanged, parent, &WalletModel::addressBookLabelsChanged);
    priv->m_snapshot = parent->requestWalletData([pk_hash_only](interfaces::Wallet& wallet) {
        return AddressTablePriv::readAddressTable(wallet, pk_hash_only);
    });
    auto* timer = new QTimer(this);
    timer->setInterval(50);
    connect(timer, &QTimer::timeout, this, [this, timer] {
        if (priv->m_snapshot.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
        timer->stop();
        try {
            auto snapshot = priv->m_snapshot.get();
            beginResetModel();
            priv->cachedAddressTable = std::move(snapshot.rows);
            priv->cachedAddressData = std::move(snapshot.addresses);
            priv->cachedAddressTextData = std::move(snapshot.address_text);
            priv->m_default_address_type = snapshot.address_type;
            priv->m_ready = true;
            endResetModel();
            const QPointer<AddressTableModel> guard{this};
            Q_EMIT labelsChanged({});
            if (!guard) return;
            priv->replayPendingUpdates();
        } catch (const std::exception& error) {
            qWarning() << "Address book snapshot failed:" << error.what();
        }
    });
    // WalletModel can be constructed outside the GUI before it is moved.
    QMetaObject::invokeMethod(timer, [timer] { timer->start(); }, Qt::QueuedConnection);
}

AddressTableModel::~AddressTableModel()
{
    delete priv;
}

bool AddressTableModel::isPkHashOnly() const
{
    return priv->m_pk_hash_only;
}

int AddressTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return priv->size();
}

int AddressTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return columns.length();
}

QVariant AddressTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.model() != this || index.column() < 0 || index.column() >= columns.size())
        return QVariant();

    const AddressTableEntry* rec = priv->index(index.row());
    if (!rec) return {};

    const auto column = static_cast<ColumnIndex>(index.column());
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (column) {
        case Label:
            if (rec->label.isEmpty() && role == Qt::DisplayRole) {
                return tr("(no label)");
            } else {
                return rec->label;
            }
        case Address:
            return rec->address;
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    } else if (role == Qt::FontRole) {
        switch (column) {
        case Label:
            return QFont();
        case Address:
            return GUIUtil::fixedPitchFont();
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    } else if (role == TypeRole) {
        switch(rec->type)
        {
        case AddressTableEntry::Sending:
            return Send;
        case AddressTableEntry::Receiving:
            return Receive;
        case AddressTableEntry::Hidden:
            return {};
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    }
    return QVariant();
}

bool AddressTableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    GUIUtil::BackendOperationGuard operation;
    if (!index.isValid() || index.model() != this || index.column() < 0 || index.column() >= columns.size())
        return false;
    const AddressTableEntry* entry = priv->index(index.row());
    if (!entry) return false;
    // A backend notification can update the table during a mutation. Keep the
    // edited values independently of the table's storage.
    const AddressTableEntry rec = *entry;
    auto purpose = rec.type == AddressTableEntry::Sending ? wallet::AddressPurpose::SEND :
                   rec.type == AddressTableEntry::Hidden ? wallet::AddressPurpose::REFUND : wallet::AddressPurpose::RECEIVE;
    QString current_label = rec.label;
    // QDataWidgetMapper may submit label and address edits consecutively,
    // before the queued label notification updates the visible row.
    getAddressData(rec.address, &current_label, &purpose);
    editStatus = OK;

    if(role == Qt::EditRole)
    {
        CTxDestination curAddress = DecodeDestination(rec.address.toStdString());
        if(index.column() == Label)
        {
            // Do nothing, if old label == new label
            if(current_label == value.toString())
            {
                editStatus = NO_CHANGES;
                return false;
            }
            if (!GUIUtil::WaitForBackendTask(walletModel->requestWalletData([curAddress, label = value.toString().toStdString(), purpose](interfaces::Wallet& wallet) {
                return wallet.setAddressBook(curAddress, label, purpose);
            }))) return false;
            priv->refreshAddress(curAddress);
        } else if(index.column() == Address) {
            CTxDestination newAddress = DecodeDestination(value.toString().toStdString());
            // Refuse to set invalid address, set error status and return false
            if(std::get_if<CNoDestination>(&newAddress))
            {
                editStatus = INVALID_ADDRESS;
                return false;
            }
            // Do nothing, if old address == new address
            else if(newAddress == curAddress)
            {
                editStatus = NO_CHANGES;
                return false;
            }
            // Check for duplicate addresses to prevent accidental deletion of addresses, if you try
            // to paste an existing address over another address (with a different label)
            const auto existing = priv->refreshAddress(newAddress);
            if (existing)
            {
                // The notification for a concurrently added address may not
                // have reached the GUI yet. Keep duplicate warnings accurate.
                editStatus = DUPLICATE_ADDRESS;
                return false;
            }
            // Double-check that we're not overwriting a receiving address
            else if(rec.type == AddressTableEntry::Sending)
            {
                // Remove old entry
                if (!GUIUtil::WaitForBackendTask(walletModel->requestWalletData([curAddress](interfaces::Wallet& wallet) { return wallet.delAddressBook(curAddress); }))) return false;
                priv->refreshAddress(curAddress);
                // Add new entry with new address
                if (!GUIUtil::WaitForBackendTask(walletModel->requestWalletData([newAddress, label = current_label.toStdString(), purpose](interfaces::Wallet& wallet) {
                    return wallet.setAddressBook(newAddress, label, purpose);
                }))) return false;
                priv->refreshAddress(newAddress);
            }
        }
        return true;
    }
    return false;
}

QVariant AddressTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation == Qt::Horizontal)
    {
        if(role == Qt::DisplayRole && section < columns.size())
        {
            return columns[section];
        }
    }
    return QVariant();
}

Qt::ItemFlags AddressTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid() || index.model() != this || index.column() < 0 || index.column() >= columns.size()) return Qt::NoItemFlags;

    const AddressTableEntry* rec = priv->index(index.row());
    if (!rec) return Qt::NoItemFlags;

    Qt::ItemFlags retval = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    // Can edit address and label for sending addresses,
    // and only label for receiving addresses.
    if(rec->type == AddressTableEntry::Sending ||
      (rec->type == AddressTableEntry::Receiving && index.column()==Label))
    {
        retval |= Qt::ItemIsEditable;
    }
    return retval;
}

QModelIndex AddressTableModel::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid() || row < 0 || row >= priv->size() || column < 0 || column >= columns.size()) return {};
    // QList insertions/removals can move its storage. Qt adjusts persistent
    // index rows itself; resolve that row when reading instead of retaining a
    // pointer into the old allocation.
    return createIndex(row, column);
}

void AddressTableModel::updateEntry(const QString &address,
        const QString &label, bool isMine, wallet::AddressPurpose purpose, int status)
{
    // Update address book model from ConnectCoin Core
    if (!priv->m_ready) priv->m_pending_updates.insert_or_assign(address, AddressTablePriv::Update{label, isMine, purpose, status});
    priv->updateEntry(address, label, isMine, purpose, status);
    // Deletions may already have updated the local cache in removeRows().
    // Always invalidate dependants when the backend confirms the removal.
    if (status == CT_DELETED) Q_EMIT labelsChanged(address);
}

QString AddressTableModel::addRow(const QString &type, const QString &label, const QString &address, const OutputType address_type)
{
    GUIUtil::BackendOperationGuard operation;
    std::string strLabel = label.toStdString();
    std::string strAddress = address.toStdString();

    editStatus = OK;

    if(type == Send)
    {
        if(!walletModel->validateAddress(address))
        {
            editStatus = INVALID_ADDRESS;
            return QString();
        }
        // Check for duplicate addresses
        {
            const auto existing = priv->refreshAddress(DecodeDestination(strAddress));
            if (existing)
            {
                editStatus = DUPLICATE_ADDRESS;
                return QString();
            }
        }

        // Add entry
        const auto destination = DecodeDestination(strAddress);
        if (!GUIUtil::WaitForBackendTask(walletModel->requestWalletData([destination, strLabel](interfaces::Wallet& wallet) {
            return wallet.setAddressBook(destination, strLabel, wallet::AddressPurpose::SEND);
        }))) return {};
        priv->refreshAddress(destination);
    }
    else if(type == Receive)
    {
        // Generate a new address to associate with given label
        const auto generate = [address_type, strLabel](interfaces::Wallet& wallet) -> std::optional<CTxDestination> {
            auto destination = wallet.getNewDestination(address_type, strLabel);
            if (!destination) return {};
            return std::move(*destination);
        };
        if (auto dest{GUIUtil::WaitForBackendTask(walletModel->requestWalletData(generate))}) {
            strAddress = EncodeDestination(*dest);
        } else {
            WalletModel::UnlockContext ctx(walletModel->requestUnlock());
            if (!ctx.isValid()) {
                // Unlock wallet failed or was cancelled
                editStatus = WALLET_UNLOCK_FAILURE;
                return QString();
            }
            if (auto dest_retry{GUIUtil::WaitForBackendTask(walletModel->requestWalletData(generate))}) {
                strAddress = EncodeDestination(*dest_retry);
            } else {
                editStatus = KEY_GENERATION_FAILURE;
                return QString();
            }
        }
        priv->refreshAddress(DecodeDestination(strAddress));
    }
    else
    {
        return QString();
    }
    return QString::fromStdString(strAddress);
}

bool AddressTableModel::removeRows(int row, int count, const QModelIndex &parent)
{
    GUIUtil::BackendOperationGuard operation;
    Q_UNUSED(parent);
    const AddressTableEntry* rec = priv->index(row);
    if(count != 1 || !rec || rec->type == AddressTableEntry::Receiving)
    {
        // Can only remove one row at a time, and cannot remove rows not in model.
        // Also refuse to remove receiving addresses.
        return false;
    }
    const auto destination = DecodeDestination(rec->address.toStdString());
    if (!GUIUtil::WaitForBackendTask(walletModel->requestWalletData([destination](interfaces::Wallet& wallet) { return wallet.delAddressBook(destination); }))) return false;
    priv->refreshAddress(destination);
    return true;
}

QString AddressTableModel::labelForAddress(const QString &address) const
{
    QString name;
    if (getAddressData(address, &name, /* purpose= */ nullptr)) {
        return name;
    }
    return QString();
}

std::optional<wallet::AddressPurpose> AddressTableModel::purposeForAddress(const QString &address) const
{
    wallet::AddressPurpose purpose;
    if (getAddressData(address, /* name= */ nullptr, &purpose)) {
        return purpose;
    }
    return std::nullopt;
}

bool AddressTableModel::getAddressData(const QString &address,
        QString* name,
        wallet::AddressPurpose* purpose) const {
    // Transaction rows already contain canonical addresses. Avoid checksum
    // and public-key validation on every paint, including missing labels.
    if (const auto text = priv->cachedAddressTextData.find(address); text != priv->cachedAddressTextData.end()) {
        if (name) *name = text->second.label;
        if (purpose) *purpose = text->second.purpose;
        return true;
    }
    // Uppercase Bech32 and whitespace-padded Base58 can alias canonical
    // addresses. Decode these uncommon cases rather than blindly trimming or
    // lowercasing, which would accept invalid Bech32 or alter Base58 spelling.
    const bool padded = !address.isEmpty() && (address.front().isSpace() || address.back().isSpace());
    if (!padded && address != address.toUpper()) return false;
    const auto destination = DecodeDestination(address.toStdString());
    if (!IsValidDestination(destination)) return false;
    const auto entry = priv->cachedAddressData.find(destination);
    if (entry == priv->cachedAddressData.end()) return false;
    if (name) *name = entry->second.label;
    if (purpose) *purpose = entry->second.purpose;
    return true;
}

int AddressTableModel::lookupAddress(const QString &address) const
{
    // Rows are already kept in address order for live updates. Avoid a
    // model-wide match()/QVariant scan when opening or editing an address.
    const auto& rows = priv->cachedAddressTable;
    const auto found = std::lower_bound(rows.cbegin(), rows.cend(), address, AddressTableEntryLessThan{});
    return found != rows.cend() && found->address == address ? static_cast<int>(found - rows.cbegin()) : -1;
}

OutputType AddressTableModel::GetDefaultAddressType() const { return priv->m_default_address_type; };

void AddressTableModel::emitDataChanged(int idx)
{
    Q_EMIT dataChanged(index(idx, 0, QModelIndex()), index(idx, columns.length()-1, QModelIndex()));
}

QString AddressTableModel::GetWalletDisplayName() const { return walletModel->getDisplayName(); };
