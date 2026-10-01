// Copyright (c) 2026 The ConnectCoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <interfaces/init.h>
#include <qt/bitcoin.h>
#include <qt/bitcoingui.h>
#include <qt/guiconstants.h>
#include <qt/networkstyle.h>
#include <qt/platformstyle.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <QApplication>
#include <QIcon>
#include <QImage>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <functional>
#include <memory>

#ifdef WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};
const std::function<std::string()> G_TEST_GET_FULL_NAME{};

class DesktopIconTests : public QObject
{
    Q_OBJECT

public:
    explicit DesktopIconTests(BitcoinApplication& app) : m_app{app} {}

private Q_SLOTS:
    void startupIdentity()
    {
        const QIcon expected{QStringLiteral(":/icons/bitcoin")};
        QVERIFY(!expected.isNull());
        QVERIFY(!QApplication::windowIcon().isNull());
        QMessageBox early_dialog;
        for (const int size : {16, 32, 48}) {
            const auto pixels = expected.pixmap(size, size).toImage();
            QVERIFY(!pixels.isNull());
            QCOMPARE(QApplication::windowIcon().pixmap(size, size).toImage(), pixels);
            QCOMPARE(early_dialog.windowIcon().pixmap(size, size).toImage(), pixels);
        }
#ifdef WIN32
        PWSTR identity{nullptr};
        const auto result = GetCurrentProcessExplicitAppUserModelID(&identity);
        const QString actual = identity ? QString::fromWCharArray(identity) : QString{};
        CoTaskMemFree(identity);
        QVERIFY(SUCCEEDED(result));
        QCOMPARE(actual, QStringLiteral("ConnectCoin.Core"));
        checkNativeIcon(early_dialog);
#endif
    }

    void mainWindow()
    {
        const std::unique_ptr<const PlatformStyle> platform{PlatformStyle::instantiate(QStringLiteral("other"))};
        QVERIFY(platform);
        for (const auto chain : {ChainType::MAIN, ChainType::REGTEST}) {
            const std::unique_ptr<const NetworkStyle> network{NetworkStyle::instantiate(chain)};
            QVERIFY(network);
            BitcoinGUI window{m_app.node(), platform.get(), network.get()};
            for (const int size : {16, 32, 48, 256}) {
                const auto expected = network->getTrayAndWindowIcon().pixmap(size, size).toImage();
                QVERIFY(!expected.isNull());
                QCOMPARE(window.windowIcon().pixmap(size, size).toImage(), expected);
                QCOMPARE(QApplication::windowIcon().pixmap(size, size).toImage(), expected);
            }
#ifdef WIN32
            checkNativeIcon(window);
#endif
        }
    }

private:
#ifdef WIN32
    static void checkNativeIcon(QWidget& window)
    {
        // Create the native window without showing it or changing any pins.
        const auto handle = reinterpret_cast<HWND>(window.winId());
        QVERIFY(handle);
        QVERIFY(SendMessageW(handle, WM_GETICON, ICON_BIG, 0) != 0);
        QVERIFY(SendMessageW(handle, WM_GETICON, ICON_SMALL, 0) != 0);
    }
#endif
    BitcoinApplication& m_app;
};

int main(int argc, char* argv[])
{
    // Never reset the user's wallet settings or inspect/change login shortcuts.
    BasicTestingSetup setup{ChainType::REGTEST, TestOpts{.extra_args = {"-randomxfast=0"}}};
    QTemporaryDir preferences;
    if (!preferences.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, preferences.path());
    QCoreApplication::setOrganizationName(QAPP_ORG_NAME);
    QCoreApplication::setApplicationName(QAPP_APP_NAME_DEFAULT "-desktop-icon-test");

#ifdef WIN32
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    qputenv("QT_ASSUME_STDERR_HAS_CONSOLE", "1");
    qputenv("QT_QPA_PLATFORM", "windows");
#else
    qputenv("QT_QPA_PLATFORM", "minimal");
#ifdef Q_OS_MACOS
    qputenv("QT_STYLE_OVERRIDE", "fusion");
#endif
#endif
    Q_INIT_RESOURCE(bitcoin);
    Q_INIT_RESOURCE(bitcoin_locale);
    auto init = interfaces::MakeGuiInit(argc, argv);
    BitcoinApplication app;
    app.createNode(*init);
    app.loadGuiPreferences();
    DesktopIconTests tests{app};
    return QTest::qExec(&tests);
}

#include <desktopicon_tests.moc>
