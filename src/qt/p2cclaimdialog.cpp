// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/p2cclaimdialog.h>
#include <interfaces/wallet.h>
#include <qt/walletmodel.h>
#include <univalue.h>
#include <wallet/p2c_worker.h>

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <exception>
#include <limits>
#include <string>
#include <vector>

P2CClaimDialog::P2CClaimDialog(QWidget* parent) : QWidget(parent)
{
    auto* outer_layout = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("p2cClaimScrollArea");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    scroll->setWidget(content);
    outer_layout->addWidget(scroll);
    auto* explanation = new QLabel(tr("Automatically discover confirmed bounties, generate TLS proofs and send rewards to this wallet. Fees come only from each reward. No private-key unlock is needed while receiving addresses remain in the keypool. HTTPS is disabled until you explicitly start it."), this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_address = new QLineEdit(this);
    m_address->setObjectName("p2cClaimAddress");
    m_address->setPlaceholderText(tr("Optional: empty uses this wallet"));
    form->addRow(tr("Reward address:"), m_address);
    auto* destination_hint = new QLabel(tr("An optional reward address overrides this wallet. Completed proofs keep their original destination when you change it."), this);
    destination_hint->setWordWrap(true);
    form->addRow(QString{}, destination_hint);
    m_rate = new QSpinBox(this);
    m_rate->setObjectName("p2cClaimRate");
    m_rate->setRange(0, 1'000'000);
    // This is only the proposed rate. The worker remains disabled until the
    // user presses Apply / start and accepts the confirmation.
    m_rate->setValue(100);
    form->addRow(tr("Connections per second (this wallet):"), m_rate);
    auto* rate_hint = new QLabel(tr("0 disables HTTPS. For no rate limit, select Unlimited rate below."), this);
    rate_hint->setWordWrap(true);
    form->addRow(QString{}, rate_hint);
    m_unlimited = new QCheckBox(tr("Unlimited rate (explicit opt-in)"), this);
    m_unlimited->setObjectName("p2cClaimUnlimited");
    form->addRow(m_unlimited);
    connect(m_unlimited, &QCheckBox::toggled, this, [this](bool enabled) { m_rate->setEnabled(!enabled); });
    m_concurrency = new QSpinBox(this);
    m_concurrency->setObjectName("p2cClaimConcurrency");
    m_concurrency->setRange(1, std::numeric_limits<int>::max());
    m_concurrency->setValue(wallet::DEFAULT_P2C_CLAIM_CONCURRENCY);
    form->addRow(tr("Simultaneous connections:"), m_concurrency);
    m_load_warning = new QLabel(tr("Warning: more than 100 connections per second, unlimited rate, or more than 100 simultaneous connections may overload your computer or network and disconnect this node."), this);
    m_load_warning->setObjectName("p2cClaimLoadWarning");
    m_load_warning->setTextFormat(Qt::PlainText);
    m_load_warning->setWordWrap(true);
    m_load_warning->setMargin(10);
    m_load_warning->setStyleSheet("QLabel { color: white; background-color: #b00020; font-weight: bold; border-radius: 4px; }");
    form->addRow(m_load_warning);
    connect(m_rate, &QSpinBox::valueChanged, this, &P2CClaimDialog::UpdateLoadWarning);
    connect(m_concurrency, &QSpinBox::valueChanged, this, &P2CClaimDialog::UpdateLoadWarning);
    connect(m_unlimited, &QCheckBox::toggled, this, &P2CClaimDialog::UpdateLoadWarning);
    UpdateLoadWarning();
    m_recent_blocks = new QSpinBox(this);
    m_recent_blocks->setObjectName("p2cClaimRecentBlocks");
    m_recent_blocks->setRange(1, std::numeric_limits<int>::max());
    m_recent_blocks->setValue(wallet::DEFAULT_P2C_BOUNTY_LOOKBACK);
    form->addRow(tr("Recent blocks to search:"), m_recent_blocks);
    m_unlimited_history = new QCheckBox(tr("Unlimited block history"), this);
    m_unlimited_history->setObjectName("p2cClaimUnlimitedHistory");
    form->addRow(m_unlimited_history);
    connect(m_unlimited_history, &QCheckBox::toggled, this, [this](bool enabled) { m_recent_blocks->setEnabled(!enabled); });
    auto* history_warning = new QLabel(tr("Large limits or unlimited history may include old, unproductive P2C bounties (\"trash bounties\")."), this);
    history_warning->setObjectName("p2cClaimHistoryWarning");
    history_warning->setWordWrap(true);
    form->addRow(QString{}, history_warning);
    m_domains = new QLineEdit(this);
    m_domains->setObjectName("p2cClaimDomains");
    m_domains->setMaxLength(65535);
    m_domains->setPlaceholderText(tr("Optional: example.com, another.example (empty = all domains)"));
    form->addRow(tr("Domain allowlist:"), m_domains);
    layout->addLayout(form);
    m_start = new QPushButton(tr("Apply / start automatic claiming"), this);
    m_start->setObjectName("p2cClaimStart");
    m_stop = new QPushButton(tr("Stop HTTPS (0)"), this);
    m_stop->setObjectName("p2cClaimStop");
    layout->addWidget(m_start);
    layout->addWidget(m_stop);
    m_status = new QLabel(tr("Disabled"), this);
    m_status->setObjectName("p2cClaimStatus");
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);
    m_reward_status = new QLabel(this);
    m_reward_status->setObjectName("p2cClaimRewardStatus");
    m_reward_status->setTextFormat(Qt::PlainText);
    m_reward_status->setWordWrap(true);
    m_reward_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_reward_status);
    layout->addStretch();
    connect(m_start, &QPushButton::clicked, this, [this] { Configure(false); });
    connect(m_stop, &QPushButton::clicked, this, [this] { Configure(true); });
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &P2CClaimDialog::Refresh);
    timer->start(500);
    setModel(nullptr);
}

