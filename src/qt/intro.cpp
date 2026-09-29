// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <chainparams.h>
#include <qt/intro.h>
#include <qt/forms/ui_intro.h>
#include <util/chaintype.h>
#include <util/fs.h>

#include <qt/freespacechecker.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>

#include <common/args.h>
#include <interfaces/node.h>
#include <node/interface_ui.h>
#include <util/fs_helpers.h>
#include <util/translation.h>
#include <validation.h>

#include <QFileDialog>
#include <qt/guipreferences.h>
#include <QMessageBox>
#include <QPointer>

#include <cmath>
#include <cstdlib>
#include <utility>

namespace {
//! Return pruning size that will be used if automatic pruning is enabled.
int GetPruneTargetGB()
{
    int64_t prune_target_mib = gArgs.GetIntArg("-prune", 0);
    // >1 means automatic pruning is enabled by config, 1 means manual pruning, 0 means no pruning.
    return prune_target_mib > 1 ? PruneMiBtoGB(prune_target_mib) : DEFAULT_PRUNE_TARGET_GB;
}
} // namespace

Intro::Intro(QWidget *parent, int64_t blockchain_size_gb, int64_t chain_state_size_gb, QString default_data_directory) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::Intro),
    m_default_data_directory(std::move(default_data_directory)),
    m_blockchain_size_gb(blockchain_size_gb),
    m_chain_state_size_gb(chain_state_size_gb),
    m_prune_target_gb{GetPruneTargetGB()}
{
    ui->setupUi(this);
    ui->welcomeLabel->setText(ui->welcomeLabel->text().arg(CLIENT_NAME));
    ui->storageLabel->setText(ui->storageLabel->text().arg(CLIENT_NAME));

    ui->lblExplanation1->setText(ui->lblExplanation1->text()
        .arg(CLIENT_NAME)
        .arg(m_blockchain_size_gb)
        .arg(2009)
        .arg(tr("ConnectCoin"))
    );
    ui->lblExplanation2->setText(ui->lblExplanation2->text().arg(CLIENT_NAME));

    const int min_prune_target_GB = std::ceil(MIN_DISK_SPACE_FOR_BLOCK_FILES / 1e9);
    ui->pruneGB->setRange(min_prune_target_GB, std::numeric_limits<int>::max());
    if (const auto arg{gArgs.GetIntArg("-prune")}) {
        m_prune_checkbox_is_default = false;
        ui->prune->setChecked(*arg >= 1);
        ui->prune->setEnabled(false);
    }
    ui->pruneGB->setValue(m_prune_target_gb);
    ui->pruneGB->setToolTip(ui->prune->toolTip());
    ui->lblPruneSuffix->setToolTip(ui->prune->toolTip());
    UpdatePruneLabels(ui->prune->isChecked());

    connect(ui->prune, &QCheckBox::toggled, [this](bool prune_checked) {
        m_prune_checkbox_is_default = false;
        UpdatePruneLabels(prune_checked);
        UpdateFreeSpaceLabel();
    });
    connect(ui->pruneGB, qOverload<int>(&QSpinBox::valueChanged), [this](int prune_GB) {
        m_prune_target_gb = prune_GB;
        UpdatePruneLabels(ui->prune->isChecked());
        UpdateFreeSpaceLabel();
    });

    startThread();
}

Intro::~Intro()
{
    // Normal dialog completion waits asynchronously in done(). Keep a safe
    // non-reentrant fallback for direct deletion/exceptional ownership paths.
    thread->quit();
    thread->wait();
    delete ui;
}

void Intro::done(int result)
{
    if (m_closing) return;
    if (!thread->isRunning()) {
        QDialog::done(result);
        return;
    }
    // A filesystem probe cannot necessarily be interrupted. Keep dispatching
    // events until it finishes, with the fully constructed dialog still alive.
    m_closing = true;
    m_close_result = result;
    setEnabled(false);
    thread->quit();
}

void Intro::showEvent(QShowEvent* event)
{
    // Directory creation can fail after acceptance, reopening this dialog.
    if (!thread->isRunning()) {
        startThread();
        checkPath(ui->dataDirectory->text());
    }
    QDialog::showEvent(event);
}

QString Intro::getDataDirectory()
{
    return ui->dataDirectory->text();
}

void Intro::setDataDirectory(const QString &dataDir)
{
    GUIUtil::BackendOperationGuard operation;
    const QPointer<Intro> guard{this};
    const auto default_directory = defaultDataDirectory();
    if (!guard) return;
    ui->dataDirectory->setText(dataDir);
    if(dataDir == default_directory)
    {
        ui->dataDirDefault->setChecked(true);
        ui->dataDirectory->setEnabled(false);
        ui->ellipsisButton->setEnabled(false);
    } else {
        ui->dataDirCustom->setChecked(true);
        ui->dataDirectory->setEnabled(true);
        ui->ellipsisButton->setEnabled(true);
    }
}

