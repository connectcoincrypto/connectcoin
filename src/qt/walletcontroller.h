// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_WALLETCONTROLLER_H
#define CONNECTCOIN_QT_WALLETCONTROLLER_H

#include <qt/sendcoinsrecipient.h>
#include <support/allocators/secure.h>
#include <sync.h>
#include <util/translation.h>

#include <map>
#include <future>
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QMessageBox>
#include <QMutex>
#include <QProgressDialog>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QString>

class ClientModel;
class OptionsModel;
class PlatformStyle;
class WalletModel;

namespace interfaces {
class Handler;
class Node;
class Wallet;
} // namespace interfaces

namespace fs {
class path;
}

class AskPassphraseDialog;
class CreateWalletActivity;
class CreateWalletDialog;
class MigrateWalletActivity;
class OpenWalletActivity;
class WalletControllerActivity;
class WalletControllerLoadState;

/**
 * Controller between interfaces::Node, WalletModel instances and the GUI.
 */
class WalletController : public QObject
{
    Q_OBJECT

    void removeAndDeleteWallet(WalletModel* wallet_model);
    void removeWalletWhenReady(WalletModel* wallet_model);

public:
    WalletController(ClientModel& client_model, const PlatformStyle* platform_style, QObject* parent);
    ~WalletController();
    //! Quiesce callbacks and drain work while the controller and its views are
    //! still intact. Call on the GUI before deletion; never from an activity.
    void stop();
    bool isStopping() const { return m_stopping; }

    WalletModel* getOrCreateWallet(std::unique_ptr<interfaces::Wallet> wallet);

    //! Returns all wallet names in the wallet dir mapped to whether the wallet
    //! is loaded.
    std::map<std::string, std::pair<bool, std::string>> listWalletDir();
    bool hasWalletDirSnapshot() const { return m_has_wallet_dir_snapshot; }
    //! Active asynchronous activities must finish before controller teardown.
    bool hasActiveActivities() const;

    void closeWallet(WalletModel* wallet_model, QWidget* parent = nullptr);
    void closeAllWallets(QWidget* parent = nullptr);

Q_SIGNALS:
    void walletAdded(WalletModel* wallet_model);
    void walletRemoved(WalletModel* wallet_model);
    void walletDirectoryChanged();

    void coinsSent(WalletModel* wallet_model, SendCoinsRecipient recipient, QByteArray transaction);

private:
    QThread* const m_activity_thread;
    QObject* const m_activity_worker;
    ClientModel& m_client_model;
    interfaces::Node& m_node;
    const PlatformStyle* const m_platform_style;
    OptionsModel* const m_options_model;
    mutable QMutex m_mutex;
    std::vector<WalletModel*> m_wallets;
    std::set<WalletModel*> m_wallets_pending_removal;
    std::set<std::string> m_loaded_wallet_names;
    std::map<std::string, std::string> m_cached_wallet_dir;
    std::future<std::map<std::string, std::string>> m_wallet_dir_query;
    QTimer* m_wallet_dir_timer{nullptr};
    bool m_has_wallet_dir_snapshot{false};
    std::unique_ptr<interfaces::Handler> m_handler_load_wallet;
    std::shared_ptr<WalletControllerLoadState> m_load_state;
    std::atomic<bool> m_stopping{false};
    bool m_stop_complete{false};

    friend class WalletControllerActivity;
    friend class LoadWalletsActivity;
    friend class MigrateWalletActivity;
    friend class WalletControllerLoadState;

    //! Starts the wallet closure procedure
    void removeWallet(WalletModel* wallet_model);
    void pollWalletDir();
    //! Register on the activity worker before its initial wallet snapshot.
    void subscribeToCoreSignals();
    void stopImpl(bool responsive);
    WalletModel* registerWalletOnGUI(std::unique_ptr<interfaces::Wallet> wallet);
};

class WalletControllerActivity : public QObject
{
    Q_OBJECT

public:
    WalletControllerActivity(WalletController* wallet_controller, QWidget* parent_widget);
    ~WalletControllerActivity() override;
    bool hasActiveBackendWork() const { return m_backend_active; }

Q_SIGNALS:
    void finished();

protected:
    interfaces::Node& node() const { return m_wallet_controller->m_node; }
    ClientModel& clientModel() const { return m_wallet_controller->m_client_model; }
    QObject* worker() const { return m_wallet_controller->m_activity_worker; }

    void showProgressDialog(const QString& title_text, const QString& label_text, bool show_minimized=false);

    WalletController* const m_wallet_controller;
    QWidget* const m_parent_widget;
    WalletModel* m_wallet_model{nullptr};
    bilingual_str m_error_message;
    std::vector<bilingual_str> m_warning_message;
    bool m_backend_active{false};
    QPointer<QProgressDialog> m_progress_dialog;
};


class CreateWalletActivity : public WalletControllerActivity
{
    Q_OBJECT

public:
    CreateWalletActivity(WalletController* wallet_controller, QWidget* parent_widget);
    virtual ~CreateWalletActivity();

    void create();

Q_SIGNALS:
    void created(WalletModel* wallet_model);

private:
    void askPassphrase();
    void createWallet();
    void finish();

    SecureString m_passphrase;
    CreateWalletDialog* m_create_wallet_dialog{nullptr};
    AskPassphraseDialog* m_passphrase_dialog{nullptr};
};

class OpenWalletActivity : public WalletControllerActivity
{
    Q_OBJECT

public:
    OpenWalletActivity(WalletController* wallet_controller, QWidget* parent_widget);

    void open(const std::string& path);

Q_SIGNALS:
    void opened(WalletModel* wallet_model);

private:
    void finish();
};

class LoadWalletsActivity : public WalletControllerActivity
{
    Q_OBJECT

public:
    LoadWalletsActivity(WalletController* wallet_controller, QWidget* parent_widget);

    void load(bool show_loading_minimized);
};

class RestoreWalletActivity : public WalletControllerActivity
{
    Q_OBJECT

public:
    RestoreWalletActivity(WalletController* wallet_controller, QWidget* parent_widget);

    void restore(const fs::path& backup_file, const std::string& wallet_name);

Q_SIGNALS:
    void restored(WalletModel* wallet_model);

private:
    void finish();
};

class MigrateWalletActivity : public WalletControllerActivity
{
    Q_OBJECT

public:
    MigrateWalletActivity(WalletController* wallet_controller, QWidget* parent) : WalletControllerActivity(wallet_controller, parent) {}

    void restore_and_migrate(const fs::path& path, const std::string& wallet_name);
    void migrate(const std::string& path);

Q_SIGNALS:
    void migrated(WalletModel* wallet_model);

private:
    QString m_success_message;

    void do_migrate(const std::string& name, bool load_wallet);
    void finish();
};

#endif // CONNECTCOIN_QT_WALLETCONTROLLER_H
