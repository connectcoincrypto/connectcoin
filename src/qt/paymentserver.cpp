// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/paymentserver.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>

#include <chainparams.h>
#include <common/args.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/interface_ui.h>
#include <policy/policy.h>
#include <wallet/wallet.h>

#include <cstdlib>
#include <memory>

#include <QApplication>
#include <QByteArray>
#include <QDataStream>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileOpenEvent>
#include <QHash>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QUrlQuery>

const int CONNECTCOIN_IPC_CONNECT_TIMEOUT = 1000; // milliseconds
static constexpr int IPC_READ_TIMEOUT{10000};
static constexpr qint64 MAX_IPC_REQUEST_BYTES{1024 * 1024};
static constexpr unsigned int MAX_IPC_CONNECTIONS{32};
const QString CONNECTCOIN_IPC_PREFIX("connectcoin:");

//
// Create a name that is unique for:
//  testnet / non-testnet
//  data directory
//
static QString ipcServerName()
{
    QString name("ConnectCoinQt");

    // Append a simple hash of the datadir
    // Note that gArgs.GetDataDirNet() returns a different path
    // for -testnet versus main net
    QString ddir(GUIUtil::PathToQString(gArgs.GetDataDirNet()));
    name.append(QString::number(qHash(ddir)));

    return name;
}

//
// We store payment URIs and requests received before
// the main GUI window is up and ready to ask the user
// to send payment.

static QSet<QString> savedPaymentRequests;

//
// Sending to the server is done synchronously, at startup.
// If the server isn't already running, startup continues,
// and the items in savedPaymentRequest will be handled
// when uiReady() is called.
//
// Warning: ipcSendCommandLine() is called early in init,
// so don't use "Q_EMIT message()", but "QMessageBox::"!
//
void PaymentServer::ipcParseCommandLine(int argc, char* argv[])
{
    for (int i = 1; i < argc; i++)
    {
        QString arg(argv[i]);
        if (arg.startsWith("-")) continue;

        if (arg.startsWith(CONNECTCOIN_IPC_PREFIX, Qt::CaseInsensitive)) // connectcoin: URI
        {
            savedPaymentRequests.insert(arg);
        }
    }
}

//
// Sending to the server is done synchronously, at startup.
// If the server isn't already running, startup continues,
// and the items in savedPaymentRequest will be handled
// when uiReady() is called.
//
bool PaymentServer::ipcSendCommandLine()
{
    if (savedPaymentRequests.isEmpty()) return false;
    return GUIUtil::WaitForBackendTask(std::async(std::launch::async,
        [requests = savedPaymentRequests] {
            // Capture initializers execute in the caller, not the worker.
            // GetDataDirNet() takes cs_args and may inspect the filesystem.
            const QString server_name = ipcServerName();
            bool sent{false};
            for (const QString& request : requests) {
                QLocalSocket socket;
                socket.connectToServer(server_name, QIODevice::WriteOnly);
                if (!socket.waitForConnected(CONNECTCOIN_IPC_CONNECT_TIMEOUT)) return false;
                QByteArray block;
                QDataStream out(&block, QIODevice::WriteOnly);
                out.setVersion(QDataStream::Qt_4_0);
                out << request;
                if (block.size() > MAX_IPC_REQUEST_BYTES + static_cast<qint64>(sizeof(quint32))) return false;
                if (socket.write(block) != block.size()) return false;
                socket.flush();
                QElapsedTimer deadline;
                deadline.start();
                while (socket.bytesToWrite() > 0) {
                    const int remaining = CONNECTCOIN_IPC_CONNECT_TIMEOUT - static_cast<int>(deadline.elapsed());
                    if (remaining <= 0 || !socket.waitForBytesWritten(remaining)) return false;
                }
                socket.disconnectFromServer();
                sent = true;
            }
            return sent;
        }));
}

PaymentServer::PaymentServer(QObject* parent)
    : QObject(parent)
{
    // Install global event filter to catch QFileOpenEvents
    // on Mac: sent when you click connectcoin: links
    // other OSes: helpful when dealing with payment request files
    if (parent)
        parent->installEventFilter(this);
}

PaymentServer::~PaymentServer() = default;

void PaymentServer::startLocalServer()
{
    if (uriServer || m_starting) return;
    GUIUtil::BackendOperationGuard operation;
    const QPointer<PaymentServer> guard{this};
    m_starting = true;
    QString name;
    try {
        name = GUIUtil::WaitForBackendTask(std::async(std::launch::async, [] {
            const QString name = ipcServerName();
            // This static cleanup touches a stale socket path on Unix. No
            // GUI-owned QLocalServer or QLocalSocket is used by the worker.
            QLocalServer::removeServer(name);
            return name;
        }));
    } catch (...) {
        if (guard) m_starting = false;
        throw;
    }
    if (!guard) return;
    m_starting = false;
    uriServer = new QLocalServer(this);
    if (!uriServer->listen(name)) {
        // Startup precedes the main window's message-signal connection.
        QMessageBox::critical(nullptr, tr("Payment request error"),
            tr("Cannot start connectcoin: click-to-pay handler"));
    } else {
        connect(uriServer, &QLocalServer::newConnection, this, &PaymentServer::handleURIConnection);
    }
}

//
// OSX-specific way of handling connectcoin: URIs
//
bool PaymentServer::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::FileOpen) {
        QFileOpenEvent *fileEvent = static_cast<QFileOpenEvent*>(event);
        if (!fileEvent->file().isEmpty())
            handleURIOrFile(fileEvent->file());
        else if (!fileEvent->url().isEmpty())
            handleURIOrFile(fileEvent->url().toString());

        return true;
    }

    return QObject::eventFilter(object, event);
}

