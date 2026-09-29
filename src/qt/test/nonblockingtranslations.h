// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGTRANSLATIONS_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGTRANSLATIONS_H

#include <qt/guitranslations.h>
#include <qt/guiutil.h>

#include <QCoreApplication>
#include <QDataStream>
#include <QFile>
#include <QScopeGuard>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QTranslator>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

// Exercise the production queue, without adding a public loader callback or
// relying on a slow filesystem, a large catalog, or external test utilities.
struct GuiTranslationsTestAccess {
    static std::future<void> submit(GuiTranslations& translations, std::function<void()> task)
    {
        return translations.submit(std::move(task));
    }
};

namespace translation_test {
struct BarrierResult {
    int ticks{0};
    bool timed_out{false};
    bool worker_on_gui{false};
};

inline BarrierResult WithBlockedWorker(GuiTranslations& translations,
                                      const std::function<void()>& operation,
                                      const std::function<void()>& during_wait = {})
{
    BarrierResult result;
    std::promise<void> entered;
    auto ready = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    auto blocker = GuiTranslationsTestAccess::submit(translations, [&] {
        result.worker_on_gui = QThread::currentThread() == qApp->thread();
        entered.set_value();
        // A synchronous-GUI regression fails in bounded time, not a hung test.
        result.timed_out = released.wait_for(std::chrono::seconds{3}) != std::future_status::ready;
    });
    ready.wait();
    QTimer heartbeat;
    bool release_sent{false};
    const auto cleanup = qScopeGuard([&] {
        heartbeat.stop();
        if (!release_sent) release.set_value();
        blocker.get(); // The release above also makes exceptional exit safe.
    });
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
        ++result.ticks;
        if (result.ticks == 1 && during_wait) during_wait();
        if (result.ticks == 5) {
            release_sent = true;
            release.set_value();
        }
    });
    heartbeat.start(10);
    operation();
    heartbeat.stop();
    if (!release_sent) {
        release_sent = true;
        release.set_value();
    }
    blocker.wait(); // Synchronize the result fields before copying them out.
    return result;
}

inline QByteArray DependencyCatalog(const QString& dependency)
{
    // A dependency-only QM catalog: the standard header followed by the
    // Dependencies block (0x96), containing a QDataStream QString. The child
    // is a real compiled application resource, not a second custom QM parser.
    QByteArray dependencies;
    QDataStream payload(&dependencies, QIODevice::WriteOnly);
    payload << dependency;
    QByteArray result = QByteArray::fromHex("3cb86418caef9c95cd211cbf60a1bddd");
    QByteArray block;
    QDataStream stream(&block, QIODevice::WriteOnly);
    stream << quint8{0x96} << quint32(dependencies.size());
    stream.writeRawData(dependencies.constData(), dependencies.size());
    return result + block;
}
} // namespace translation_test

