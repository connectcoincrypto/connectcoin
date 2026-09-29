// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_BITCOIN_H
#define CONNECTCOIN_QT_BITCOIN_H

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <interfaces/node.h>
#include <qt/initexecutor.h>

#include <cassert>
#include <future>
#include <memory>
#include <optional>

#include <QApplication>

class BitcoinGUI;
class ClientModel;
class GuiPreferences;
class GuiTranslations;
class NetworkStyle;
class OptionsModel;
class PaymentServer;
class PlatformStyle;
class QtLogForwarder;
class SplashScreen;
class WalletController;
class WalletModel;
namespace interfaces {
class Init;
} // namespace interfaces


/** Main ConnectCoin application object */
class BitcoinApplication: public QApplication
{
    Q_OBJECT
public:
    explicit BitcoinApplication();
    ~BitcoinApplication();

#ifdef ENABLE_WALLET
    /// Create payment server
    void createPaymentServer();
#endif
    /// parameter interaction/setup based on rules
    void parameterSetup();
    /// Create options model
    [[nodiscard]] bool createOptionsModel(bool resetSettings);
    /// Initialize prune setting
    void InitPruneSetting(int64_t prune_MiB);
    /// Create main window
    void createWindow(const NetworkStyle *networkStyle);
    /// Create splash screen
    void createSplashScreen(const NetworkStyle *networkStyle);
    /// Create or spawn node
    void createNode(interfaces::Init& init);
    /// Basic initialization, before starting initialization/shutdown thread. Return true on success.
    bool baseInitialize();

    /// Request core initialization
    void requestInitialize();

    /// Get window identifier of QMainWindow (BitcoinGUI)
    WId getMainWinId() const;

    /// Setup platform style
    void setupPlatformStyle();
    /// Forward Qt diagnostics through an application-owned bounded worker.
    void startQtLogging();
    /// Explicit startup boundary, after selecting the application/network name.
    void loadGuiPreferences();
    /// Load catalogs/dependencies off-GUI after selecting the language profile.
    void loadTranslations();

    interfaces::Node& node() const { assert(m_node); return *m_node; }

public Q_SLOTS:
    void initializeResult(bool success, interfaces::BlockAndHeaderTipInfo tip_info);
    /// Request core shutdown
    void requestShutdown();
    /// Handle runaway exceptions. Shows a message box with the problem and quits the program.
    void handleRunawayException(const QString &message);

    /**
     * A helper function that shows a message box
     * with details about a non-fatal exception.
     */
    void handleNonFatalException(const QString& message);

Q_SIGNALS:
    void requestedInitialize();
    void requestedShutdown();
    void windowShown(BitcoinGUI* window);

protected:
    bool event(QEvent* e) override;

private:
    std::optional<InitExecutor> m_executor;
    OptionsModel* optionsModel{nullptr};
    ClientModel* clientModel{nullptr};
    BitcoinGUI* window{nullptr};
    QTimer* pollShutdownTimer{nullptr};
    bool m_initialization_finished{false};
    bool m_start_minimized{false};
    bool m_wallet_enabled{false};
    bool m_shutdown_requested{false};
    bool m_shutdown_started{false};
    bool m_shutdown_retry_pending{false};
    std::future<void> m_shutdown_interrupt;
    std::unique_ptr<QtLogForwarder> m_qt_log_forwarder;
    std::unique_ptr<GuiPreferences> m_gui_preferences;
    std::unique_ptr<GuiTranslations> m_translations;
    QtMessageHandler m_previous_message_handler{nullptr};
#ifdef ENABLE_WALLET
    PaymentServer* paymentServer{nullptr};
    WalletController* m_wallet_controller{nullptr};
#endif
    const PlatformStyle* platformStyle{nullptr};
    std::unique_ptr<QWidget> shutdownWindow;
    SplashScreen* m_splash = nullptr;
    std::unique_ptr<interfaces::Node> m_node;

    void startThread();
};

int GuiMain(int argc, char* argv[]);

#endif // CONNECTCOIN_QT_BITCOIN_H
