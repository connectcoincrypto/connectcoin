// Copyright (c) 2026 The ConnectCoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/miningpage.h>

#include <interfaces/node.h>
#include <outputtype.h>
#include <qt/addresstablemodel.h>
#include <qt/clientmodel.h>
#include <qt/walletmodel.h>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <exception>

MiningPage::MiningPage(WalletModel* wallet_model, QWidget* parent)
    : QWidget{parent}, m_wallet{wallet_model}
{
    auto* layout = new QVBoxLayout(this);
    auto* description = new QLabel(tr("Mine testnet blocks with your CPU using RandomX. Mining is off until you start it. The miner is shared by all wallets in this node and continues when you change tabs or close a wallet."), this);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto* resources = new QLabel(tr("FAST mode shares roughly 2 GiB of RandomX memory with validation. More threads use more CPU and power; leave capacity for the node. Initializing the dataset can take a while."), this);
    resources->setWordWrap(true);
    layout->addWidget(resources);
    auto* form = new QFormLayout;
    m_address = new QLineEdit(this);
    m_address->setObjectName("miningAddress");
    m_address->setPlaceholderText(tr("Reward address for this network"));
    m_new_address = new QPushButton(tr("New address from this wallet"), this);
    m_new_address->setObjectName("newMiningAddress");
    auto* address_row = new QHBoxLayout;
    address_row->addWidget(m_address);
    address_row->addWidget(m_new_address);
    form->addRow(tr("Reward address:"), address_row);
    m_threads = new QSpinBox(this);
    m_threads->setObjectName("miningThreads");
    m_threads->setRange(1, 1);
    m_threads->setValue(1);
    form->addRow(tr("CPU threads:"), m_threads);
    layout->addLayout(form);
    m_thread_warning = new QLabel(this);
    m_thread_warning->setObjectName("miningThreadWarning");
    m_thread_warning->setWordWrap(true);
    layout->addWidget(m_thread_warning);
    auto* buttons = new QHBoxLayout;
    m_start = new QPushButton(tr("Start mining"), this);
    m_start->setObjectName("startMining");
    m_stop = new QPushButton(tr("Stop mining"), this);
    m_stop->setObjectName("stopMining");
    buttons->addWidget(m_start);
    buttons->addWidget(m_stop);
    layout->addLayout(buttons);
    m_status = new QLabel(this);
    m_status->setObjectName("miningStatus");
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);
    layout->addStretch();
    connect(m_start, &QPushButton::clicked, this, &MiningPage::start);
    connect(m_stop, &QPushButton::clicked, this, [this] {
        if (m_client) m_client->node().stopCpuMining();
        refresh();
    });
    connect(m_new_address, &QPushButton::clicked, this, &MiningPage::newAddress);
    connect(m_threads, &QSpinBox::valueChanged, this, &MiningPage::updateThreadWarning);
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MiningPage::refresh);
    timer->start(1000);
    refresh();
}

void MiningPage::setClientModel(ClientModel* model)
{
    m_client = model;
    refresh();
}

void MiningPage::newAddress()
{
    if (!m_wallet) return;
    const QString address{m_wallet->getAddressTableModel()->addRow(AddressTableModel::Receive, tr("Mining"), QString{}, OutputType::BECH32M)};
    if (address.isEmpty()) {
        QMessageBox::warning(this, tr("Mining"), tr("Could not generate a reward address. Check that the wallet can generate receiving addresses."));
        return;
    }
    m_address->setText(address);
}

void MiningPage::start()
{
    if (!m_client) return;
    try {
        m_client->node().startCpuMining(m_address->text().trimmed().toStdString(), m_threads->value());
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Mining"), QString::fromUtf8(e.what()));
    }
    refresh();
}

void MiningPage::updateThreadWarning()
{
    m_thread_warning->setText(tr("Warning: %1 mining threads exceed the %2 logical CPUs detected. This can reduce hashrate and slow down the node.")
                                  .arg(m_threads->value()).arg(m_logical_cpus));
    m_thread_warning->setVisible(m_client && m_threads->value() > m_logical_cpus);
}

void MiningPage::refresh()
{
    const auto info{m_client ? m_client->node().getCpuMiningStatus() : node::CpuMiningStatus{}};
    m_logical_cpus = info.logical_cpus;
    m_threads->setMaximum(info.max_threads);
    m_start->setEnabled(m_client && !info.running);
    m_stop->setEnabled(m_client && info.running && !info.stopping);
    m_address->setEnabled(!info.running);
    m_threads->setEnabled(!info.running);
    m_new_address->setEnabled(m_client && m_wallet && !info.running);
    if (info.running) {
        m_address->setText(QString::fromStdString(info.address));
        m_threads->setValue(info.threads);
    }
    updateThreadWarning();
    QString state{tr("Stopped")};
    if (info.state == "starting")
        state = tr("Starting");
    else if (info.state == "mining")
        state = tr("Mining / preparing RandomX");
    else if (info.state == "waiting")
        state = tr("Waiting for the node to catch up");
    else if (info.state == "stopping")
        state = tr("Stopping (waiting for current work)");
    else if (info.state == "error")
        state = tr("Error");
    m_status->setText(tr("State: %1\nHashrate: %2 H/s\nHashes: %3 | Accepted blocks: %4\nActive reward address: %5")
                          .arg(state)
                          .arg(info.hashes_per_second, 0, 'f', 2)
                          .arg(info.hashes)
                          .arg(info.blocks)
                          .arg(QString::fromStdString(info.address)) +
                      (info.error.empty() ? QString{} : QStringLiteral("\n") + QString::fromStdString(info.error)));
}
