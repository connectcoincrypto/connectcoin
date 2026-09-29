// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <qt/guipreferences.h>

#include <QCoreApplication>
#include <QDebug>
#include <QMap>
#include <QSettings>
#include <QThread>

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {
GuiPreferences* g_preferences{nullptr};
using Identity = std::tuple<QSettings::Format, QString, QString>;
} // namespace

struct GuiSettings::Profile {
    const Identity identity;
    QVariantMap values;
    std::map<QString, uint64_t> revisions;
    std::map<QString, uint64_t> removed_groups;
    std::map<QString, std::optional<QVariant>> pending;
    std::map<QString, std::optional<QVariant>> failed;
    uint64_t revision{0};
    uint64_t clear_revision{0};
    bool pending_clear{false};
    bool failed_clear{false};
    bool writer_queued{false};
    bool loaded{false};
    explicit Profile(Identity id) : identity(std::move(id)) {}
};

struct GuiPreferences::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    std::map<Identity, std::shared_ptr<GuiSettings::Profile>> profiles;
    bool stopping{false};
    std::thread worker{[this] { run(); }};

    ~Impl()
    {
        // Synchronous fallback only. Normal shutdown has already flushed all
        // widget snapshots; no nested event loop in a partially destroyed app.
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        worker.join();
    }

    template <typename Fn> std::future<void> submit(Fn&& fn)
    {
        auto task = std::make_shared<std::packaged_task<void()>>(std::forward<Fn>(fn));
        auto future = task->get_future();
        {
            std::lock_guard lock(mutex);
            assert(!stopping);
            jobs.emplace_back([task] { (*task)(); });
        }
        wake.notify_one();
        return future;
    }

    void run()
    {
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait(lock, [this] { return stopping || !jobs.empty(); });
            if (jobs.empty()) return;
            auto job = std::move(jobs.front());
            jobs.pop_front();
            lock.unlock();
            job();
            lock.lock();
        }
    }

    // Called under mutex; at most one waiting writer per profile. Storage I/O
    // never holds the cache mutex, so a stalled disk cannot block widget reads.
    void schedule(const std::shared_ptr<GuiSettings::Profile>& profile)
    {
        if (profile->writer_queued) return;
        profile->writer_queued = true;
        jobs.emplace_back([this, profile] { write(profile); });
        wake.notify_one();
    }

    void write(const std::shared_ptr<GuiSettings::Profile>& profile)
    {
        std::map<QString, std::optional<QVariant>> changes;
        bool clear;
        {
            std::lock_guard lock(mutex);
            changes.swap(profile->failed);
            clear = std::exchange(profile->failed_clear, false);
            if (std::exchange(profile->pending_clear, false)) {
                changes.clear();
                clear = true;
            }
            for (auto& [key, value] : profile->pending) changes[key] = std::move(value);
            profile->pending.clear();
            profile->writer_queued = false;
        }
        persist(profile, std::move(changes), clear);
    }

    void persist(const std::shared_ptr<GuiSettings::Profile>& profile,
                 std::map<QString, std::optional<QVariant>> changes, bool clear)
    {
        if (!clear && changes.empty()) return;
        const auto& [format, organization, application] = profile->identity;
        QSettings settings(format, QSettings::UserScope, organization, application);
        if (clear) settings.clear();
        for (const auto& [key, value] : changes) {
            if (value) settings.setValue(key, *value);
            else settings.remove(key);
        }
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            qWarning() << "Unable to save GUI preferences for" << application;
            // Keep only the latest batch, without a retry loop. A subsequent
            // write or explicit shutdown flush can recover from transient I/O
            // failures even when the GUI's cached value has not changed.
            std::lock_guard lock(mutex);
            profile->failed = std::move(changes);
            profile->failed_clear = clear;
        }
    }

    void retryFailures()
    {
        std::vector<std::shared_ptr<GuiSettings::Profile>> snapshots;
        {
            std::lock_guard lock(mutex);
            for (const auto& [identity, profile] : profiles) snapshots.push_back(profile);
        }
        for (const auto& profile : snapshots) {
            std::map<QString, std::optional<QVariant>> changes;
            bool clear;
            {
                std::lock_guard lock(mutex);
                changes.swap(profile->failed);
                clear = std::exchange(profile->failed_clear, false);
            }
            persist(profile, std::move(changes), clear);
        }
    }
};

GuiPreferences::GuiPreferences() : m_impl(std::make_unique<Impl>())
{
    assert(!g_preferences);
    g_preferences = this;
}

GuiPreferences::~GuiPreferences()
{
    m_impl.reset();
    g_preferences = nullptr;
}

