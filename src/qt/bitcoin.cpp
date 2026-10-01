// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <qt/bitcoin.h>

#include <chainparams.h>
#include <common/args.h>
#include <common/init.h>
#include <common/system.h>
#include <init.h>
#include <interfaces/handler.h>
#include <interfaces/init.h>
#include <interfaces/node.h>
#include <node/context.h>
#include <node/interface_ui.h>
#include <noui.h>
#include <qt/bitcoingui.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guipreferences.h>
#include <qt/guitranslations.h>
#include <qt/guiutil.h>
#include <qt/initexecutor.h>
#include <qt/intro.h>
#include <qt/networkstyle.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/qtlogforwarder.h>
#include <qt/splashscreen.h>
#include <qt/utilitydialog.h>
#include <qt/winshutdownmonitor.h>
#ifdef Q_OS_MACOS
#include <qt/macdockiconhandler.h>
#endif
#include <uint256.h>
#include <util/btcsignals.h>
#include <util/exception.h>
#include <util/log.h>
#include <util/string.h>
#include <util/threadnames.h>
#include <util/translation.h>
#include <validation.h>

#ifdef ENABLE_WALLET
#include <qt/paymentserver.h>
#include <qt/walletcontroller.h>
#include <qt/walletmodel.h>
#include <wallet/p2c_tls.h>
#include <wallet/types.h>
#endif // ENABLE_WALLET

#include <chrono>
#include <memory>

#include <QApplication>
#include <QDebug>
#include <QIcon>
#include <QLatin1String>
#include <QLocale>
#include <QMessageBox>
#include <QThread>
#include <QTimer>
#include <QWindow>

#ifdef WIN32
#include <shobjidl.h>
#endif

// Declare meta types used for QMetaObject::invokeMethod
Q_DECLARE_METATYPE(bool*)
Q_DECLARE_METATYPE(CAmount)
Q_DECLARE_METATYPE(SynchronizationState)
Q_DECLARE_METATYPE(SyncType)
Q_DECLARE_METATYPE(uint256)
#ifdef ENABLE_WALLET
Q_DECLARE_METATYPE(wallet::AddressPurpose)
#endif // ENABLE_WALLET

using util::MakeUnorderedList;

static void RegisterMetaTypes()
{
    // Register meta types used for QMetaObject::invokeMethod and Qt::QueuedConnection
    qRegisterMetaType<bool*>();
    qRegisterMetaType<SynchronizationState>();
    qRegisterMetaType<SyncType>();
  #ifdef ENABLE_WALLET
    qRegisterMetaType<WalletModel*>();
    qRegisterMetaType<wallet::AddressPurpose>();
  #endif // ENABLE_WALLET
    // Register typedefs (see https://doc.qt.io/qt-5/qmetatype.html#qRegisterMetaType)
    // IMPORTANT: if CAmount is no longer a typedef use the normal variant above (see https://doc.qt.io/qt-5/qmetatype.html#qRegisterMetaType-1)
    qRegisterMetaType<CAmount>("CAmount");
    qRegisterMetaType<size_t>("size_t");

    qRegisterMetaType<std::function<void()>>("std::function<void()>");
    qRegisterMetaType<QMessageBox::Icon>("QMessageBox::Icon");
    qRegisterMetaType<interfaces::BlockAndHeaderTipInfo>("interfaces::BlockAndHeaderTipInfo");
    qRegisterMetaType<BitcoinUnit>("BitcoinUnit");
}

static QString GetLangTerritory()
{
    // System locale < saved preference < command-line override.
    QString locale = QLocale::system().name();
    const QString saved = GuiSettings{}.value("language", "").toString();
    if (!saved.isEmpty()) locale = saved;
    return GUIUtil::WaitForBackendTask(std::async(std::launch::async, [locale] {
        return QString::fromStdString(gArgs.GetArg("-lang", locale.toStdString()));
    }));
}

