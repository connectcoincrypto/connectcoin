// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_QTLOGFORWARDER_H
#define CONNECTCOIN_QT_QTLOGFORWARDER_H

#include <QString>
#include <QtCore/qlogging.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

//! Bounded forwarding of Qt diagnostics. The sink never runs under the queue
//! mutex, so GUI producers cannot wait for the core logger or a slow log file.
class QtLogForwarder
{
public:
    using Sink = std::function<void(QtMsgType, const QString&)>;
    static constexpr size_t MAX_QUEUED_MESSAGES{1024};
    static constexpr size_t MAX_QUEUED_BYTES{1024 * 1024};
    static constexpr qsizetype MAX_MESSAGE_CHARS{8192};

    struct Stats {
        size_t queued_messages{0};
        size_t queued_bytes{0};
        size_t dropped_messages{0};
    };

    explicit QtLogForwarder(Sink sink = {});
    ~QtLogForwarder();
    bool submit(QtMsgType type, const QString& message);
    Stats stats() const;
    //! Drain and join from an owned shutdown worker while the GUI remains alive.
    //! The destructor is a synchronous fallback only. Stop calls must be serialized.
    void stop();

private:
    struct Record {
        QtMsgType type{QtDebugMsg};
        QString message;
        size_t bytes{0};
    };
    void run();

    Sink m_sink;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Record> m_queue;
    size_t m_queued_bytes{0};
    size_t m_dropped_pending{0};
    size_t m_dropped_total{0};
    bool m_stopping{false};
    std::thread m_thread;
};

//! Install while the application owns the forwarder. Disconnect excludes any
//! in-flight message-handler enqueue before the owner drains/deletes the queue.
QtMessageHandler InstallQtLogForwarder(QtLogForwarder& forwarder);
void DisconnectQtLogForwarder(QtLogForwarder& forwarder);

#endif // CONNECTCOIN_QT_QTLOGFORWARDER_H