void PaymentServer::uiReady()
{
    saveURIs = false;
    for (const QString& s : savedPaymentRequests)
    {
        handleURIOrFile(s);
    }
    savedPaymentRequests.clear();
}

void PaymentServer::handleURIOrFile(const QString& s)
{
    GUIUtil::BackendOperationGuard operation;
    if (saveURIs)
    {
        savedPaymentRequests.insert(s);
        return;
    }

    if (s.startsWith("connectcoin://", Qt::CaseInsensitive))
    {
        Q_EMIT message(tr("URI handling"), tr("'connectcoin://' is not a valid URI. Use 'connectcoin:' instead."),
            CClientUIInterface::MSG_ERROR);
        return;
    }
    else if (s.startsWith(CONNECTCOIN_IPC_PREFIX, Qt::CaseInsensitive)) // connectcoin: URI
    {
        QUrlQuery uri((QUrl(s)));
        // normal URI
        {
            SendCoinsRecipient recipient;
            if (GUIUtil::parseBitcoinURI(s, &recipient))
            {
                std::string error_msg;
                const CTxDestination dest = DecodeDestination(recipient.address.toStdString(), error_msg);

                if (!IsValidDestination(dest)) {
                    if (uri.hasQueryItem("r")) {  // payment request
                        Q_EMIT message(tr("URI handling"),
                            tr("Cannot process payment request because BIP70 is not supported.\n"
                               "Due to widespread security flaws in BIP70 it's strongly recommended that any merchant instructions to switch wallets be ignored.\n"
                               "If you are receiving this error you should request the merchant provide a BIP21 compatible URI."),
                            CClientUIInterface::ICON_WARNING);
                    }
                    Q_EMIT message(tr("URI handling"), QString::fromStdString(error_msg),
                        CClientUIInterface::MSG_ERROR);
                }
                else
                    Q_EMIT receivedPaymentRequest(recipient);
            }
            else
                Q_EMIT message(tr("URI handling"),
                    tr("URI cannot be parsed! This can be caused by an invalid ConnectCoin address or malformed URI parameters."),
                    CClientUIInterface::ICON_WARNING);

            return;
        }
    }

    // Payment request files are unsupported, so probing their paths (possibly
    // on a slow network share) cannot change the result.
    if (!s.isEmpty())
    {
        Q_EMIT message(tr("Payment request file handling"),
            tr("Cannot process payment request because BIP70 is not supported.\n"
               "Due to widespread security flaws in BIP70 it's strongly recommended that any merchant instructions to switch wallets be ignored.\n"
               "If you are receiving this error you should request the merchant provide a BIP21 compatible URI."),
            CClientUIInterface::ICON_WARNING);
    }
}

void PaymentServer::handleURIConnection()
{
    GUIUtil::BackendOperationGuard operation;
    while (uriServer->hasPendingConnections()) {
        QLocalSocket* socket = uriServer->nextPendingConnection();
        if (!socket) break;
        if (m_uri_connections >= MAX_IPC_CONNECTIONS) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        ++m_uri_connections;
        connect(socket, &QObject::destroyed, this, [this] { --m_uri_connections; });
        socket->setReadBufferSize(MAX_IPC_REQUEST_BYTES + static_cast<qint64>(sizeof(quint32)));
        auto* deadline = new QTimer(socket);
        deadline->setSingleShot(true);
        connect(deadline, &QTimer::timeout, socket, [socket] {
            socket->deleteLater();
            socket->abort();
        });
        auto processed = std::make_shared<bool>(false);
        const auto receive = [this, socket, deadline, processed] {
            if (*processed || socket->bytesAvailable() < static_cast<qint64>(sizeof(quint32))) return;
            QDataStream header(socket->peek(sizeof(quint32)));
            header.setVersion(QDataStream::Qt_4_0);
            quint32 payload_bytes{0};
            header >> payload_bytes;
            // Qt_4_0 serializes QString as a byte length followed by UTF-16.
            // Reject oversized, odd-length, and null/extended-length markers
            // before allowing QDataStream to allocate the payload.
            if (payload_bytes > MAX_IPC_REQUEST_BYTES || (payload_bytes & 1U) != 0 ||
                socket->bytesAvailable() > MAX_IPC_REQUEST_BYTES + static_cast<qint64>(sizeof(quint32))) {
                *processed = true;
                deadline->stop();
                socket->abort();
                socket->deleteLater();
                return;
            }
            const qint64 message_bytes = static_cast<qint64>(sizeof(quint32)) + payload_bytes;
            if (socket->bytesAvailable() < message_bytes) return;
            QDataStream input(socket->read(message_bytes));
            input.setVersion(QDataStream::Qt_4_0);
            QString message;
            input >> message;
            *processed = true;
            deadline->stop();
            socket->abort();
            socket->deleteLater();
            if (input.status() == QDataStream::Ok && !message.isEmpty()) handleURIOrFile(message);
        };
        connect(socket, &QLocalSocket::readyRead, socket, receive);
        connect(socket, &QLocalSocket::disconnected, socket, [socket, receive] {
            // A peer can close immediately after its final write.
            socket->deleteLater();
            receive();
        });
        deadline->start(IPC_READ_TIMEOUT);
        if (socket->state() == QLocalSocket::UnconnectedState) socket->deleteLater();
        // readyRead may already have fired before nextPendingConnection().
        receive();
    }
}

void PaymentServer::setOptionsModel(OptionsModel *_optionsModel)
{
    this->optionsModel = _optionsModel;
}