static bool ErrorSettingsRead(const bilingual_str& error, const std::vector<std::string>& details)
{
    QMessageBox messagebox(QMessageBox::Critical, CLIENT_NAME, QString::fromStdString(strprintf("%s.", error.translated)), QMessageBox::Reset | QMessageBox::Abort);
    /*: Explanatory text shown on startup when the settings file cannot be read.
      Prompts user to make a choice between resetting or aborting. */
    messagebox.setInformativeText(QObject::tr("Do you want to reset settings to default values, or to abort without making changes?"));
    messagebox.setDetailedText(QString::fromStdString(MakeUnorderedList(details)));
    messagebox.setTextFormat(Qt::PlainText);
    messagebox.setDefaultButton(QMessageBox::Reset);
    switch (messagebox.exec()) {
    case QMessageBox::Reset:
        return false;
    case QMessageBox::Abort:
        return true;
    default:
        assert(false);
        return true;
    }
}

static void ErrorSettingsWrite(const bilingual_str& error, const std::vector<std::string>& details)
{
    QMessageBox messagebox(QMessageBox::Critical, CLIENT_NAME, QString::fromStdString(strprintf("%s.", error.translated)), QMessageBox::Ok);
    /*: Explanatory text shown on startup when the settings file could not be written.
        Prompts user to check that we have the ability to write to the file.
        Explains that the user has the option of running without a settings file.*/
    messagebox.setInformativeText(QObject::tr("A fatal error occurred. Check that settings file is writable, or try running with -nosettings."));
    messagebox.setDetailedText(QString::fromStdString(MakeUnorderedList(details)));
    messagebox.setTextFormat(Qt::PlainText);
    messagebox.setDefaultButton(QMessageBox::Ok);
    messagebox.exec();
}

static char qt_program_name[] = "connectcoin-qt";
#ifdef Q_OS_LINUX
// Qt's XCB backend uses -name for the WM_CLASS instance. Keep it aligned with
// StartupWMClass even when the per-network application/QSettings name changes.
// Only Qt sees these arguments; Core's command line remains unchanged.
static char qt_name_option[] = "-name";
static char qt_desktop_name[] = QAPP_DESKTOP_FILE_NAME;
static char* qt_argv[] = {qt_program_name, qt_name_option, qt_desktop_name, nullptr};
static int qt_argc = 3;
#else
static char* qt_argv[] = {qt_program_name, nullptr};
static int qt_argc = 1;
#endif

BitcoinApplication::BitcoinApplication()
    : QApplication(qt_argc, qt_argv)
{
#ifdef WIN32
    // Associate every window, including early startup dialogs, with the same
    // identity as the installed shortcuts and taskbar pin.
    if (FAILED(SetCurrentProcessExplicitAppUserModelID(WINDOWS_APP_USER_MODEL_ID))) {
        qWarning("Could not set the Windows taskbar application identity.");
    }
#endif
#ifdef Q_OS_LINUX
    // Wayland/app launchers use the desktop-file ID.
    setDesktopFileName(QStringLiteral(QAPP_DESKTOP_FILE_NAME));
#endif
    // Startup/help/error windows appear before the network-specific main
    // window. Give them the embedded icon on every desktop platform too.
    setWindowIcon(QIcon(QStringLiteral(":/icons/bitcoin")));
    // Qt runs setlocale(LC_ALL, "") on initialization.
    RegisterMetaTypes();
    setQuitOnLastWindowClosed(false);
    m_gui_preferences = std::make_unique<GuiPreferences>();
}

void BitcoinApplication::setupPlatformStyle()
{
    // UI per-platform customization
    // This must be done inside the BitcoinApplication constructor, or after it, because
    // PlatformStyle::instantiate requires a QApplication
    std::string platformName;
    platformName = gArgs.GetArg("-uiplatform", BitcoinGUI::DEFAULT_UIPLATFORM);
    platformStyle = PlatformStyle::instantiate(QString::fromStdString(platformName));
    if (!platformStyle) // Fall back to "other" if specified name not found
        platformStyle = PlatformStyle::instantiate("other");
    assert(platformStyle);
}

