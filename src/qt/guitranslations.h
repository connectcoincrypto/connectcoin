// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_GUITRANSLATIONS_H
#define CONNECTCOIN_QT_GUITRANSLATIONS_H

#include <QString>

#include <functional>
#include <future>
#include <memory>
#include <vector>

/** GUI-owned translation facade. Catalogs and their private dependency
 * translators are created, loaded and destroyed on one persistent worker.
 * Only immutable QTranslator::translate() calls cross threads (Qt documents
 * that operation as thread-safe). Public operations are GUI-thread-only.
 */
class GuiTranslations
{
public:
    struct Source {
        QString filename;
        QString directory;
    };
    using Sources = std::vector<Source>;

    GuiTranslations();
    ~GuiTranslations();
    GuiTranslations(const GuiTranslations&) = delete;
    GuiTranslations& operator=(const GuiTranslations&) = delete;

    //! Responsive startup/reload. Later sources override earlier ones.
    void load(const Sources& sources);
    //! Load Qt and application catalogs with base/territory fallback.
    void loadLocale(const QString& locale);
    //! Unregister the facade, then drain/destroy catalogs responsively.
    void stop();

private:
    friend struct GuiTranslationsTestAccess;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::future<void> submit(std::function<void()> task);
    void loadSources(std::function<Sources()> sources);
};

#endif // CONNECTCOIN_QT_GUITRANSLATIONS_H
