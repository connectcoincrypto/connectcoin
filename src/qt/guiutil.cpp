// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/guiutil.h>

#include <qt/bitcoinaddressvalidator.h>
#include <qt/bitcoinunits.h>
#include <qt/guiconstants.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/sendcoinsrecipient.h>

#include <addresstype.h>
#include <base58.h>
#include <chainparams.h>
#include <common/args.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <logging.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <script/script.h>
#include <util/chaintype.h>
#include <util/exception.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/time.h>

#ifdef WIN32
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#endif

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLatin1String>
#include <QLineEdit>
#include <QList>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPluginLoader>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QScreen>
#include <QShortcut>
#include <QSize>
#include <QStandardPaths>
#include <QString>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWindow>
#include <QtGlobal>

#include <cassert>
#include <chrono>
#include <exception>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(Q_OS_MACOS)

#include <QProcess>

void ForceActivation();
#endif

using namespace std::chrono_literals;

namespace GUIUtil {

QString dateTimeStr(const QDateTime &date)
{
    return QLocale::system().toString(date.date(), QLocale::ShortFormat) + QString(" ") + date.toString("hh:mm");
}

QString dateTimeStr(qint64 nTime)
{
    return dateTimeStr(QDateTime::fromSecsSinceEpoch(nTime));
}

QFont fixedPitchFont(bool use_embedded_font)
{
    if (use_embedded_font) {
        return {"Roboto Mono"};
    }
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

// Return a pre-generated dummy bech32m address (P2TR) with invalid checksum.
static std::string DummyAddress(const CChainParams &params)
{
    std::string addr;
    switch (params.GetChainType()) {
    case ChainType::MAIN:
        addr = "cc1p35yvjel7srp783ztf8v6jdra7dhfzk5jaun8xz2qp6ws7z80n4tq2jku9f";
        break;
    case ChainType::SIGNET:
    case ChainType::TESTNET:
    case ChainType::TESTNET4:
        addr = "tcc1p35yvjel7srp783ztf8v6jdra7dhfzk5jaun8xz2qp6ws7z80n4tqa6qnlg";
        break;
    case ChainType::REGTEST:
        addr = "ccrt1p35yvjel7srp783ztf8v6jdra7dhfzk5jaun8xz2qp6ws7z80n4tqsr2427";
        break;
    } // no default case, so the compiler can warn about missing cases
    assert(!addr.empty());

    if (Assume(!IsValidDestinationString(addr))) return addr;
    return {};
}

void setupAddressWidget(QValidatedLineEdit *widget, QWidget *parent)
{
    parent->setFocusProxy(widget);

    widget->setFont(fixedPitchFont());
    // We don't want translators to use own addresses in translations
    // and this is the only place, where this address is supplied.
    widget->setPlaceholderText(QObject::tr("Enter a ConnectCoin address (e.g. %1)").arg(
        QString::fromStdString(DummyAddress(Params()))));
    widget->setValidator(new BitcoinAddressEntryValidator(parent));
    widget->setCheckValidator(new BitcoinAddressCheckValidator(parent));
}

void AddButtonShortcut(QAbstractButton* button, const QKeySequence& shortcut)
{
    QObject::connect(new QShortcut(shortcut, button), &QShortcut::activated, [button]() { button->animateClick(); });
}

bool parseBitcoinURI(const QUrl &uri, SendCoinsRecipient *out)
{
    // Return if URI is invalid or does not use the connectcoin: scheme.
    if(!uri.isValid() || uri.scheme() != QString("connectcoin"))
        return false;

    SendCoinsRecipient rv;
    rv.address = uri.path();
    // Trim any following forward slash which may have been added by the OS
    if (rv.address.endsWith("/")) {
        rv.address.truncate(rv.address.length() - 1);
    }
    rv.amount = 0;

    QUrlQuery uriQuery(uri);
    QList<QPair<QString, QString> > items = uriQuery.queryItems();
    for (QList<QPair<QString, QString> >::iterator i = items.begin(); i != items.end(); i++)
    {
        bool fShouldReturnFalse = false;
        if (i->first.startsWith("req-"))
        {
            i->first.remove(0, 4);
            fShouldReturnFalse = true;
        }

        if (i->first == "label")
        {
            rv.label = i->second;
            fShouldReturnFalse = false;
        }
        if (i->first == "message")
        {
            rv.message = i->second;
            fShouldReturnFalse = false;
        }
        else if (i->first == "amount")
        {
            if(!i->second.isEmpty())
            {
                if (!BitcoinUnits::parse(BitcoinUnit::BTC, i->second, &rv.amount) ||
                    rv.amount < 0 || rv.amount > BitcoinUnits::maxMoney()) {
                    return false;
                }
            }
            fShouldReturnFalse = false;
        }

        if (fShouldReturnFalse)
            return false;
    }
    if(out)
    {
        *out = rv;
    }
    return true;
}

bool parseBitcoinURI(QString uri, SendCoinsRecipient *out)
{
    QUrl uriInstance(uri);
    return parseBitcoinURI(uriInstance, out);
}

QString formatBitcoinURI(const SendCoinsRecipient &info)
{
    bool bech_32 = info.address.startsWith(QString::fromStdString(Params().Bech32HRP() + "1"));

    QString ret = QString("connectcoin:%1").arg(bech_32 ? info.address.toUpper() : info.address);
    int paramCount = 0;

    if (info.amount)
    {
        ret += QString("?amount=%1").arg(BitcoinUnits::format(BitcoinUnit::BTC, info.amount, false, BitcoinUnits::SeparatorStyle::NEVER));
        paramCount++;
    }

    if (!info.label.isEmpty())
    {
        QString lbl(QUrl::toPercentEncoding(info.label));
        ret += QString("%1label=%2").arg(paramCount == 0 ? "?" : "&").arg(lbl);
        paramCount++;
    }

    if (!info.message.isEmpty())
    {
        QString msg(QUrl::toPercentEncoding(info.message));
        ret += QString("%1message=%2").arg(paramCount == 0 ? "?" : "&").arg(msg);
        paramCount++;
    }

    return ret;
}

bool isDust(interfaces::Node& node, const QString& address, const CAmount& amount)
{
    CTxDestination dest = DecodeDestination(address.toStdString());
    CScript script = GetScriptForDestination(dest);
    CTxOut txOut(amount, script);
    return IsDust(txOut, node.getDustRelayFee());
}

QString HtmlEscape(const QString& str, bool fMultiLine)
{
    QString escaped = str.toHtmlEscaped();
    if(fMultiLine)
    {
        escaped = escaped.replace("\n", "<br>\n");
    }
    return escaped;
}

QString HtmlEscape(const std::string& str, bool fMultiLine)
{
    return HtmlEscape(QString::fromStdString(str), fMultiLine);
}

QModelIndex firstSelectedRow(const QAbstractItemView* view, int column)
{
    if (!view || !view->selectionModel() || !view->model()) return {};
    // These actions use one row only. selectedRows() would materialize every
    // selected index first, even for a history with hundreds of thousands of
    // selected rows. Keep its full-row semantics and range order, but stop as
    // soon as the first fully selected row is found.
    const auto* selection = view->selectionModel();
    for (const auto& range : selection->selection()) {
        const auto parent = range.parent();
        for (int row = range.top(); row <= range.bottom(); ++row) {
            if (selection->isRowSelected(row, parent)) return view->model()->index(row, column, parent);
        }
    }
    return {};
}

void copyEntryData(const QAbstractItemView *view, int column, int role)
{
    const auto selected = firstSelectedRow(view, column);
    if (selected.isValid()) setClipboard(selected.data(role).toString());
}

QList<QModelIndex> getEntryData(const QAbstractItemView *view, int column)
{
    if(!view || !view->selectionModel())
        return QList<QModelIndex>();
    return view->selectionModel()->selectedRows(column);
}

bool hasEntryData(const QAbstractItemView *view, int column, int role)
{
    const auto selected = firstSelectedRow(view, column);
    return selected.isValid() && !selected.data(role).toString().isEmpty();
}

void LoadFont(const QString& file_name)
{
    // The qminimal plugin does not provide font loading support.
    if (QApplication::platformName() == "minimal") return;
    const int id = QFontDatabase::addApplicationFont(file_name);
    assert(id != -1);
}

QString getDefaultDataDirectory()
{
    return PathToQString(GetDefaultDataDir());
}

QString ExtractFirstSuffixFromFilter(const QString& filter)
{
    QRegularExpression filter_re(QStringLiteral(".* \\(\\*\\.(.*)[ \\)]"), QRegularExpression::InvertedGreedinessOption);
    QString suffix;
    QRegularExpressionMatch m = filter_re.match(filter);
    if (m.hasMatch()) {
        suffix = m.captured(1);
    }
    return suffix;
}

QString getSaveFileName(QWidget *parent, const QString &caption, const QString &dir,
    const QString &filter,
    QString *selectedSuffixOut)
{
    QString selectedFilter;
    QString myDir;
    if(dir.isEmpty()) // Default to user documents location
    {
        myDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    else
    {
        myDir = dir;
    }
    /* Directly convert path to native OS path separators */
    QString result = QDir::toNativeSeparators(QFileDialog::getSaveFileName(parent, caption, myDir, filter, &selectedFilter));

    QString selectedSuffix = ExtractFirstSuffixFromFilter(selectedFilter);

    /* Add suffix if needed */
    QFileInfo info(result);
    if(!result.isEmpty())
    {
        if(info.suffix().isEmpty() && !selectedSuffix.isEmpty())
        {
            /* No suffix specified, add selected suffix */
            if(!result.endsWith("."))
                result.append(".");
            result.append(selectedSuffix);
        }
    }

    /* Return selected suffix if asked to */
    if(selectedSuffixOut)
    {
        *selectedSuffixOut = selectedSuffix;
    }
    return result;
}

QString getOpenFileName(QWidget *parent, const QString &caption, const QString &dir,
    const QString &filter,
    QString *selectedSuffixOut)
{
    QString selectedFilter;
    QString myDir;
    if(dir.isEmpty()) // Default to user documents location
    {
        myDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    else
    {
        myDir = dir;
    }
    /* Directly convert path to native OS path separators */
    QString result = QDir::toNativeSeparators(QFileDialog::getOpenFileName(parent, caption, myDir, filter, &selectedFilter));

    if(selectedSuffixOut)
    {
        *selectedSuffixOut = ExtractFirstSuffixFromFilter(selectedFilter);
        ;
    }
    return result;
}

Qt::ConnectionType blockingGUIThreadConnection()
{
    if(QThread::currentThread() != qApp->thread())
    {
        return Qt::BlockingQueuedConnection;
    }
    else
    {
        return Qt::DirectConnection;
    }
}

bool checkPoint(const QPoint &p, const QWidget *w)
{
    QWidget *atW = QApplication::widgetAt(w->mapToGlobal(p));
    if (!atW) return false;
    return atW->window() == w;
}

bool isObscured(QWidget *w)
{
    return !(checkPoint(QPoint(0, 0), w)
        && checkPoint(QPoint(w->width() - 1, 0), w)
        && checkPoint(QPoint(0, w->height() - 1), w)
        && checkPoint(QPoint(w->width() - 1, w->height() - 1), w)
        && checkPoint(QPoint(w->width() / 2, w->height() / 2), w));
}

void bringToFront(QWidget* w)
{
#ifdef Q_OS_MACOS
    ForceActivation();
#endif

    if (w) {
        // activateWindow() (sometimes) helps with keyboard focus on Windows
        if (w->isMinimized()) {
            w->showNormal();
        } else {
            w->show();
        }
        w->activateWindow();
        w->raise();
    }
}

void handleCloseWindowShortcut(QWidget* w)
{
    QObject::connect(new QShortcut(QKeySequence(QObject::tr("Ctrl+W")), w), &QShortcut::activated, w, &QWidget::close);
}

void openDebugLogfile()
{
    BackendOperationGuard operation;
    fs::path pathDebug = LogInstance().m_file_path;

    /* Open debug.log with the associated application */
    if (WaitForBackendTask(std::async(std::launch::async, [pathDebug] { return fs::exists(pathDebug); })))
        QDesktopServices::openUrl(QUrl::fromLocalFile(PathToQString(pathDebug)));
}

bool openBitcoinConf()
{
    BackendOperationGuard operation;

    /* Create the file */
    const auto [created, pathConfig] = WaitForBackendTask(std::async(std::launch::async, [] {
        const fs::path pathConfig = gArgs.GetConfigFilePath();
        std::ofstream configFile{pathConfig.std_path(), std::ios_base::app};
        configFile.close();
        return std::make_pair(!configFile.fail(), pathConfig);
    }));
    if (!created) return false;

    /* Open connectcoin.conf with the associated application */
    bool res = QDesktopServices::openUrl(QUrl::fromLocalFile(PathToQString(pathConfig)));
#ifdef Q_OS_MACOS
    // Workaround for macOS-specific behavior; see #15409.
    if (!res) {
        res = QProcess::startDetached("/usr/bin/open", QStringList{"-t", PathToQString(pathConfig)});
    }
#endif

    return res;
}

ToolTipToRichTextFilter::ToolTipToRichTextFilter(int _size_threshold, QObject *parent) :
    QObject(parent),
    size_threshold(_size_threshold)
{

}

bool ToolTipToRichTextFilter::eventFilter(QObject *obj, QEvent *evt)
{
    if(evt->type() == QEvent::ToolTipChange)
    {
        QWidget *widget = static_cast<QWidget*>(obj);
        QString tooltip = widget->toolTip();
        if(tooltip.size() > size_threshold && !tooltip.startsWith("<qt") && !Qt::mightBeRichText(tooltip))
        {
            // Envelop with <qt></qt> to make sure Qt detects this as rich text
            // Escape the current message as HTML and replace \n by <br>
            tooltip = "<qt>" + HtmlEscape(tooltip, true) + "</qt>";
            widget->setToolTip(tooltip);
            return true;
        }
    }
    return QObject::eventFilter(obj, evt);
}

LabelOutOfFocusEventFilter::LabelOutOfFocusEventFilter(QObject* parent)
    : QObject(parent)
{
}

bool LabelOutOfFocusEventFilter::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FocusOut) {
        auto focus_out = static_cast<QFocusEvent*>(event);
        if (focus_out->reason() != Qt::PopupFocusReason) {
            auto label = qobject_cast<QLabel*>(watched);
            if (label) {
                auto flags = label->textInteractionFlags();
                label->setTextInteractionFlags(Qt::NoTextInteraction);
                label->setTextInteractionFlags(flags);
            }
        }
    }

    return QObject::eventFilter(watched, event);
}

#ifdef WIN32
fs::path static StartupShortcutPath()
{
    ChainType chain = gArgs.GetChainType();
    if (chain == ChainType::MAIN)
        return GetSpecialFolderPath(CSIDL_STARTUP) / "ConnectCoin.lnk";
    if (chain == ChainType::TESTNET) // Remove this special case when testnet CBaseChainParams::DataDir() is incremented to "testnet4"
        return GetSpecialFolderPath(CSIDL_STARTUP) / "ConnectCoin (testnet).lnk";
    return GetSpecialFolderPath(CSIDL_STARTUP) / fs::u8path(strprintf("ConnectCoin (%s).lnk", ChainTypeToString(chain)));
}

bool GetStartOnSystemStartup()
{
    // Check for the ConnectCoin startup shortcut.
    return fs::exists(StartupShortcutPath());
}

bool SetStartOnSystemStartup(bool fAutoStart)
{
    // If the shortcut exists already, remove it for updating
    fs::remove(StartupShortcutPath());

    if (fAutoStart)
    {
        CoInitialize(nullptr);

        // Get a pointer to the IShellLink interface.
        IShellLinkW* psl = nullptr;
        HRESULT hres = CoCreateInstance(CLSID_ShellLink, nullptr,
            CLSCTX_INPROC_SERVER, IID_IShellLinkW,
            reinterpret_cast<void**>(&psl));

        if (SUCCEEDED(hres))
        {
            // Get the current executable path
            WCHAR pszExePath[MAX_PATH];
            GetModuleFileNameW(nullptr, pszExePath, ARRAYSIZE(pszExePath));

            // Start client minimized
            QString strArgs = "-min";
            // Set -testnet /-regtest options
            strArgs += QString::fromStdString(strprintf(" -chain=%s", gArgs.GetChainTypeString()));

            // Set the path to the shortcut target
            psl->SetPath(pszExePath);
            PathRemoveFileSpecW(pszExePath);
            psl->SetWorkingDirectory(pszExePath);
            psl->SetShowCmd(SW_SHOWMINNOACTIVE);
            psl->SetArguments(strArgs.toStdWString().c_str());

            // Query IShellLink for the IPersistFile interface for
            // saving the shortcut in persistent storage.
            IPersistFile* ppf = nullptr;
            hres = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
            if (SUCCEEDED(hres))
            {
                // Save the link by calling IPersistFile::Save.
                hres = ppf->Save(StartupShortcutPath().wstring().c_str(), TRUE);
                ppf->Release();
                psl->Release();
                CoUninitialize();
                return true;
            }
            psl->Release();
        }
        CoUninitialize();
        return false;
    }
    return true;
}
#elif defined(Q_OS_LINUX)

// Follow the Desktop Application Autostart Spec:
// https://specifications.freedesktop.org/autostart-spec/autostart-spec-latest.html

fs::path static GetAutostartDir()
{
    char* pszConfigHome = getenv("XDG_CONFIG_HOME");
    if (pszConfigHome) return fs::path(pszConfigHome) / "autostart";
    char* pszHome = getenv("HOME");
    if (pszHome) return fs::path(pszHome) / ".config" / "autostart";
    return fs::path();
}

fs::path static GetAutostartFilePath()
{
    const fs::path autostart_dir{GetAutostartDir()};
    if (autostart_dir.empty()) return {};

    ChainType chain = gArgs.GetChainType();
    if (chain == ChainType::MAIN)
        return autostart_dir / "connectcoin.desktop";
    return autostart_dir / fs::u8path(strprintf("connectcoin-%s.desktop", ChainTypeToString(chain)));
}

static std::string EscapeDesktopExecPath(std::string_view path)
{
    // Desktop Entry values and quoted Exec arguments each process backslash
    // escapes. Keep both layers explicit and prevent '%' from being treated as
    // a field code. See the Desktop Entry Specification, section "The Exec key".
    std::string escaped;
    escaped.reserve(path.size());
    for (const char ch : path) {
        switch (ch) {
        case '\\': escaped += "\\\\\\\\"; break;
        case '"':
        case '`':
        case '$': escaped += "\\\\"; escaped += ch; break;
        case '%': escaped += "%%"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += ch;
        }
    }
    return escaped;
}

bool GetStartOnSystemStartup()
{
    const fs::path autostart_file{GetAutostartFilePath()};
    if (autostart_file.empty()) return false;

    std::ifstream optionFile{autostart_file.std_path()};
    if (!optionFile.good())
        return false;
    // Scan through file for "Hidden=true":
    std::string line;
    while (!optionFile.eof())
    {
        getline(optionFile, line);
        if (line.find("Hidden") != std::string::npos &&
            line.find("true") != std::string::npos)
            return false;
    }
    optionFile.close();

    return true;
}

bool SetStartOnSystemStartup(bool fAutoStart)
{
    const fs::path autostart_file{GetAutostartFilePath()};
    if (autostart_file.empty()) return false;

    if (!fAutoStart)
        fs::remove(autostart_file);
    else
    {
        char pszExePath[MAX_PATH+1];
        ssize_t r = readlink("/proc/self/exe", pszExePath, sizeof(pszExePath));
        if (r == -1 || r > MAX_PATH) {
            return false;
        }
        pszExePath[r] = '\0';
        const std::string exe_path{pszExePath};
        // The Desktop Entry specification cannot represent '=' in an
        // executable name or path.
        if (exe_path.find('=') != std::string::npos) return false;

        fs::create_directories(autostart_file.parent_path());

        std::ofstream optionFile{autostart_file.std_path(), std::ios_base::out | std::ios_base::trunc};
        if (!optionFile.good())
            return false;
        ChainType chain = gArgs.GetChainType();
        // Write a connectcoin.desktop file to the autostart directory:
        optionFile << "[Desktop Entry]\n";
        optionFile << "Type=Application\n";
        if (chain == ChainType::MAIN)
            optionFile << "Name=ConnectCoin\n";
        else
            optionFile << strprintf("Name=ConnectCoin (%s)\n", ChainTypeToString(chain));
        optionFile << "Icon=" QAPP_DESKTOP_ICON_NAME "\n";
        optionFile << "StartupWMClass=" QAPP_DESKTOP_FILE_NAME "\n";
        optionFile << "Exec=\"" << EscapeDesktopExecPath(exe_path)
                   << strprintf("\" -min -chain=%s\n", ChainTypeToString(chain));
        optionFile << "Terminal=false\n";
        optionFile << "Hidden=false\n";
        optionFile.close();
    }
    return true;
}

#else

bool GetStartOnSystemStartup() { return false; }
bool SetStartOnSystemStartup(bool fAutoStart) { return false; }

#endif

void setClipboard(const QString& str)
{
    QClipboard* clipboard = QApplication::clipboard();
    clipboard->setText(str, QClipboard::Clipboard);
    if (clipboard->supportsSelection()) {
        clipboard->setText(str, QClipboard::Selection);
    }
}

fs::path QStringToPath(const QString &path)
{
    return fs::u8path(path.toStdString());
}

QString PathToQString(const fs::path &path)
{
    return QString::fromStdString(path.utf8string());
}

QString NetworkToQString(Network net)
{
    switch (net) {
    case NET_UNROUTABLE: return QObject::tr("Unroutable");
    //: Name of IPv4 network in peer info
    case NET_IPV4: return QObject::tr("IPv4", "network name");
    //: Name of IPv6 network in peer info
    case NET_IPV6: return QObject::tr("IPv6", "network name");
    //: Name of Tor network in peer info
    case NET_ONION: return QObject::tr("Onion", "network name");
    //: Name of I2P network in peer info
    case NET_I2P: return QObject::tr("I2P", "network name");
    //: Name of CJDNS network in peer info
    case NET_CJDNS: return QObject::tr("CJDNS", "network name");
    case NET_INTERNAL: return "Internal";  // should never actually happen
    case NET_MAX: assert(false);
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString ConnectionTypeToQString(ConnectionType conn_type, bool prepend_direction)
{
    QString prefix;
    if (prepend_direction) {
        prefix = (conn_type == ConnectionType::INBOUND) ?
                     /*: An inbound connection from a peer. An inbound connection
                         is a connection initiated by a peer. */
                     QObject::tr("Inbound") :
                     /*: An outbound connection to a peer. An outbound connection
                         is a connection initiated by us. */
                     QObject::tr("Outbound") + " ";
    }
    switch (conn_type) {
    case ConnectionType::INBOUND: return prefix;
    //: Peer connection type that relays all network information.
    case ConnectionType::OUTBOUND_FULL_RELAY: return prefix + QObject::tr("Full Relay");
    /*: Peer connection type that relays network information about
        blocks and not transactions or addresses. */
    case ConnectionType::BLOCK_RELAY: return prefix + QObject::tr("Block Relay");
    //: Peer connection type established manually through one of several methods.
    case ConnectionType::MANUAL: return prefix + QObject::tr("Manual");
    //: Short-lived peer connection type that tests the aliveness of known addresses.
    case ConnectionType::FEELER: return prefix + QObject::tr("Feeler");
    //: Short-lived peer connection type that solicits known addresses from a peer.
    case ConnectionType::ADDR_FETCH: return prefix + QObject::tr("Address Fetch");
    //: Short-lived peer connection type that is used for broadcasting privacy-sensitive data.
    case ConnectionType::PRIVATE_BROADCAST: return prefix + QObject::tr("Private Broadcast");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString formatDurationStr(std::chrono::nanoseconds dur)
{
    const auto d{std::chrono::duration_cast<std::chrono::days>(dur)};
    const auto h{std::chrono::duration_cast<std::chrono::hours>(dur - d)};
    const auto m{std::chrono::duration_cast<std::chrono::minutes>(dur - d - h)};
    const auto s{std::chrono::duration_cast<std::chrono::seconds>(dur - d - h - m)};
    QStringList str_list;
    if (auto d2{d.count()}) str_list.append(QObject::tr("%1 d").arg(d2));
    if (auto h2{h.count()}) str_list.append(QObject::tr("%1 h").arg(h2));
    if (auto m2{m.count()}) str_list.append(QObject::tr("%1 m").arg(m2));
    const auto s2{s.count()};
    if (s2 || str_list.empty()) str_list.append(QObject::tr("%1 s").arg(s2));
    return str_list.join(" ");
}

QString FormatPeerAge(NodeClock::time_point connected)
{
    const auto age{NodeClock::now() - connected};
    if (age >= 24h) return QObject::tr("%1 d").arg(age / 24h);
    if (age >= 1h) return QObject::tr("%1 h").arg(age / 1h);
    if (age >= 1min) return QObject::tr("%1 m").arg(age / 1min);
    return QObject::tr("%1 s").arg(age / 1s);
}

QString formatServicesStr(quint64 mask)
{
    QStringList strList;

    for (const auto& flag : serviceFlagsToStr(mask)) {
        strList.append(QString::fromStdString(flag));
    }

    if (strList.size())
        return strList.join(", ");
    else
        return QObject::tr("None");
}

QString formatPingTime(NodeClock::duration ping_time)
{
    return (ping_time == decltype(CNode::m_min_ping_time.load())::max() || ping_time == 0us) ?
        QObject::tr("N/A") :
        QObject::tr("%1 ms").arg(QString::number(Ticks<std::chrono::milliseconds>(ping_time)));
}

QString formatTimeOffset(int64_t time_offset)
{
  return QObject::tr("%1 s").arg(QString::number((int)time_offset, 10));
}

QString formatNiceTimeOffset(qint64 secs)
{
    // Represent time from last generated block in human readable text
    QString timeBehindText;
    const int HOUR_IN_SECONDS = 60*60;
    const int DAY_IN_SECONDS = 24*60*60;
    const int WEEK_IN_SECONDS = 7*24*60*60;
    const int YEAR_IN_SECONDS = 31556952; // Average length of year in Gregorian calendar
    if(secs < 60)
    {
        timeBehindText = QObject::tr("%n second(s)","",secs);
    }
    else if(secs < 2*HOUR_IN_SECONDS)
    {
        timeBehindText = QObject::tr("%n minute(s)","",secs/60);
    }
    else if(secs < 2*DAY_IN_SECONDS)
    {
        timeBehindText = QObject::tr("%n hour(s)","",secs/HOUR_IN_SECONDS);
    }
    else if(secs < 2*WEEK_IN_SECONDS)
    {
        timeBehindText = QObject::tr("%n day(s)","",secs/DAY_IN_SECONDS);
    }
    else if(secs < YEAR_IN_SECONDS)
    {
        timeBehindText = QObject::tr("%n week(s)","",secs/WEEK_IN_SECONDS);
    }
    else
    {
        qint64 years = secs / YEAR_IN_SECONDS;
        qint64 remainder = secs % YEAR_IN_SECONDS;
        timeBehindText = QObject::tr("%1 and %2").arg(QObject::tr("%n year(s)", "", years)).arg(QObject::tr("%n week(s)","", remainder/WEEK_IN_SECONDS));
    }
    return timeBehindText;
}

QString formatBytes(uint64_t bytes)
{
    if (bytes < 1'000)
        return QObject::tr("%1 B").arg(bytes);
    if (bytes < 1'000'000)
        return QObject::tr("%1 kB").arg(bytes / 1'000);
    if (bytes < 1'000'000'000)
        return QObject::tr("%1 MB").arg(bytes / 1'000'000);

    return QObject::tr("%1 GB").arg(bytes / 1'000'000'000);
}

qreal calculateIdealFontSize(int width, const QString& text, QFont font, qreal minPointSize, qreal font_size) {
    while(font_size >= minPointSize) {
        font.setPointSizeF(font_size);
        QFontMetrics fm(font);
        if (TextWidth(fm, text) < width) {
            break;
        }
        font_size -= 0.5;
    }
    return font_size;
}

ThemedLabel::ThemedLabel(const PlatformStyle* platform_style, QWidget* parent)
    : QLabel{parent}, m_platform_style{platform_style}
{
    assert(m_platform_style);
}

void ThemedLabel::setThemedPixmap(const QString& image_filename, int width, int height)
{
    m_image_filename = image_filename;
    m_pixmap_width = width;
    m_pixmap_height = height;
    updateThemedPixmap();
}

void ThemedLabel::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
        updateThemedPixmap();
    }

    QLabel::changeEvent(e);
}

void ThemedLabel::updateThemedPixmap()
{
    setPixmap(m_platform_style->SingleColorIcon(m_image_filename).pixmap(m_pixmap_width, m_pixmap_height));
}

ClickableLabel::ClickableLabel(const PlatformStyle* platform_style, QWidget* parent)
    : ThemedLabel{platform_style, parent}
{
}

void ClickableLabel::mouseReleaseEvent(QMouseEvent *event)
{
    Q_EMIT clicked(event->pos());
}

void ClickableProgressBar::mouseReleaseEvent(QMouseEvent *event)
{
    Q_EMIT clicked(event->pos());
}

bool ItemDelegate::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            Q_EMIT keyEscapePressed();
        }
    }
    return QItemDelegate::eventFilter(object, event);
}

void PolishProgressDialog(QProgressDialog* dialog)
{
#ifdef Q_OS_MACOS
    // Workaround for macOS-only Qt bug; see: QTBUG-65750, QTBUG-70357.
    const int margin = TextWidth(dialog->fontMetrics(), ("X"));
    dialog->resize(dialog->width() + 2 * margin, dialog->height());
#endif
    // QProgressDialog estimates the time the operation will take (based on time
    // for steps), and only shows itself if that estimate is beyond minimumDuration.
    // The default minimumDuration value is 4 seconds, and it could make users
    // think that the GUI is frozen.
    dialog->setMinimumDuration(0);
}

int TextWidth(const QFontMetrics& fm, const QString& text)
{
    return fm.horizontalAdvance(text);
}

void LogQtInfo()
{
    // Snapshot Qt-owned presentation objects on their owning thread. System
    // discovery and core logging may perform IO or wait for the log mutex;
    // neither belongs on the GUI thread, even during startup.
    std::vector<std::string> lines;
#ifdef QT_STATIC
    const std::string qt_link{"static"};
#else
    const std::string qt_link{"dynamic"};
#endif
    lines.push_back(strprintf("Qt %s (%s), plugin=%s\n", qVersion(), qt_link, QGuiApplication::platformName().toStdString()));
    const auto static_plugins = QPluginLoader::staticPlugins();
    if (static_plugins.empty()) {
        lines.emplace_back("No static plugins.\n");
    } else {
        lines.emplace_back("Static plugins:\n");
        for (const QStaticPlugin& p : static_plugins) {
            QJsonObject meta_data = p.metaData();
            const std::string plugin_class = meta_data.take(QString("className")).toString().toStdString();
            const int plugin_version = meta_data.take(QString("version")).toInt();
            lines.push_back(strprintf(" %s, version %d\n", plugin_class, plugin_version));
        }
    }

    lines.push_back(strprintf("Style: %s / %s\n", QApplication::style()->objectName().toStdString(), QApplication::style()->metaObject()->className()));
    std::vector<std::string> screens;
    for (const QScreen* s : QGuiApplication::screens()) {
        screens.push_back(strprintf("Screen: %s %dx%d, pixel ratio=%.1f\n", s->name().toStdString(), s->size().width(), s->size().height(), s->devicePixelRatio()));
    }
    WaitForBackendTask(std::async(std::launch::async, [lines = std::move(lines), screens = std::move(screens)] {
        for (const auto& line : lines) LogInfo("%s", line);
        LogInfo("System: %s, %s\n", QSysInfo::prettyProductName().toStdString(), QSysInfo::buildAbi().toStdString());
        for (const auto& line : screens) LogInfo("%s", line);
    }));
}

void PopupMenu(QMenu* menu, const QPoint& point, QAction* at_action)
{
    // The qminimal plugin does not provide window system integration.
    if (QApplication::platformName() == "minimal") return;
    menu->popup(point, at_action);
}

QDateTime StartOfDay(const QDate& date)
{
    return date.startOfDay();
}

bool HasPixmap(const QLabel* label)
{
    return !label->pixmap(Qt::ReturnByValue).isNull();
}

QString MakeHtmlLink(const QString& source, const QString& link)
{
    return QString(source).replace(
        link,
        QLatin1String("<a href=\"") + link + QLatin1String("\">") + link + QLatin1String("</a>"));
}

void PrintSlotException(
    const std::exception* exception,
    const QObject* sender,
    const QObject* receiver)
{
    std::string description = sender->metaObject()->className();
    description += "->";
    if (receiver) {
        description += receiver->metaObject()->className();
    } else {
        description += "anonymous function";
    }
    PrintExceptionContinue(exception, description);
}

void ShowModalDialogAsynchronously(QDialog* dialog)
{
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->show();
}

namespace {
// Accessed exclusively from the GUI thread. Pending unwinds prevent a queued
// unload/shutdown from deleting an action's caller as its wait dialog closes.
size_t g_backend_operations{0};
size_t g_backend_operation_unwinds{0};

class BackendTaskDialog final : public QDialog
{
public:
    explicit BackendTaskDialog(QWidget* parent)
        : QDialog(nullptr, Qt::Dialog | Qt::WindowTitleHint | Qt::CustomizeWindowHint)
    {
        // This dialog lives on the waiting stack, not in its caller's QObject
        // tree. A queued deleteLater() of that caller must not delete the
        // stack object or end the wait before the backend result is ready.
        // A native transient relationship still keeps it with the window.
        if (parent && parent->window()->windowHandle()) {
            winId();
            windowHandle()->setTransientParent(parent->window()->windowHandle());
        }
        setWindowTitle(QObject::tr("Working…"));
        setWindowModality(Qt::ApplicationModal);
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel(QObject::tr("Please wait while the operation completes."), this));
        auto* progress = new QProgressBar(this);
        progress->setRange(0, 0);
        layout->addWidget(progress);
    }

    void finish() { QDialog::done(QDialog::Accepted); }

    // Closing the UI cannot cancel the submitted backend task. In particular,
    // never let Escape unwind stack data before its future is ready.
    void done(int) override {}
    void accept() override {}
    void reject() override {}

protected:
    void closeEvent(QCloseEvent* event) override { event->ignore(); }
};
} // namespace

BackendOperationGuard::BackendOperationGuard()
{
    assert(qApp && QThread::currentThread() == qApp->thread());
    ++g_backend_operations;
}

BackendOperationGuard::~BackendOperationGuard()
{
    assert(qApp && QThread::currentThread() == qApp->thread());
    assert(g_backend_operations > 0);
    --g_backend_operations;
    ++g_backend_operation_unwinds;
    QTimer::singleShot(0, qApp, [] {
        assert(g_backend_operation_unwinds > 0);
        --g_backend_operation_unwinds;
    });
}

bool HasActiveBackendOperation()
{
    assert(qApp && QThread::currentThread() == qApp->thread());
    return g_backend_operations != 0 || g_backend_operation_unwinds != 0;
}

void WaitForBackendTaskReady(const std::function<bool()>& ready, QWidget* parent)
{
    assert(qApp && QThread::currentThread() == qApp->thread());
    if (ready()) return;

    BackendTaskDialog dialog(parent);
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &dialog, [&] {
        if (ready()) dialog.finish();
    });
    poll.start();
    // The dialog deliberately ignores reject/close. Only a ready future can
    // finish it, and unload/shutdown must honor the operation guard above.
    dialog.exec();
    assert(ready());
}

QString WalletDisplayName(const QString& name)
{
    return name.isEmpty() ? "[" + QObject::tr("default wallet") + "]" : name;
}

QString WalletDisplayName(const std::string& name)
{
    return WalletDisplayName(QString::fromStdString(name));
}
} // namespace GUIUtil