BitcoinApplication::~BitcoinApplication()
{
    // Fallback for owners that exit without running the normal shutdown flow.
    // Never leave the interrupt worker accessing a destroyed node.
    if (m_shutdown_interrupt.valid()) m_shutdown_interrupt.wait();
#ifdef ENABLE_WALLET
    wallet::ShutdownP2CRsaProbes();
#endif
    m_executor.reset();

    delete m_splash;
    m_splash = nullptr;
    delete window;
    window = nullptr;
#ifdef Q_OS_MACOS
    // The Dock handler is shared by all windows and must outlive them.
    MacDockIconHandler::cleanup();
#endif
    m_translations.reset();
    m_gui_preferences.reset();
    delete platformStyle;
    platformStyle = nullptr;
    if (m_qt_log_forwarder) {
        // Synchronous fallback only; normal shutdown already drained this on
        // a worker before core logging/process state can disappear.
        m_qt_log_forwarder->stop();
        qInstallMessageHandler(m_previous_message_handler);
        m_qt_log_forwarder.reset();
    }
}

void BitcoinApplication::startQtLogging()
{
    assert(!m_qt_log_forwarder);
    m_qt_log_forwarder = std::make_unique<QtLogForwarder>();
    m_previous_message_handler = InstallQtLogForwarder(*m_qt_log_forwarder);
}

void BitcoinApplication::loadGuiPreferences()
{
    GUIUtil::WaitForBackendTask(GuiSettings{}.load());
}

void BitcoinApplication::loadTranslations()
{
    if (!m_translations) m_translations = std::make_unique<GuiTranslations>();
    m_translations->loadLocale(GetLangTerritory());
}

#ifdef ENABLE_WALLET
void BitcoinApplication::createPaymentServer()
{
    paymentServer = new PaymentServer(this);
    paymentServer->startLocalServer();
}
#endif

bool BitcoinApplication::createOptionsModel(bool resetSettings)
{
    optionsModel = new OptionsModel(node(), this);
    if (resetSettings) {
        optionsModel->Reset();
    }
    bilingual_str error;
    if (!optionsModel->Init(error)) {
        fs::path settings_path;
        if (gArgs.GetSettingsPath(&settings_path)) {
            error += Untranslated("\n");
            std::string quoted_path = strprintf("%s", fs::quoted(fs::PathToString(settings_path)));
            error.original += strprintf("Settings file %s might be corrupt or invalid.", quoted_path);
            error.translated += tr("Settings file %1 might be corrupt or invalid.").arg(QString::fromStdString(quoted_path)).toStdString();
        }
        InitError(error);
        QMessageBox::critical(nullptr, CLIENT_NAME, QString::fromStdString(error.translated));
        return false;
    }
    return true;
}

void BitcoinApplication::createWindow(const NetworkStyle *networkStyle)
{
    window = new BitcoinGUI(node(), platformStyle, networkStyle, nullptr);
    connect(window, &BitcoinGUI::quitRequested, this, &BitcoinApplication::requestShutdown);

    pollShutdownTimer = new QTimer(window);
    connect(pollShutdownTimer, &QTimer::timeout, [this]{
        if (!QApplication::activeModalWidget()) {
            window->detectShutdown();
        }
    });
}

void BitcoinApplication::createSplashScreen(const NetworkStyle *networkStyle)
{
    assert(!m_splash);
    m_splash = new SplashScreen(networkStyle);
    connect(m_splash, &SplashScreen::shutdownRequested, this, &BitcoinApplication::requestShutdown);
    m_splash->show();
}

void BitcoinApplication::createNode(interfaces::Init& init)
{
    assert(!m_node);
    m_node = init.makeNode();
    if (m_splash) m_splash->setNode(*m_node);
}

bool BitcoinApplication::baseInitialize()
{
    GUIUtil::BackendOperationGuard operation;
    // Directory probes/locks, network setup and cryptographic sanity checks can
    // block. Core error/question callbacks still run on the responsive GUI.
    struct Result {
        bool initialized{false};
        bool start_minimized{false};
        bool wallet_enabled{false};
    };
    const auto result = GUIUtil::WaitForBackendTask(std::async(std::launch::async, [node_ptr = &node()] {
        Result result;
        result.initialized = node_ptr->baseInitialize();
        result.start_minimized = gArgs.GetBoolArg("-min", false);
#ifdef ENABLE_WALLET
        WalletModel::refreshWalletEnabled();
        result.wallet_enabled = WalletModel::isWalletEnabled();
#endif
        return result;
    }));
    m_start_minimized = result.start_minimized;
    m_wallet_enabled = result.wallet_enabled;
    return result.initialized;
}

