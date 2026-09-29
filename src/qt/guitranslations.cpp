// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit-license.php.

#include <qt/guitranslations.h>

#include <qt/guiutil.h>

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QScopeGuard>
#include <QThread>
#include <QTranslator>

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace {
struct Catalog {
    std::vector<std::unique_ptr<QTranslator>> translators;
    bool empty{true};

    QString translate(const char* context, const char* text, const char* comment, int n) const
    {
        for (auto it = translators.rbegin(); it != translators.rend(); ++it) {
            const auto translated = (*it)->translate(context, text, comment, n);
            if (!translated.isNull()) return translated;
        }
        return {};
    }
};

class CatalogWorker : public QObject
{
public:
    std::vector<std::unique_ptr<Catalog>> catalogs;

    Catalog* load(const GuiTranslations::Sources& sources)
    {
        assert(QThread::currentThread() == thread());
        auto catalog = std::make_unique<Catalog>();
        for (const auto& source : sources) {
            auto translator = std::make_unique<QTranslator>();
            if (!translator->load(source.filename, source.directory)) continue;
            catalog->empty &= translator->isEmpty();
            catalog->translators.push_back(std::move(translator));
        }
        const auto result = catalog.get();
        catalogs.push_back(std::move(catalog));
        return result;
    }
};

class TranslationFacade : public QTranslator
{
public:
    // Only replaced while uninstalled. QCoreApplication::removeTranslator()
    // takes Qt's translation write lock, draining all in-flight translate()
    // calls before the old catalog can be reclaimed by the worker.
    const Catalog* catalog{nullptr};

    bool isEmpty() const override { return !catalog || catalog->empty; }
    QString translate(const char* context, const char* text, const char* comment, int n) const override
    {
        return catalog ? catalog->translate(context, text, comment, n) : QString{};
    }
};
} // namespace

struct GuiTranslations::Impl {
    QThread thread;
    CatalogWorker* worker{new CatalogWorker};
    TranslationFacade facade;
    bool busy{false};
    bool stopped{false};
    bool joined{false};

    Impl()
    {
        thread.setObjectName("qt-translations");
        worker->moveToThread(&thread);
        // Qt processes deferred deletion on the worker when finished fires,
        // including the catalogs' unparented, private dependency translators.
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();
    }

    void unregister()
    {
        QCoreApplication::removeTranslator(&facade);
        facade.catalog = nullptr;
    }
};

GuiTranslations::GuiTranslations() : m_impl(std::make_unique<Impl>()) {}

GuiTranslations::~GuiTranslations()
{
    // Startup/exception fallback: never enter an event loop from destruction.
    // The application's normal shutdown explicitly calls responsive stop().
    if (!m_impl->joined) {
        m_impl->unregister();
        m_impl->thread.quit();
        m_impl->thread.wait();
    }
}

std::future<void> GuiTranslations::submit(std::function<void()> task)
{
    if (m_impl->stopped) throw std::logic_error("Translation worker already stopped");
    auto packaged = std::make_shared<std::packaged_task<void()>>(std::move(task));
    auto result = packaged->get_future();
    const bool invoked = QMetaObject::invokeMethod(m_impl->worker, [packaged] { (*packaged)(); }, Qt::QueuedConnection);
    if (!invoked) throw std::runtime_error("Unable to queue translation loading");
    return result;
}

void GuiTranslations::loadSources(std::function<Sources()> sources)
{
    if (m_impl->busy) throw std::logic_error("Translation operation already in progress");
    m_impl->busy = true;
    const auto idle = qScopeGuard([&] { m_impl->busy = false; });
    // Keep the old immutable translation active throughout the responsive wait.
    auto result = std::make_shared<const Catalog*>(nullptr);
    GUIUtil::WaitForBackendTask(submit([worker = m_impl->worker, sources = std::move(sources), result] {
        *result = worker->load(sources());
    }));
    m_impl->unregister();
    m_impl->facade.catalog = *result;
    if (!m_impl->facade.isEmpty()) QCoreApplication::installTranslator(&m_impl->facade);
    // The previous catalog is no longer reachable by Qt readers. Reclaim it on
    // its owner, not during the GUI model/language-change notification.
    (void)submit([worker = m_impl->worker, keep = *result] {
        std::erase_if(worker->catalogs, [keep](const auto& catalog) { return catalog.get() != keep; });
    });
}

void GuiTranslations::load(const Sources& sources)
{
    loadSources([sources] { return sources; });
}

void GuiTranslations::loadLocale(const QString& locale)
{
    loadSources([locale] {
        const QString base = locale.section('_', 0, 0);
        const QString qt_path = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
        return Sources{{"qt_" + base, qt_path}, {"qt_" + locale, qt_path},
                       {base, ":/translations/"}, {locale, ":/translations/"}};
    });
}

void GuiTranslations::stop()
{
    if (m_impl->joined) return;
    if (m_impl->busy) throw std::logic_error("Translation operation already in progress");
    m_impl->busy = true;
    const auto idle = qScopeGuard([&] { m_impl->busy = false; });
    m_impl->stopped = true;
    m_impl->unregister();
    GUIUtil::WaitForBackendTask(std::async(std::launch::async, [impl = m_impl.get()] {
        impl->thread.quit();
        impl->thread.wait();
    }));
    // Track completed joining separately: a failed std::async launch must
    // still leave the destructor's safety fallback responsible for the thread.
    m_impl->joined = true;
    m_impl->worker = nullptr;
}