inline void CheckNonblockingTranslations()
{
    const auto overview = [] { return QCoreApplication::translate("BitcoinGUI", "&Overview"); };
    const QString previous_overview = overview();
    QTranslator portuguese, arabic;
    QVERIFY(portuguese.load("pt_BR", ":/translations/"));
    QVERIFY(arabic.load("ar", ":/translations/"));
    const auto pt_overview = portuguese.translate("BitcoinGUI", "&Overview");
    const auto ar_overview = arabic.translate("BitcoinGUI", "&Overview");
    QVERIFY(!pt_overview.isNull());
    QVERIFY(!ar_overview.isNull());
    QVERIFY(pt_overview != ar_overview);

    GuiTranslations translations;
    const auto initial = translation_test::WithBlockedWorker(translations, [&] {
        translations.load({{"pt_BR", ":/translations/"}});
    });
    QVERIFY(initial.ticks >= 5);
    QVERIFY(!initial.timed_out);
    QVERIFY(!initial.worker_on_gui);
    QCOMPARE(overview(), pt_overview);

    bool old_catalog_read{false}, reload_rejected{false}, stop_rejected{false};
    const auto reload = translation_test::WithBlockedWorker(translations, [&] {
        translations.load({{"pt_BR", ":/translations/"}, {"ar", ":/translations/"}});
    }, [&] {
        old_catalog_read = overview() == pt_overview;
        try {
            translations.load({});
        } catch (const std::logic_error&) {
            reload_rejected = true;
        }
        try {
            translations.stop();
        } catch (const std::logic_error&) {
            stop_rejected = true;
        }
    });
    QVERIFY(reload.ticks >= 5);
    QVERIFY(!reload.timed_out);
    QVERIFY(old_catalog_read);
    QVERIFY(reload_rejected);
    QVERIFY(stop_rejected);
    QCOMPARE(overview(), ar_overview); // Last successfully loaded source wins.
    translations.load({{"ar", ":/translations/"}, {"pt_BR", ":/translations/"}});
    QCOMPARE(overview(), pt_overview);

    // Catalog publication/reclamation also serves translations from backend
    // threads. Exercise the Qt translation lock while old catalogs are retired.
    // The brief uninstalled interval may legitimately return the source text.
    {
        std::atomic<bool> stop_reader{false}, unexpected{false};
        std::atomic<int> reads{0};
        std::promise<void> entered;
        auto ready = entered.get_future();
        auto reader = std::async(std::launch::async, [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            entered.set_value();
            while (!stop_reader && std::chrono::steady_clock::now() < deadline && reads < 100000) {
                const auto translated = overview();
                if (translated != pt_overview && translated != ar_overview &&
                    translated != previous_overview && translated != QString{"&Overview"}) {
                    unexpected = true;
                }
                ++reads;
                std::this_thread::yield();
            }
        });
        const auto join = qScopeGuard([&] {
            stop_reader = true;
            reader.get();
        });
        ready.wait();
        for (int iteration{0}; iteration < 8; ++iteration) {
            translations.load({{iteration % 2 ? "pt_BR" : "ar", ":/translations/"}});
        }
        stop_reader = true;
        reader.wait();
        QVERIFY(reads > 0);
        QVERIFY(!unexpected);
    }

    // Compare the public translation entry point, including plural selection
    // and %n substitution, against Qt's ordinary translator installation.
    for (const auto& locale : {QString{"pt_BR"}, QString{"ar"}}) {
        QTranslator reference;
        QVERIFY(reference.load(locale, ":/translations/"));
        QStringList expected;
        QVERIFY(QCoreApplication::installTranslator(&reference));
        for (const int count : {0, 1, 2, 3, 11, 100}) {
            expected.append(QCoreApplication::translate("QObject", "%n second(s)", nullptr, count));
        }
        QCoreApplication::removeTranslator(&reference);
        translations.loadLocale(locale);
        qsizetype index{0};
        for (const int count : {0, 1, 2, 3, 11, 100}) {
            QCOMPARE(QCoreApplication::translate("QObject", "%n second(s)", nullptr, count), expected[index++]);
        }
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QFile::copy(":/translations/pt_BR", directory.filePath("dependency.qm")));
    QFile master(directory.filePath("master.qm"));
    QVERIFY(master.open(QIODevice::WriteOnly));
    const auto dependency_catalog = translation_test::DependencyCatalog("dependency");
    QCOMPARE(master.write(dependency_catalog), dependency_catalog.size());
    master.close();
    // The master has no messages of its own. isEmpty() must account for loaded
    // dependencies, whose relative path is based on Source.directory.
    translations.load({{"master", directory.path()}});
    QCOMPARE(overview(), pt_overview);
    translations.load({{"dependency_NONEXISTENT", directory.path()}});
    QCOMPARE(overview(), pt_overview); // Preserve Qt's filename fallback.
    translations.load({{"not-present", directory.path()}});
    QCOMPARE(overview(), previous_overview); // Never retain a stale old catalog.
    translations.load({});
    QCOMPARE(overview(), previous_overview);
    translations.load({{"master", directory.path()}});

    bool removed_during_stop{false};
    const auto stopped = translation_test::WithBlockedWorker(translations, [&] {
        translations.stop();
    }, [&] {
        removed_during_stop = overview() == previous_overview;
    });
    QVERIFY(stopped.ticks >= 5);
    QVERIFY(!stopped.timed_out);
    QVERIFY(!stopped.worker_on_gui);
    QVERIFY(removed_during_stop);
    QCOMPARE(overview(), previous_overview);
    translations.stop(); // Idempotent, including after dependency teardown.
    QVERIFY_THROWS_EXCEPTION(std::logic_error, translations.load({}));
    QVERIFY_THROWS_EXCEPTION(std::logic_error, GuiTranslationsTestAccess::submit(translations, [] {}));
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGTRANSLATIONS_H