QString Intro::defaultDataDirectory()
{
    if (!m_default_data_directory.isNull()) return m_default_data_directory;
    // Direct users/tests may construct Intro without the startup snapshot.
    // Resolve the shell path only at this fully constructed, explicit action
    // boundary; neither construction nor later edits perform filesystem work.
    const QPointer<Intro> guard{this};
    auto path = GUIUtil::WaitForBackendTask(std::async(std::launch::async, [] {
        return GUIUtil::getDefaultDataDirectory();
    }), this);
    if (guard) m_default_data_directory = path;
    return path;
}

int64_t Intro::getPruneMiB() const
{
    switch (ui->prune->checkState()) {
    case Qt::Checked:
        return PruneGBtoMiB(m_prune_target_gb);
    case Qt::Unchecked: default:
        return 0;
    }
}

bool Intro::showIfNeeded(bool& did_show_intro, int64_t& prune_MiB)
{
    did_show_intro = false;

    GuiSettings settings;
    /* If data directory provided on command line, no need to look at settings
       or show a picking dialog */
    if(!gArgs.GetArg("-datadir", "").empty())
        return true;
    /* 1) Default data directory for operating system */
    const auto default_directory = GUIUtil::WaitForBackendTask(std::async(std::launch::async, [] {
        return GUIUtil::getDefaultDataDirectory();
    }));
    QString dataDir = default_directory;
    /* 2) Allow QSettings to override default dir */
    dataDir = settings.value("strDataDir", dataDir).toString();

    const bool directory_exists = GUIUtil::WaitForBackendTask(std::async(std::launch::async,
        [path = GUIUtil::QStringToPath(dataDir)] { return fs::exists(path); }));
    if(!directory_exists || gArgs.GetBoolArg("-choosedatadir", DEFAULT_CHOOSE_DATADIR) || settings.value("fReset", false).toBool() || gArgs.GetBoolArg("-resetguisettings", false))
    {
        /* Use selectParams here to guarantee Params() can be used by node interface */
        try {
            SelectParams(gArgs.GetChainType());
        } catch (const std::exception& e) {
            InitError(Untranslated(e.what()));
            QMessageBox::critical(nullptr, CLIENT_NAME, QObject::tr("Error: %1").arg(QString(e.what())));
            std::exit(EXIT_FAILURE);
        }

        /* If current default data directory does not exist, let the user choose one */
        Intro intro(nullptr, Params().AssumedBlockchainSize(), Params().AssumedChainStateSize(), default_directory);
        intro.setDataDirectory(dataDir);
        intro.setWindowIcon(QIcon(":icons/bitcoin"));
        did_show_intro = true;

        while(true)
        {
            if(!intro.exec())
            {
                /* Cancel clicked */
                return false;
            }
            dataDir = intro.getDataDirectory();
            try {
                GUIUtil::WaitForBackendTask(std::async(std::launch::async,
                    [path = GUIUtil::QStringToPath(dataDir)] {
                        if (TryCreateDirectories(path)) TryCreateDirectories(path / "wallets");
                    }));
                break;
            } catch (const fs::filesystem_error&) {
                QMessageBox::critical(nullptr, CLIENT_NAME,
                    tr("Error: Specified data directory \"%1\" cannot be created.").arg(dataDir));
                /* fall through, back to choosing screen */
            }
        }

        // Additional preferences:
        prune_MiB = intro.getPruneMiB();

        settings.setValue("strDataDir", dataDir);
        settings.setValue("fReset", false);
        GUIUtil::WaitForBackendTask(GuiPreferences::Flush());
    }
    /* Only override -datadir if different from the default, to make it possible to
     * override -datadir in the connectcoin.conf file in the default data directory
     * (to be consistent with connectcoind behavior)
     */
    if(dataDir != default_directory) {
        gArgs.SoftSetArg("-datadir", fs::PathToString(GUIUtil::QStringToPath(dataDir))); // use OS locale for path setting
    }
    return true;
}

void Intro::setStatus(int status, const QString &message, quint64 bytesAvailable)
{
    switch(status)
    {
    case FreespaceChecker::ST_OK:
        ui->errorMessage->setText(message);
        ui->errorMessage->setStyleSheet("");
        break;
    case FreespaceChecker::ST_ERROR:
        ui->errorMessage->setText(tr("Error") + ": " + message);
        ui->errorMessage->setStyleSheet("QLabel { color: #800000 }");
        break;
    }
    /* Indicate number of bytes available */
    if(status == FreespaceChecker::ST_ERROR)
    {
        ui->freeSpace->setText("");
    } else {
        m_bytes_available = bytesAvailable;
        if (ui->prune->isEnabled() && m_prune_checkbox_is_default) {
            ui->prune->setChecked(m_bytes_available < (m_blockchain_size_gb + m_chain_state_size_gb + 10) * GB_BYTES);
        }
        UpdateFreeSpaceLabel();
    }
    /* Don't allow confirm in ERROR state */
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(status != FreespaceChecker::ST_ERROR);
}