void BitcoinApplication::startThread()
{
    assert(!m_executor);
    m_executor.emplace(node());

    /*  communication to and from thread */
    connect(&m_executor.value(), &InitExecutor::initializeResult, this, &BitcoinApplication::initializeResult);
    connect(&m_executor.value(), &InitExecutor::shutdownResult, this, [this] {
        // Keep translations available to Core throughout shutdown, then
        // destroy all catalog/dependency objects on their own worker.
        if (m_translations) m_translations->stop();
        QCoreApplication::exit(0);
    });
    connect(&m_executor.value(), &InitExecutor::runawayException, this, &BitcoinApplication::handleRunawayException);
    connect(this, &BitcoinApplication::requestedInitialize, &m_executor.value(), &InitExecutor::initialize);
    connect(this, &BitcoinApplication::requestedShutdown, &m_executor.value(), &InitExecutor::shutdown);
}

void BitcoinApplication::parameterSetup()
{
    // Logging configuration resolves paths and parameter interactions share
    // cs_args with settings writes. Neither needs access to Qt widgets.
    GUIUtil::WaitForBackendTask(std::async(std::launch::async, [] {
        // GUI programs should not print to the console unnecessarily.
        gArgs.SoftSetBoolArg("-printtoconsole", false);
        InitLogging(gArgs);
        InitParameterInteraction(gArgs);
    }));
}

void BitcoinApplication::InitPruneSetting(int64_t prune_MiB)
{
    optionsModel->SetPruneTargetGB(PruneMiBtoGB(prune_MiB));
}

void BitcoinApplication::requestInitialize()
{
    qDebug() << __func__ << ": Requesting initialize";
    startThread();
    Q_EMIT requestedInitialize();
}

void BitcoinApplication::requestShutdown()
{
    if (m_shutdown_started) return;
    m_shutdown_requested = true;
    // startShutdown also joins mining threads and user -shutdownnotify commands.
    // Submit it once, independently of the model pools that are being drained.
    // Before the init executor exists, base initialization can still be changing
    // the node's interfaces; remember the request without racing those changes.
    if (m_executor && !m_shutdown_interrupt.valid()) {
        m_shutdown_interrupt = std::async(std::launch::async,
            [node_ptr = &node()] {
                node_ptr->startShutdown();
#ifdef ENABLE_WALLET
                // Drain probe phases touching process state before models/node
                // are torn down. This can wait and must not run on the GUI.
                wallet::ShutdownP2CRsaProbes();
#endif
            });
    }
    if (!m_initialization_finished || !m_shutdown_interrupt.valid() ||
        m_shutdown_interrupt.wait_for(std::chrono::seconds{0}) != std::future_status::ready ||
        GUIUtil::HasActiveBackendOperation()
#ifdef ENABLE_WALLET
        || (m_wallet_controller && m_wallet_controller->hasActiveActivities())
#endif
    ) {
        // Keep callbacks and their GUI callers alive until initialization,
        // interruption and explicit actions (including unlock contexts) unwind.
        if (!m_shutdown_retry_pending) {
            m_shutdown_retry_pending = true;
            QTimer::singleShot(25, this, [this] {
                m_shutdown_retry_pending = false;
                requestShutdown();
            });
        }
        return;
    }
    m_shutdown_started = true;
    m_shutdown_interrupt.get();

    for (const auto w : QGuiApplication::topLevelWindows()) {
        w->hide();
    }

    if (m_splash) m_splash->stop();
    delete m_splash;
    m_splash = nullptr;

    // Show a simple window indicating shutdown status
    // Do this first as some of the steps may take some time below,
    // for example the RPC console may still be executing a command.
    shutdownWindow.reset(ShutdownWindow::showShutdownWindow(window));

    qDebug() << __func__ << ": Requesting shutdown";

    // Must disconnect node signals otherwise current thread can deadlock since
    // no event loop is running.
    window->unsubscribeFromCoreSignals();
    // Prior to unsetting the client model, stop listening backend signals
    if (clientModel) {
        clientModel->stopWorkers();
    }

    // Unsetting the client model can cause the current thread to wait for node
    // to complete an operation, like wait for a RPC execution to complete.
    window->setClientModel(nullptr);
    pollShutdownTimer->stop();

#ifdef ENABLE_WALLET
    // Delete wallet controller here manually, instead of relying on Qt object
    // tracking (https://doc.qt.io/qt-5/objecttrees.html). This makes sure
    // walletmodel m_handle_* notification handlers are deleted before wallets
    // are unloaded, which can simplify wallet implementations. It also avoids
    // these notifications having to be handled while GUI objects are being
    // destroyed, making GUI code less fragile as well.
    if (m_wallet_controller) m_wallet_controller->stop();
    delete m_wallet_controller;
    m_wallet_controller = nullptr;
#endif // ENABLE_WALLET

    delete clientModel;
    clientModel = nullptr;

    // All wallet views have detached and saved their state. Capture the two
    // surviving windows before flushing; their destructors will be no-ops for
    // preferences and cannot introduce a late disk wait.
    window->saveSettings();
    GUIUtil::WaitForBackendTask(GuiPreferences::Flush());

    if (m_qt_log_forwarder) {
        GUIUtil::WaitForBackendTask(std::async(std::launch::async,
            [forwarder = m_qt_log_forwarder.get()] { forwarder->stop(); }));
    }

    // Request shutdown from core thread
    Q_EMIT requestedShutdown();
}

