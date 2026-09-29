// Copyright (c) The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGCSVCAPTURE_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGCSVCAPTURE_H

#include <qt/csvmodelwriter.h>

#include <functional>
#include <memory>
#include <stdexcept>

#include <QAbstractTableModel>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

inline void CheckCSVRestartAndLifetime()
{
    class CaptureModel final : public QAbstractTableModel {
    public:
        int generation{0}, reads{0};
        bool unstable{false}, unrelated_changes{false}, throw_on_read{false}, gui_only{true};
        std::function<void()> during_capture;
        int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 4096; }
        int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 1; }
        QVariant data(const QModelIndex& item, int role) const override
        {
            if (!item.isValid() || role != Qt::EditRole) return {};
            auto& self = *const_cast<CaptureModel*>(this);
            ++self.reads;
            self.gui_only &= QThread::currentThread() == thread();
            if (unrelated_changes && item.row() % 128 == 0) {
                Q_EMIT self.dataChanged(index(0, 0), index(rowCount() - 1, 0), {Qt::ToolTipRole});
            }
            if (item.row() == 3071 && (generation < 2 || unstable)) {
                ++self.generation;
                Q_EMIT self.dataChanged(index(0, 0), index(rowCount() - 1, 0));
            }
            if (item.row() == 2048) {
                if (throw_on_read) throw std::runtime_error("CSV fixture model failure");
                if (during_capture) during_capture();
            }
            return QString("%1:%2").arg(generation).arg(item.row());
        }
    };
    auto model = std::make_unique<CaptureModel>();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("worker-capture.csv");
    CSVModelWriter writer{path};
    writer.setModel(model.get());
    writer.addColumn("value", 0);
    QVERIFY(writer.write());
    QCOMPARE(model->generation, 2);
    QVERIFY(model->gui_only);
    QVERIFY(model->reads > 2 * 3071);
    QFile file{path};
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto contents = file.readAll();
    file.close();
    QByteArray expected{"\"value\"\n"};
    for (int row{0}; row < model->rowCount(); ++row) expected += QString("\"2:%1\"\n").arg(row).toUtf8();
    QCOMPARE(contents, expected);

    // Background hydration of roles not exported must not exhaust the retry
    // budget, even when every capture chunk triggers another notification.
    model->unrelated_changes = true;
    const int reads_before_unrelated_changes = model->reads;
    QVERIFY(writer.write());
    QCOMPARE(model->reads - reads_before_unrelated_changes, model->rowCount());
    model->unrelated_changes = false;
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(file.readAll(), contents);
    file.close();

    // Each failed revision has already built thousands of rows on the worker.
    // Retry exhaustion and exceptions must reclaim them there, never open the
    // destination, and terminate the worker instead of leaving it waiting.
    model->unstable = true;
    const int previous_reads = model->reads;
    QVERIFY(!writer.write());
    QVERIFY(model->reads - previous_reads <= 4 * 3072);
    model->unstable = false;
    model->throw_on_read = true;
    QVERIFY(!writer.write());
    model->throw_on_read = false;
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(file.readAll(), contents);
    file.close();

    // A view/model may disappear while the explicit export wait processes GUI
    // events. The worker owns only captured values and must be safely stopped.
    QObject deletion_context;
    model->during_capture = [&] {
        QTimer::singleShot(0, &deletion_context, [&] { model.reset(); });
    };
    QVERIFY(!writer.write());
    QVERIFY(!model);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(file.readAll(), contents);

    // An empty or intentionally absent model still exports its header.
    const auto empty_path = directory.filePath("header-only.csv");
    CSVModelWriter empty{empty_path};
    empty.addColumn("header", 0);
    QVERIFY(empty.write());
    QFile empty_file{empty_path};
    QVERIFY(empty_file.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(empty_file.readAll(), QByteArray{"\"header\"\n"});
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGCSVCAPTURE_H