void P2CClaimDialog::setModel(WalletModel* model)
{
    if (m_model) disconnect(m_model, nullptr, this, nullptr);
    m_model = model;
    if (model) connect(model, &QObject::destroyed, this, [this] { setModel(nullptr); });
    m_active_high_load = false;
    UpdateLoadWarning();
    setEnabled(model != nullptr);
    Refresh();
}

void P2CClaimDialog::UpdateLoadWarning()
{
    // Advisory only: neither high settings nor an unlimited rate block Start.
    // Keep warning about an active configuration even while editing lower values.
    m_load_warning->setVisible(m_active_high_load || m_unlimited->isChecked() || m_rate->value() > 100 || m_concurrency->value() > 100);
}

void P2CClaimDialog::Configure(bool stop)
{
    if (!m_model || m_operation.valid()) return;
    m_configuration_error.clear();
    const int rate{stop ? 0 : m_unlimited->isChecked() ? -1 : m_rate->value()};
    const int concurrency{m_concurrency->value()};
    const int recent_blocks{m_unlimited_history->isChecked() ? 0 : m_recent_blocks->value()};
    const QString reward_address{stop ? QString{} : m_address->text().trimmed()};
    std::vector<std::string> domains;
    for (const auto& domain : m_domains->text().split(',', Qt::SkipEmptyParts)) domains.push_back(domain.trimmed().toStdString());
    if (rate != 0) {
        const QPointer<P2CClaimDialog> guard{this};
        const QPointer<WalletModel> model{m_model};
        const bool high_load{rate < 0 || rate > 100 || concurrency > 100};
        auto* confirmation = new QMessageBox(high_load ? QMessageBox::Warning : QMessageBox::Question, tr("Enable automatic P2C claiming?"),
            tr("This makes direct HTTPS connections to public domains and automatically submits successful claims. Your IP address is visible to those servers. Proxy configurations are not bypassed. Fees are deducted from rewards.\n\n%1\n\nContinue?")
                .arg((rate < 0 ? tr("You selected UNLIMITED connections per second.") : tr("Rate: %1 connections per second for this wallet.").arg(rate)) +
                     QStringLiteral("\n") + tr("Reward target: %1").arg(reward_address.isEmpty() ? tr("This wallet (default)") : reward_address.toHtmlEscaped())),
            QMessageBox::Yes | QMessageBox::No, this);
        if (high_load) confirmation->setInformativeText(m_load_warning->text());
        // The wallet/page can disappear during exec(). A stack-owned modal
        // parented to this page would also be destroyed by its parent's teardown.
        confirmation->setAttribute(Qt::WA_DeleteOnClose);
        confirmation->setDefaultButton(QMessageBox::No);
        const auto answer = confirmation->exec();
        if (!guard || !model || answer != QMessageBox::Yes) return;
        if (m_model != model) return;
    }
    if (stop) { m_unlimited->setChecked(false); m_rate->setValue(0); }
    try {
        m_operation = m_model->wallet().configureP2CClaiming(rate, concurrency, stop ? std::vector<std::string>{} : std::move(domains),
                                                          reward_address.toStdString(), recent_blocks);
    } catch (const std::exception& error) {
        m_configuration_error = QString::fromUtf8(error.what());
        m_status->setText(m_configuration_error);
        return;
    }
    m_start->setEnabled(false);
    m_stop->setEnabled(false);
    m_status->setText(tr("Applying configuration…"));
}