void BitcoinApplication::initializeResult(bool success, interfaces::BlockAndHeaderTipInfo tip_info)
{
    GUIUtil::BackendOperationGuard operation;
    qDebug() << __func__ << ": Initialization result: " << success;
    m_initialization_finished = true;

    if (!success || m_shutdown_requested || m_node->shutdownRequested()) {
        requestShutdown();
        return;
    }

    if (m_splash) m_splash->stop();
    delete m_splash;
    m_splash = nullptr;

    // Log this only after AppInitMain finishes, as then logging setup is guaranteed complete
    qInfo() << "Platform customization:" << platformStyle->getName();
    clientModel = new ClientModel(node(), optionsModel);
    window->setClientModel(clientModel, &tip_info);

    // If '-min' option passed, start window minimized (iconified) or minimized to tray
    const bool start_minimized = m_start_minimized;
#ifdef ENABLE_WALLET
    if (m_wallet_enabled) {
        m_wallet_controller = new WalletController(*clientModel, platformStyle, this);
        window->setWalletController(m_wallet_controller, /*show_loading_minimized=*/start_minimized);
        if (paymentServer) {
            paymentServer->setOptionsModel(optionsModel);
        }
    }
#endif // ENABLE_WALLET

    // Show or minimize window
    if (!start_minimized) {
        window->show();
    } else if (clientModel->getOptionsModel()->getMinimizeToTray() && window->hasTrayIcon()) {
        // do nothing as the window is managed by the tray icon
    } else {
        window->showMinimized();
    }
    Q_EMIT windowShown(window);

#ifdef ENABLE_WALLET
    // Now that initialization/startup is done, process any command-line
    // connectcoin: URIs or payment requests:
    if (paymentServer) {
        connect(paymentServer, &PaymentServer::receivedPaymentRequest, window, &BitcoinGUI::handlePaymentRequest);
        connect(window, &BitcoinGUI::receivedURI, paymentServer, &PaymentServer::handleURIOrFile);
        connect(paymentServer, &PaymentServer::message, [this](const QString& title, const QString& message, unsigned int style) {
            window->message(title, message, style);
        });
        QTimer::singleShot(100ms, paymentServer, &PaymentServer::uiReady);
    }
#endif
    pollShutdownTimer->start(SHUTDOWN_POLLING_DELAY);
}