std::future<void> GuiPreferences::Flush()
{
    assert(g_preferences);
    auto* store = g_preferences->m_impl.get();
    return store->submit([store] { store->retryFailures(); });
}

GuiSettings::GuiSettings()
{
    assert(g_preferences);
    m_store = g_preferences->m_impl.get();
    QString organization = QCoreApplication::organizationName();
#ifdef Q_OS_MAC
    // Match the default QSettings constructor's domain preference on macOS.
    if (!QCoreApplication::organizationDomain().isEmpty()) organization = QCoreApplication::organizationDomain();
#endif
    Identity identity{QSettings::defaultFormat(), organization, QCoreApplication::applicationName()};
    std::lock_guard lock(m_store->mutex);
    auto& profile = m_store->profiles[identity];
    if (!profile) profile = std::make_shared<Profile>(std::move(identity));
    m_profile = profile;
}

std::future<void> GuiSettings::load(bool reload) const
{
    uint64_t revision;
    {
        std::lock_guard lock(m_store->mutex);
        if (m_profile->loaded && !reload) {
            std::promise<void> ready;
            ready.set_value();
            return ready.get_future();
        }
        revision = m_profile->revision;
    }
    return m_store->submit([store = m_store, profile = m_profile, revision] {
        const auto& [format, organization, application] = profile->identity;
        QSettings settings(format, QSettings::UserScope, organization, application);
        settings.sync();
        if (settings.status() != QSettings::NoError) qWarning() << "Unable to load GUI preferences for" << application;
        QVariantMap values;
        for (const auto& key : settings.allKeys()) values.insert(key, settings.value(key));
        std::lock_guard lock(store->mutex);
        // A responsive startup wait may process newer GUI changes. Never let a
        // slow load overwrite such edits, including key removal and clear().
        if (profile->clear_revision <= revision && !profile->failed_clear) {
            for (const auto& [group, changed] : profile->removed_groups) {
                if (changed <= revision) continue;
                for (auto it = values.begin(); it != values.end();) {
                    if (it.key() == group || it.key().startsWith(group + '/')) it = values.erase(it);
                    else ++it;
                }
            }
            for (const auto& [key, changed] : profile->revisions) {
                if (changed <= revision && profile->failed.count(key) == 0) continue;
                if (profile->values.contains(key)) values.insert(key, profile->values.value(key));
                else values.remove(key);
            }
            profile->values = std::move(values);
        }
        profile->loaded = true;
    });
}

QVariant GuiSettings::value(const QString& key, const QVariant& fallback) const
{
    std::lock_guard lock(m_store->mutex);
    return m_profile->values.value(key, fallback);
}

bool GuiSettings::contains(const QString& key) const
{
    std::lock_guard lock(m_store->mutex);
    return m_profile->values.contains(key);
}

QStringList GuiSettings::allKeys() const
{
    std::lock_guard lock(m_store->mutex);
    return m_profile->values.keys();
}

void GuiSettings::setValue(const QString& key, const QVariant& value)
{
    std::lock_guard lock(m_store->mutex);
    if (m_profile->values.contains(key) && m_profile->values.value(key) == value) return;
    m_profile->values.insert(key, value);
    m_profile->revisions[key] = ++m_profile->revision;
    m_profile->pending[key] = value;
    m_store->schedule(m_profile);
}

void GuiSettings::remove(const QString& key)
{
    if (key.isEmpty()) { clear(); return; }
    std::lock_guard lock(m_store->mutex);
    const auto keys = m_profile->values.keys();
    for (const auto& candidate : keys) {
        if (candidate != key && !candidate.startsWith(key + '/')) continue;
        m_profile->values.remove(candidate);
        m_profile->revisions[candidate] = ++m_profile->revision;
        m_profile->pending[candidate] = std::nullopt;
    }
    // Preserve the tombstone even when a concurrent initial load has not yet
    // discovered this key, and retain QSettings' group-removal semantics.
    m_profile->revisions[key] = ++m_profile->revision;
    m_profile->pending[key] = std::nullopt;
    m_profile->removed_groups[key] = m_profile->revision;
    m_store->schedule(m_profile);
}

void GuiSettings::clear()
{
    std::lock_guard lock(m_store->mutex);
    m_profile->values.clear();
    m_profile->pending.clear();
    m_profile->revisions.clear();
    m_profile->removed_groups.clear();
    m_profile->clear_revision = ++m_profile->revision;
    m_profile->pending_clear = true;
    m_store->schedule(m_profile);
}

void GuiSettings::sync() const
{
    assert(QThread::currentThread() != QCoreApplication::instance()->thread());
    GuiPreferences::Flush().get();
}
