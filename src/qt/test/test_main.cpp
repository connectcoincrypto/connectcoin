// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <interfaces/init.h>
#include <interfaces/node.h>
#include <qt/bitcoin.h>
#include <qt/guiconstants.h>
#include <qt/test/apptests.h>
#include <qt/test/optiontests.h>
#include <qt/test/rpcnestedtests.h>
#include <qt/test/uritests.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#ifdef ENABLE_WALLET
#include <qt/test/addressbooktests.h>
#include <qt/test/wallettests.h>
#endif // ENABLE_WALLET

#include <QApplication>
#include <QDebug>
#include <QObject>
#include <QSettings>
#include <QTest>

#include <functional>
#include <iostream>

const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};

const std::function<std::string()> G_TEST_GET_FULL_NAME{};

// This is all you need to run all the tests
int main(int argc, char* argv[])
{
    // Initialize persistent globals with the testing setup state for sanity.
    // E.g. -datadir in gArgs is set to a temp directory dummy value (instead
    // of defaulting to the default datadir), or globalChainParams is set to
    // regtest params.
    //
    // All tests must use their own testing setup (if needed).
    fs::create_directories([] {
        BasicTestingSetup dummy{ChainType::REGTEST};
        return gArgs.GetDataDirNet() / "blocks";
    }());

    std::unique_ptr<interfaces::Init> init = interfaces::MakeGuiInit(argc, argv);
    gArgs.ForceSetArg("-listen", "0");
    gArgs.ForceSetArg("-listenonion", "0");
    gArgs.ForceSetArg("-discover", "0");
    gArgs.ForceSetArg("-dnsseed", "0");
    gArgs.ForceSetArg("-fixedseeds", "0");
    gArgs.ForceSetArg("-natpmp", "0");

    std::string error;
    if (!gArgs.ReadConfigFiles(error, true)) qWarning() << error.c_str();

    // Prefer the "minimal" platform for the test instead of the normal default
    // platform ("xcb", "windows", or "cocoa") so tests can't unintentionally
    // interfere with any background GUIs and don't require extra resources.
    // Dynamic Qt builds provided by vcpkg cannot use the minimal plugin on
    // Windows, so use the native Windows plugin for that configuration.
#if defined(WIN32)
#if defined(CONNECTCOIN_QT_TEST_USE_WINDOWS_PLATFORM)
    if (getenv("QT_QPA_PLATFORM") == nullptr) _putenv_s("QT_QPA_PLATFORM", "windows");
#else
    if (getenv("QT_QPA_PLATFORM") == nullptr) _putenv_s("QT_QPA_PLATFORM", "minimal");
#endif
#else
    setenv("QT_QPA_PLATFORM", "minimal", 0 /* overwrite */);
#endif

#ifdef Q_OS_MACOS
    // QMacStyle assumes a Cocoa platform window, which the minimal platform
    // does not provide. In particular, painting a QGroupBox can make Qt call
    // addSubview: on an invalid native object (QTBUG-49686).
    setenv("QT_STYLE_OVERRIDE", "fusion", 0 /* overwrite */);
#endif

    QCoreApplication::setOrganizationName(QAPP_ORG_NAME);
    QCoreApplication::setApplicationName(QAPP_APP_NAME_DEFAULT "-test");

    int num_test_failures{0};

    const auto run_test = [](QObject& test) {
        const int failures{QTest::qExec(&test)};
        if (failures != 0) {
            std::cerr << test.metaObject()->className() << ": " << failures << " failed test(s)\n";
        }
        return failures;
    };

    {
        BitcoinApplication app;
        app.createNode(*init);

        AppTests app_tests(app);
        num_test_failures += run_test(app_tests);

        OptionTests options_tests(app.node());
        num_test_failures += run_test(options_tests);

        URITests test1;
        num_test_failures += run_test(test1);

        RPCNestedTests test3(app.node());
        num_test_failures += run_test(test3);

#ifdef ENABLE_WALLET
        WalletTests test5(app.node());
        num_test_failures += run_test(test5);

        AddressBookTests test6(app.node());
        num_test_failures += run_test(test6);
#endif

        if (num_test_failures) {
            qWarning("\nFailed tests: %d\n", num_test_failures);
        } else {
            qDebug("\nAll tests passed.\n");
        }
    }

    QSettings settings;
    settings.clear();

    return num_test_failures;
}