void BitcoinApplication::handleRunawayException(const QString &message)
{
    QMessageBox::critical(
        nullptr, tr("Runaway exception"),
        tr("A fatal error occurred. %1 can no longer continue safely and will quit.").arg(CLIENT_NAME) +
        QLatin1String("<br><br>") + GUIUtil::MakeHtmlLink(message, CLIENT_BUGREPORT));
#ifdef ENABLE_WALLET
    // exit() skips the application destructor but still destroys process
    // state. Probe phases need no GUI callbacks, and blocked DNS is not joined.
    wallet::ShutdownP2CRsaProbes();
#endif
    ::exit(EXIT_FAILURE);
}

void BitcoinApplication::handleNonFatalException(const QString& message)
{
    assert(QThread::currentThread() == thread());
    QMessageBox::warning(
        nullptr, tr("Internal error"),
        tr("An internal error occurred. %1 will attempt to continue safely. This is "
           "an unexpected bug which can be reported as described below.").arg(CLIENT_NAME) +
        QLatin1String("<br><br>") + GUIUtil::MakeHtmlLink(message, CLIENT_BUGREPORT));
}

WId BitcoinApplication::getMainWinId() const
{
    if (!window)
        return 0;

    return window->winId();
}

bool BitcoinApplication::event(QEvent* e)
{
    if (e->type() == QEvent::Quit) {
        requestShutdown();
        return true;
    }

    return QApplication::event(e);
}

