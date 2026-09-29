// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_GUIPREFERENCES_H
#define CONNECTCOIN_QT_GUIPREFERENCES_H

#include <QStringList>
#include <QVariant>

#include <future>
#include <memory>

/** Application-owned serialized QSettings I/O. Construct before loading a
 * profile or creating widgets; destroy after all settings-using widgets.
 * Normal shutdown explicitly flushes while the application is still alive.
 */
class GuiPreferences
{
public:
    GuiPreferences();
    ~GuiPreferences();
    GuiPreferences(const GuiPreferences&) = delete;
    GuiPreferences& operator=(const GuiPreferences&) = delete;
    static std::future<void> Flush();

private:
    friend class GuiSettings;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/** Cache-only preferences facade. Construction, reads and destruction never
 * touch storage or run an event loop. Writes update the cache immediately and
 * are coalesced on the owned worker. The application/organization/format are
 * captured at construction, never resolved later on the worker.
 *
 * Load is an explicit startup boundary, not a widget constructor operation.
 * reload is for explicit reinitialization/import, not routine reads.
 */
class GuiSettings
{
public:
    GuiSettings();
    std::future<void> load(bool reload = false) const;
    QVariant value(const QString& key, const QVariant& fallback = {}) const;
    bool contains(const QString& key) const;
    QStringList allKeys() const;
    void setValue(const QString& key, const QVariant& value);
    void remove(const QString& key);
    void clear();
    /** Worker-only barrier, for explicit options initialization/migration. */
    void sync() const;

private:
    friend class GuiPreferences;
    struct Profile;
    GuiPreferences::Impl* m_store;
    std::shared_ptr<Profile> m_profile;
};

#endif // CONNECTCOIN_QT_GUIPREFERENCES_H