void Intro::UpdateFreeSpaceLabel()
{
    QString freeString = tr("%n GB of space available", "", m_bytes_available / GB_BYTES);
    if (m_bytes_available < m_required_space_gb * GB_BYTES) {
        freeString += " " + tr("(of %n GB needed)", "", m_required_space_gb);
        ui->freeSpace->setStyleSheet("QLabel { color: #800000 }");
    } else if (m_bytes_available / GB_BYTES - m_required_space_gb < 10) {
        freeString += " " + tr("(%n GB needed for full chain)", "", m_required_space_gb);
        ui->freeSpace->setStyleSheet("QLabel { color: #999900 }");
    } else {
        ui->freeSpace->setStyleSheet("");
    }
    ui->freeSpace->setText(freeString + ".");
}

void Intro::on_dataDirectory_textChanged(const QString &dataDirStr)
{
    /* Disable OK button until check result comes in */
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    checkPath(dataDirStr);
}

void Intro::on_ellipsisButton_clicked()
{
    QString dir = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(nullptr, tr("Choose data directory"), ui->dataDirectory->text()));
    if(!dir.isEmpty())
        ui->dataDirectory->setText(dir);
}

void Intro::on_dataDirDefault_clicked()
{
    GUIUtil::BackendOperationGuard operation;
    const QPointer<Intro> guard{this};
    const auto path = defaultDataDirectory();
    if (guard) setDataDirectory(path);
}

void Intro::on_dataDirCustom_clicked()
{
    ui->dataDirectory->setEnabled(true);
    ui->ellipsisButton->setEnabled(true);
}

void Intro::startThread()
{
    delete thread; // Only called for an absent or already finished thread.
    thread = new QThread(this);
    FreespaceChecker* executor = new FreespaceChecker;
    executor->moveToThread(thread);

    m_check_running = false;
    connect(executor, &FreespaceChecker::reply, this, [this](const QString& path, int status, const QString& message, quint64 available) {
        m_check_running = false;
        if (m_closing) return;
        if (path != pathToCheck) {
            checkPath(pathToCheck);
            return;
        }
        setStatus(status, message, available);
    });
    connect(this, &Intro::requestCheck, executor, &FreespaceChecker::check);
    /*  make sure executor object is deleted in its own thread */
    connect(thread, &QThread::finished, executor, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this] {
        if (!m_closing) return;
        m_closing = false;
        setEnabled(true);
        QDialog::done(m_close_result);
    });

    thread->start();
}

void Intro::checkPath(const QString &dataDir)
{
    if (m_closing) return;
    pathToCheck = dataDir;
    if (!m_check_running) {
        m_check_running = true;
        Q_EMIT requestCheck(dataDir);
    }
}

void Intro::UpdatePruneLabels(bool prune_checked)
{
    m_required_space_gb = m_blockchain_size_gb + m_chain_state_size_gb;
    QString storageRequiresMsg = tr("At least %1 GB of data will be stored in this directory, and it will grow over time.");
    if (prune_checked && m_prune_target_gb <= m_blockchain_size_gb) {
        m_required_space_gb = m_prune_target_gb + m_chain_state_size_gb;
        storageRequiresMsg = tr("Approximately %1 GB of data will be stored in this directory.");
    }
    ui->lblExplanation3->setVisible(prune_checked);
    ui->pruneGB->setEnabled(prune_checked);
    static constexpr uint64_t nPowTargetSpacing = 10;  // from the public chainparams, which we don't have at this stage
    static constexpr uint32_t expected_block_data_size = 2250000;  // includes undo data
    const uint64_t expected_backup_days = m_prune_target_gb * 1e9 / (uint64_t(expected_block_data_size) * 86400 / nPowTargetSpacing);
    ui->lblPruneSuffix->setText(
        //: Explanatory text on the capability of the current prune target.
        tr("(sufficient to restore backups %n day(s) old)", "", expected_backup_days));
    ui->sizeWarningLabel->setText(
        tr("%1 will download and store a copy of the ConnectCoin blockchain.").arg(CLIENT_NAME) + " " +
        storageRequiresMsg.arg(m_required_space_gb) + " " +
        tr("The wallet will also be stored in this directory.")
    );
    this->adjustSize();
}