static void SetupUIArgs(ArgsManager& argsman)
{
    argsman.AddArg("-choosedatadir", strprintf("Choose data directory on startup (default: %u)", DEFAULT_CHOOSE_DATADIR), ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-lang=<lang>", "Set language, for example \"de_DE\" (default: system locale)", ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-min", "Start minimized", ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-popupnotifications", "Enable desktop pop-up notifications (default: 0). Error and confirmation dialogs are not affected.", ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-resetguisettings", "Reset all settings changed in the GUI", ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-splash", strprintf("Show splash screen on startup (default: %u)", DEFAULT_SPLASHSCREEN), ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-uiplatform", strprintf("Select platform to customize UI for (one of windows, macosx, other; default: %s)", BitcoinGUI::DEFAULT_UIPLATFORM), ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::GUI);
}

int GuiMain(int argc, char* argv[])
{
    std::unique_ptr<interfaces::Init> init = interfaces::MakeGuiInit(argc, argv);

    SetupEnvironment();
    util::ThreadSetInternalName("main");

    // Subscribe to global signals from core
    btcsignals::scoped_connection handler_message_box{::uiInterface.ThreadSafeMessageBox.connect(noui_ThreadSafeMessageBox)};
    btcsignals::scoped_connection handler_question{::uiInterface.ThreadSafeQuestion.connect(noui_ThreadSafeQuestion)};
    btcsignals::scoped_connection handler_init_message{::uiInterface.InitMessage.connect(noui_InitMessage)};

    // Do not refer to data directory yet, this can be overridden by Intro::pickDataDirectory

    /// 1. Basic Qt initialization (not dependent on parameters or configuration)
    Q_INIT_RESOURCE(bitcoin);
    Q_INIT_RESOURCE(bitcoin_locale);

#if defined(QT_QPA_PLATFORM_ANDROID)
    QApplication::setAttribute(Qt::AA_DontUseNativeMenuBar);
    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
#endif

    BitcoinApplication app;
    // Responsive startup waits can show Qt dialogs before settings/configuration
    // are loaded. Route their diagnostics through the buffered core logger too,
    // rather than leaking platform-plugin warnings directly to stderr.
    app.startQtLogging();
    GUIUtil::LoadFont(QStringLiteral(":/fonts/monospace"));

    /// 2. Parse command-line options. We do this after qt in order to show an error if there are problems parsing these
    // Command-line options take precedence:
    SetupServerArgs(gArgs, init->canListenIpc());
    SetupUIArgs(gArgs);
    std::string error;
    if (!gArgs.ParseParameters(argc, argv, error)) {
        InitError(Untranslated(strprintf("Error parsing command line arguments: %s", error)));
        // Create a message box, because the gui has neither been created nor has subscribed to core signals
        QMessageBox::critical(nullptr, CLIENT_NAME,
            // message cannot be translated because translations have not been initialized
            QString::fromStdString("Error parsing command line arguments: %1.").arg(QString::fromStdString(error)));
        return EXIT_FAILURE;
    }

    // Error out when loose non-argument tokens are encountered on command line
    // However, allow BIP-21 URIs only if no options follow
    bool payment_server_token_seen = false;
    for (int i = 1; i < argc; i++) {
        QString arg(argv[i]);
        bool invalid_token = !arg.startsWith("-");
#ifdef ENABLE_WALLET
        if (arg.startsWith(CONNECTCOIN_IPC_PREFIX, Qt::CaseInsensitive)) {
            invalid_token &= false;
            payment_server_token_seen = true;
        }
#endif
        if (payment_server_token_seen && arg.startsWith("-")) {
            InitError(Untranslated(strprintf("Options ('%s') cannot follow a BIP-21 payment URI", argv[i])));
            QMessageBox::critical(nullptr, CLIENT_NAME,
                                  // message cannot be translated because translations have not been initialized
                                  QString::fromStdString("Options ('%1') cannot follow a BIP-21 payment URI").arg(QString::fromStdString(argv[i])));
            return EXIT_FAILURE;
        }
        if (invalid_token) {
            InitError(Untranslated(strprintf("Command line contains unexpected token '%s', see connectcoin-qt -h for a list of options.", argv[i])));
            QMessageBox::critical(nullptr, CLIENT_NAME,
                                  // message cannot be translated because translations have not been initialized
                                  QString::fromStdString("Command line contains unexpected token '%1', see connectcoin-qt -h for a list of options.").arg(QString::fromStdString(argv[i])));
            return EXIT_FAILURE;
        }
    }

    // Now that the QApplication is setup and we have parsed our parameters, we can set the platform style
    app.setupPlatformStyle();

    /// 3. Application identification
    // must be set before OptionsModel is initialized or translations are loaded,
    // as it is used to locate QSettings
    QApplication::setOrganizationName(QAPP_ORG_NAME);
    QApplication::setOrganizationDomain(QAPP_ORG_DOMAIN);
    QApplication::setApplicationName(QAPP_APP_NAME_DEFAULT);
    app.loadGuiPreferences();

    /// 4. Initialization of translations, so that intro dialog is in user's language
    // Now that QSettings are accessible, initialize translations
    app.loadTranslations();

    // Show help message immediately after parsing command-line options (for "-lang") and setting locale,
    // but before showing splash screen.
    if (HelpRequested(gArgs) || gArgs.GetBoolArg("-version", false)) {
        const bool about = gArgs.GetBoolArg("-version", false);
        const auto options = about ? QString{} : HelpMessageDialog::loadHelpOptions();
        HelpMessageDialog help(nullptr, about, options);
        help.showOrPrint();
        return EXIT_SUCCESS;
    }

    // Install global event filter that makes sure that long tooltips can be word-wrapped
    app.installEventFilter(new GUIUtil::ToolTipToRichTextFilter(TOOLTIP_WRAP_THRESHOLD, &app));

    /// 5. Now that settings and translations are available, ask user for data directory
    // User language is set up: pick a data directory
    bool did_show_intro = false;
    int64_t prune_MiB = 0;  // Intro dialog prune configuration
    // Gracefully exit if the user cancels
    if (!Intro::showIfNeeded(did_show_intro, prune_MiB)) return EXIT_SUCCESS;

    /// 6-7. Parse connectcoin.conf, determine network, switch to network specific
    /// options, and create datadir and settings.json.
    // - Do not call gArgs.GetDataDirNet() before this step finishes
    // - Do not call Params() before this step
    // - QSettings() will use the new application name after this, resulting in network-specific settings
    // - Needs to be done before createOptionsModel
    auto config_error = GUIUtil::WaitForBackendTask(std::async(std::launch::async, [] {
        auto error = common::InitConfig(gArgs, [](const bilingual_str& message, const std::vector<std::string>& details) {
            // Settings I/O runs on the worker, but its reset/abort decision is
            // still made by the user on the GUI. The blocking dispatch keeps
            // the local result alive until the dialog has returned.
            bool abort{true};
            const bool invoked = QMetaObject::invokeMethod(qApp, [&abort, message, details] {
                abort = ErrorSettingsRead(message, details);
            }, Qt::BlockingQueuedConnection);
            assert(invoked);
            return abort;
        });
#ifdef ENABLE_WALLET
        if (!error) WalletModel::refreshWalletEnabled();
#endif
        return error;
    }));
    if (auto& error = config_error) {
        InitError(error->message, error->details);
        if (error->status == common::ConfigStatus::FAILED_WRITE) {
            // Show a custom error message to provide more information in the
            // case of a datadir write error.
            ErrorSettingsWrite(error->message, error->details);
        } else if (error->status != common::ConfigStatus::ABORTED) {
            // Show a generic message in other cases, and no additional error
            // message in the case of a read error if the user decided to abort.
            QMessageBox::critical(nullptr, CLIENT_NAME, QObject::tr("Error: %1").arg(QString::fromStdString(error->message.translated)));
        }
        return EXIT_FAILURE;
    }
#ifdef ENABLE_WALLET
    // Parse URIs on command line
    PaymentServer::ipcParseCommandLine(argc, argv);
#endif

    QScopedPointer<const NetworkStyle> networkStyle(NetworkStyle::instantiate(Params().GetChainType()));
    assert(!networkStyle.isNull());
    // Allow for separate UI settings for testnets
    QApplication::setApplicationName(networkStyle->getAppName());
    app.loadGuiPreferences();
    // Re-initialize translations after changing application name (language in network-specific settings can be different)
    app.loadTranslations();

#ifdef ENABLE_WALLET
    /// 8. URI IPC sending
    // - Do this early as we don't want to bother initializing if we are just calling IPC
    // - Do this *after* setting up the data directory, as the data directory hash is used in the name
    // of the server.
    // - Do this after creating app and setting up translations, so errors are
    // translated properly.
    if (PaymentServer::ipcSendCommandLine())
        return EXIT_SUCCESS;

    // Start up the payment server early, too, so impatient users that click on
    // connectcoin: links repeatedly have their payment requests routed to this process:
    if (WalletModel::isWalletEnabled()) {
        app.createPaymentServer();
    }
#endif // ENABLE_WALLET

    /// 9. Main GUI initialization
    // Install global event filter that makes sure that out-of-focus labels do not contain text cursor.
    app.installEventFilter(new GUIUtil::LabelOutOfFocusEventFilter(&app));
#if defined(Q_OS_WIN)
    // Install global event filter for processing Windows session related Windows messages (WM_QUERYENDSESSION and WM_ENDSESSION)
    // Early requests are remembered until the node/executor is ready. The
    // native callback must never synchronously join backend threads.
    qApp->installNativeEventFilter(new WinShutdownMonitor([&app] { app.requestShutdown(); }));
#endif
    // Allow parameter interaction before we create the options model
    app.parameterSetup();
    GUIUtil::LogQtInfo();

    if (gArgs.GetBoolArg("-splash", DEFAULT_SPLASHSCREEN) && !gArgs.GetBoolArg("-min", false))
        app.createSplashScreen(networkStyle.data());

    app.createNode(*init);

    // Load GUI settings from QSettings
    if (!app.createOptionsModel(gArgs.GetBoolArg("-resetguisettings", false))) {
        return EXIT_FAILURE;
    }

    if (did_show_intro) {
        // Store intro dialog settings other than datadir (network specific)
        app.InitPruneSetting(prune_MiB);
    }

    try
    {
        app.createWindow(networkStyle.data());
        // Complete base initialization responsively before starting the
        // long-running initialization/shutdown executor.
        if (app.baseInitialize()) {
            app.requestInitialize();
#if defined(Q_OS_WIN)
            WinShutdownMonitor::registerShutdownBlockReason(QObject::tr("%1 didn't yet exit safely…").arg(CLIENT_NAME), (HWND)app.getMainWinId());
#endif
            app.exec();
        } else {
            // A dialog with detailed error will have been shown by InitError()
            return EXIT_FAILURE;
        }
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(app.node().getWarnings().translated));
    } catch (...) {
        PrintExceptionContinue(nullptr, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(app.node().getWarnings().translated));
    }
    return app.node().getExitStatus();
}
