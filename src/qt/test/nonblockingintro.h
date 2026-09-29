// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGINTRO_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGINTRO_H

#include <qt/freespacechecker.h>
#include <qt/intro.h>

#include <QDialogButtonBox>
#include <QFile>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <future>

inline void CheckNonblockingIntro()
{
    // Editing/reopening the dialog can enqueue an empty path. Do not pass it
    // to fs::space: the MinGW implementation can terminate instead of throwing.
    FreespaceChecker checker;
    QSignalSpy checked(&checker, &FreespaceChecker::reply);
    checker.check({});
    QCOMPARE(checked.count(), 1);
    QCOMPARE(checked.first().at(0).toString(), QString{});
    QCOMPARE(checked.first().at(1).toInt(), int(FreespaceChecker::ST_ERROR));
    QCOMPARE(checked.first().at(3).toULongLong(), qulonglong{0});

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Intro intro{nullptr, 0, 0, directory.path()};
    intro.show();
    auto* worker = intro.findChild<QThread*>();
    QVERIFY(worker);

    // Hold the checker thread without relying on a slow disk/network mount.
    // The bounded wait makes a GUI-join regression fail instead of hanging.
    std::promise<void> entered;
    auto ready = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    std::atomic<bool> timed_out{false};
    auto* blocker = new QObject;
    blocker->moveToThread(worker);
    QObject::connect(worker, &QThread::finished, blocker, &QObject::deleteLater);
    QMetaObject::invokeMethod(blocker, [&] {
        entered.set_value();
        timed_out = released.wait_for(std::chrono::seconds{3}) == std::future_status::timeout;
    }, Qt::QueuedConnection);
    ready.wait();

    QSignalSpy finished(&intro, &QDialog::finished);
    int ticks{0};
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        if (++ticks == 5) release.set_value();
    });
    heartbeat.start(10);
    intro.reject();
    const bool returned_before_completion = finished.isEmpty();
    // Even direct rejection must await the worker before finishing the dialog.
    QTRY_COMPARE(finished.count(), 1);
    heartbeat.stop();
    QVERIFY(returned_before_completion);
    QVERIFY(!timed_out.load());
    QVERIFY(ticks >= 5);
    QCOMPARE(finished.first().first().toInt(), int(QDialog::Rejected));

    // A failed directory creation may reopen the same dialog. Its new worker
    // must discard old checks, coalesce edits and validate the latest path.
    intro.show();
    auto* buttons = intro.findChild<QDialogButtonBox*>("buttonBox");
    QVERIFY(buttons);
    QSignalSpy requests(&intro, &Intro::requestCheck);
    const QString file_path = directory.filePath("not-a-directory");
    QFile file(file_path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    intro.setDataDirectory(directory.path());
    intro.setDataDirectory(file_path);
    intro.setDataDirectory(directory.filePath("new-directory"));
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QTRY_VERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QVERIFY(!requests.isEmpty());
    QCOMPARE(requests.last().first().toString(), directory.filePath("new-directory"));
    // Only the first request plus its latest replacement can be submitted.
    QVERIFY(requests.count() <= 2);
    intro.accept();
    QTRY_COMPARE(finished.count(), 2);
    QCOMPARE(finished.last().first().toInt(), int(QDialog::Accepted));
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGINTRO_H
