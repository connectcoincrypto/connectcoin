// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/csvmodelwriter.h>
#include <qt/guiutil.h>

#include <QAbstractItemModel>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QTextStream>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

CSVModelWriter::CSVModelWriter(const QString& _filename, QObject* parent)
    : QObject(parent),
      filename(_filename)
{
}

void CSVModelWriter::setModel(const QAbstractItemModel *_model)
{
    this->model = _model;
}

void CSVModelWriter::addColumn(const QString &title, int column, int role)
{
    Column col;
    col.title = title;
    col.column = column;
    col.role = role;

    columns.append(col);
}

static void writeValue(QTextStream &f, const QString &value)
{
    QString escaped = value;
    escaped.replace('"', "\"\"");
    f << "\"" << escaped << "\"";
}

static void writeSep(QTextStream &f)
{
    f << ",";
}

static void writeNewline(QTextStream &f)
{
    f << "\n";
}

bool CSVModelWriter::write()
{
    GUIUtil::BackendOperationGuard operation;
    // Copy Qt model cells in bounded GUI turns, then encode/write on a worker.
    // An export must describe one model revision, not a mixture of rows moved
    // by live sorting or wallet notifications between those turns.
    const QPointer<const QAbstractItemModel> source{model};
    const bool has_model = model != nullptr;
    const auto export_columns = columns;
    const auto path = filename;
    // Only a small handoff chunk belongs to the GUI. The complete export is
    // owned by one worker throughout capture, revision retries and writing:
    // vector growth and destroying a discarded snapshot are both O(history).
    struct Batch {
        enum class Kind { Reset, Append, Write } kind;
        std::vector<QStringList> rows;
    };
    struct ExportQueue {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<Batch> batches;
        bool cancelled{false};
    };
    const auto queue = std::make_shared<ExportQueue>();
    std::future<bool> write_task;
    const auto cancel = [queue] {
        {
            std::lock_guard lock{queue->mutex};
            queue->cancelled = true;
        }
        queue->ready.notify_one();
    };
    // Covers exceptional exits too. Notify before the async future is joined;
    // an idle worker must never wait forever for another capture command.
    const auto stop_worker = qScopeGuard(cancel);
    try {
        write_task = std::async(std::launch::async, [path, export_columns, queue] {
            std::vector<QStringList> rows;
            try {
                for (;;) {
                    Batch batch;
                    {
                        std::unique_lock lock{queue->mutex};
                        queue->ready.wait(lock, [&] { return queue->cancelled || !queue->batches.empty(); });
                        if (queue->cancelled) return false;
                        batch = std::move(queue->batches.front());
                        queue->batches.pop_front();
                    }
                    switch (batch.kind) {
                    case Batch::Kind::Reset:
                        rows.clear();
                        rows.emplace_back();
                        for (const auto& column : export_columns) rows.back().append(column.title);
                        break;
                    case Batch::Kind::Append:
                        for (auto& row : batch.rows) rows.push_back(std::move(row));
                        break;
                    case Batch::Kind::Write: {
                        QFile file(path);
                        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
                        QTextStream out(&file);
                        for (const auto& row : rows) {
                            for (int column = 0; column < row.size(); ++column) {
                                if (column != 0) writeSep(out);
                                writeValue(out, row[column]);
                            }
                            writeNewline(out);
                        }
                        out.flush();
                        const bool written = out.status() == QTextStream::Ok && file.flush();
                        file.close();
                        return written && file.error() == QFile::NoError;
                    }
                    }
                }
            } catch (...) {
                return false;
            }
        });
    } catch (...) {
        return false;
    }
    int next_row{0}, num_rows{0}, restarts{0};
    bool changed{true};
    bool submitted{false};
    std::promise<bool> completion;
    auto completed = completion.get_future();
    QTimer timer;
    timer.setInterval(0);
    const auto invalidate = [&] { changed = true; };
    if (source) {
        connect(source, &QAbstractItemModel::dataChanged, &timer, invalidate);
        connect(source, &QAbstractItemModel::headerDataChanged, &timer, invalidate);
        connect(source, &QAbstractItemModel::rowsInserted, &timer, invalidate);
        connect(source, &QAbstractItemModel::rowsRemoved, &timer, invalidate);
        connect(source, &QAbstractItemModel::rowsMoved, &timer, invalidate);
        connect(source, &QAbstractItemModel::columnsInserted, &timer, invalidate);
        connect(source, &QAbstractItemModel::columnsRemoved, &timer, invalidate);
        connect(source, &QAbstractItemModel::columnsMoved, &timer, invalidate);
        connect(source, &QAbstractItemModel::layoutChanged, &timer, invalidate);
        connect(source, &QAbstractItemModel::modelReset, &timer, invalidate);
    }
    const auto finish = [&](bool success) {
        timer.stop();
        completion.set_value(success);
    };
    const auto enqueue = [&](Batch batch) {
        {
            std::lock_guard lock{queue->mutex};
            queue->batches.push_back(std::move(batch));
        }
        queue->ready.notify_one();
    };
    const auto abort = [&] {
        cancel();
        submitted = true;
        timer.setInterval(10);
    };
    connect(&timer, &QTimer::timeout, &timer, [&] {
        try {
            if (write_task.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
                finish(write_task.get());
                return;
            }
            if (submitted) return;
            if (has_model && !source) {
                abort();
                return;
            }
            {
                std::lock_guard lock{queue->mutex};
                // Never enqueue an entire second snapshot if allocation/IO is
                // slow. The worker holds no GUI/model pointer, and holds this
                // mutex only while taking one bounded handoff command.
                if (queue->batches.size() >= 2) {
                    timer.setInterval(10);
                    return;
                }
            }
            timer.setInterval(0);
            if (changed) {
                // Allow three retries, but never chase a constantly changing
                // table forever. No destination is opened until capture ends.
                if (restarts++ > 3) {
                    abort();
                    return;
                }
                num_rows = source ? source->rowCount() : 0;
                next_row = 0;
                changed = false;
                enqueue({Batch::Kind::Reset, {}});
                return;
            }
            if (next_row == num_rows) {
                enqueue({Batch::Kind::Write, {}});
                submitted = true;
                timer.setInterval(10);
                return;
            }
            constexpr int MAX_ROWS_PER_TURN{128};
            Batch batch{Batch::Kind::Append, {}};
            batch.rows.reserve(MAX_ROWS_PER_TURN);
            const int end_row = std::min(num_rows, next_row + MAX_ROWS_PER_TURN);
            for (; next_row < end_row; ++next_row) {
                batch.rows.emplace_back();
                for (const auto& column : export_columns) {
                    batch.rows.back().append(source->index(next_row, column.column).data(column.role).toString());
                }
            }
            // A reentrant dataChanged invalidates only this bounded GUI
            // chunk; the next reset discards all earlier rows on the worker.
            if (!changed) enqueue(std::move(batch));
        } catch (...) {
            abort();
        }
    });
    timer.start();
    // This explicit action keeps the producer/timer and IO future alive until
    // completion, while unload/shutdown are deferred by the operation guard.
    return GUIUtil::WaitForBackendTask(std::move(completed));
}
