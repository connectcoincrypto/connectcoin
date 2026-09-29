// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit-license.php.

#include <qt/qtlogforwarder.h>

#include <logging.h>
#include <util/threadnames.h>

#include <cassert>
#include <utility>

namespace {
std::mutex g_forwarder_mutex;
QtLogForwarder* g_forwarder{nullptr};

void WriteQtDiagnostic(QtMsgType type, const QString& message)
{
    if (type == QtDebugMsg) {
        LogDebug(BCLog::QT, "GUI: %s\n", message.toStdString());
    } else {
        LogInfo("GUI: %s", message.toStdString());
    }
}

void ForwardQtDiagnostic(QtMsgType type, const QMessageLogContext&, const QString& message)
{
    // Qt aborts after a fatal message returns, so this emergency path cannot
    // depend on an asynchronous drain. Ordinary diagnostics are always queued.
    if (type == QtFatalMsg) {
        WriteQtDiagnostic(type, message);
        return;
    }
    std::lock_guard lock{g_forwarder_mutex};
    if (g_forwarder) g_forwarder->submit(type, message);
}
} // namespace

QtLogForwarder::QtLogForwarder(Sink sink)
    : m_sink(sink ? std::move(sink) : WriteQtDiagnostic), m_thread([this] { run(); })
{
}

QtLogForwarder::~QtLogForwarder()
{
    stop();
}

bool QtLogForwarder::submit(QtMsgType type, const QString& message)
{
    QString bounded = message;
    if (bounded.size() > MAX_MESSAGE_CHARS) {
        bounded = bounded.left(MAX_MESSAGE_CHARS - 16) + QStringLiteral("… [truncated]");
    }
    const size_t bytes = static_cast<size_t>(bounded.size()) * sizeof(QChar);
    std::lock_guard lock{m_mutex};
    if (m_stopping) return false;
    if (m_queue.size() >= MAX_QUEUED_MESSAGES || m_queued_bytes + bytes > MAX_QUEUED_BYTES) {
        ++m_dropped_pending;
        ++m_dropped_total;
        m_wake.notify_one();
        return false;
    }
    m_queue.push_back({type, std::move(bounded), bytes});
    m_queued_bytes += bytes;
    m_wake.notify_one();
    return true;
}

QtLogForwarder::Stats QtLogForwarder::stats() const
{
    std::lock_guard lock{m_mutex};
    return {m_queue.size(), m_queued_bytes, m_dropped_total};
}

void QtLogForwarder::stop()
{
    DisconnectQtLogForwarder(*this);
    {
        std::lock_guard lock{m_mutex};
        m_stopping = true;
    }
    m_wake.notify_one();
    if (m_thread.joinable()) {
        assert(m_thread.get_id() != std::this_thread::get_id());
        m_thread.join();
    }
}

void QtLogForwarder::run()
{
    util::ThreadRename("qt-log");
    for (;;) {
        Record record;
        {
            std::unique_lock lock{m_mutex};
            m_wake.wait(lock, [this] { return m_stopping || !m_queue.empty() || m_dropped_pending != 0; });
            if (!m_queue.empty()) {
                record = std::move(m_queue.front());
                m_queue.pop_front();
                m_queued_bytes -= record.bytes;
            } else if (m_dropped_pending != 0) {
                record = {QtWarningMsg, QStringLiteral("Qt diagnostic queue saturated; discarded %1 messages.").arg(qulonglong(m_dropped_pending)), 0};
                m_dropped_pending = 0;
            } else {
                assert(m_stopping);
                return;
            }
        }
        m_sink(record.type, record.message);
    }
}

QtMessageHandler InstallQtLogForwarder(QtLogForwarder& forwarder)
{
    {
        std::lock_guard lock{g_forwarder_mutex};
        assert(!g_forwarder);
        g_forwarder = &forwarder;
    }
    return qInstallMessageHandler(ForwardQtDiagnostic);
}

void DisconnectQtLogForwarder(QtLogForwarder& forwarder)
{
    std::lock_guard lock{g_forwarder_mutex};
    if (g_forwarder == &forwarder) g_forwarder = nullptr;
}