QString P2CClaimDialog::StateText(const std::string& state)
{
    // RPC state identifiers stay stable and untranslated. Only the GUI maps
    // them to localized text, so automation does not depend on the UI language.
    if (state == "disabled") return tr("Disabled");
    if (state == "starting") return tr("Starting");
    if (state == "resolving") return tr("Resolving domain");
    if (state == "retrying domain resolution") return tr("Retrying domain resolution");
    if (state == "searching") return tr("Searching for proofs");
    if (state == "retrying connections") return tr("Retrying connections");
    if (state == "certificate rejected") return tr("Certificate rejected");
    if (state == "stopped with error") return tr("Stopped with an error");
    if (state == "bounty spent") return tr("Bounty already claimed");
    if (state == "scanning confirmed bounties") return tr("Scanning confirmed bounties");
    if (state == "waiting for bounties") return tr("Waiting for bounties");
    if (state == "waiting for eligible bounties") return tr("Waiting for eligible bounties");
    if (state == "bounty skipped") return tr("Bounty skipped");
    if (state == "submitted") return tr("Submitted");
    if (state == "stored; check wallet history") return tr("Stored; check wallet history");
    return QString::fromStdString(state);
}

void P2CClaimDialog::Refresh()
{
    if (!m_model) return;
    try {
        if (m_operation.valid()) {
            if (m_operation.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
            auto result = m_operation.get();
            m_start->setEnabled(true);
            m_stop->setEnabled(true);
            if (!result.empty()) {
                m_configuration_error = QString::fromStdString(result);
            }
        }
        const auto progress = m_model->wallet().getP2CClaimStatus();
        const int rate = progress["connections_per_second"].getInt<int>();
        m_active_high_load = rate != 0 && (rate < 0 || rate > 100 || progress["concurrency"].getInt<int>() > 100);
        UpdateLoadWarning();
        const QString rate_text = rate == -1 ? tr("Unlimited") : rate == 0 ? tr("Disabled (0)") : QString::number(rate);
        const auto reward_address{QString::fromStdString(progress["reward_address"].get_str())};
        m_reward_status->setText(tr("Reward target: %1").arg(reward_address.isEmpty() ? tr("This wallet (default)") : reward_address));
        m_status->setText(tr("State: %1\nActive rate: %2 | Concurrency: %3\nDomain: %4\nAttempts: %5 | Submitted: %6\nLast claim: %7\n%8")
            .arg(StateText(progress["state"].get_str()))
            .arg(rate_text)
            .arg(progress["concurrency"].getInt<int>())
            .arg(QString::fromStdString(progress["domain"].get_str()))
            .arg(static_cast<qulonglong>(progress["attempts"].getInt<uint64_t>()))
            .arg(static_cast<qulonglong>(progress["submitted"].getInt<uint64_t>()))
            .arg(QString::fromStdString(progress["last_txid"].get_str()))
            .arg(m_configuration_error.isEmpty() ? QString::fromStdString(progress["last_error"].get_str()) : m_configuration_error));
    } catch (const std::exception& error) {
        m_status->setText(QString::fromUtf8(error.what()));
    }
}
