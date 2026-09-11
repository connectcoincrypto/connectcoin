// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <util/fs_helpers.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(fs_tests, BasicTestingSetup)

#ifdef WIN32
BOOST_AUTO_TEST_CASE(raise_crt_stream_limit)
{
    const int original{_getmaxstdio()};
    struct RestoreLimit {
        int value;
        ~RestoreLimit() { _setmaxstdio(value); }
    } restore{original};
#if defined(_MSC_VER) || defined(_UCRT)
    constexpr int runtime_ceiling{8192};
#else
    constexpr int runtime_ceiling{2048};
#endif
    BOOST_REQUIRE_EQUAL(RaiseFileDescriptorLimit(), 2048); // Separate socket budget.
    BOOST_CHECK_EQUAL(_getmaxstdio(), std::max(original, runtime_ceiling));
    // A second call must not lower a limit already raised by the first one.
    BOOST_CHECK_EQUAL(RaiseFileDescriptorLimit(), 2048);
    BOOST_CHECK_EQUAL(_getmaxstdio(), std::max(original, runtime_ceiling));

    // Exceed the CRT's default 512 streams without touching wallet/disk data.
    // Streams close before RestoreLimit runs, including on assertion failure.
    std::vector<std::unique_ptr<FILE, decltype(&std::fclose)>> streams;
    for (int i{0}; i < 600; ++i) {
        streams.emplace_back(std::fopen("NUL", "rb"), &std::fclose);
        BOOST_REQUIRE(streams.back());
    }
}
#endif

BOOST_AUTO_TEST_CASE(fsbridge_pathtostring)
{
    std::string u8_str = "fs_tests_₿_🏃";
    std::u8string str8{u8"fs_tests_₿_🏃"};
    BOOST_CHECK_EQUAL(fs::PathToString(fs::PathFromString(u8_str)), u8_str);
    BOOST_CHECK_EQUAL(fs::u8path(u8_str).utf8string(), u8_str);
    BOOST_CHECK_EQUAL(fs::path(str8).utf8string(), u8_str);
    BOOST_CHECK(fs::path(str8).u8string() == str8);
    BOOST_CHECK_EQUAL(fs::PathFromString(u8_str).utf8string(), u8_str);
    BOOST_CHECK_EQUAL(fs::PathToString(fs::u8path(u8_str)), u8_str);
#ifndef WIN32
    // On non-windows systems, verify that arbitrary byte strings containing
    // invalid UTF-8 can be round tripped successfully with PathToString and
    // PathFromString. On non-windows systems, paths are just byte strings so
    // these functions do not do any encoding. On windows, paths are Unicode,
    // and these functions do encoding and decoding, so the behavior of this
    // test would be undefined.
    std::string invalid_u8_str = "\xf0";
    BOOST_CHECK_EQUAL(invalid_u8_str.size(), 1);
    BOOST_CHECK_EQUAL(fs::PathToString(fs::PathFromString(invalid_u8_str)), invalid_u8_str);
#endif
}

BOOST_AUTO_TEST_CASE(fsbridge_stem)
{
    std::string test_filename = "fs_tests_₿_🏃.dat";
    std::string expected_stem = "fs_tests_₿_🏃";
    BOOST_CHECK_EQUAL(fs::PathToString(fs::PathFromString(test_filename).stem()), expected_stem);
}

BOOST_AUTO_TEST_CASE(fsbridge_fstream)
{
    fs::path tmpfolder = m_args.GetDataDirBase();
    // tmpfile1 should be the same as tmpfile2
    fs::path tmpfile1 = tmpfolder / fs::u8path("fs_tests_₿_🏃");
    fs::path tmpfile2 = tmpfolder / fs::path(u8"fs_tests_₿_🏃");
    {
        std::ofstream file{tmpfile1.std_path()};
        file << "bitcoin";
    }
    {
        std::ifstream file{tmpfile2.std_path()};
        std::string input_buffer;
        file >> input_buffer;
        BOOST_CHECK_EQUAL(input_buffer, "bitcoin");
    }
    {
        std::ifstream file{tmpfile1.std_path(), std::ios_base::in | std::ios_base::ate};
        std::string input_buffer;
        file >> input_buffer;
        BOOST_CHECK_EQUAL(input_buffer, "");
    }
    {
        std::ofstream file{tmpfile2.std_path(), std::ios_base::out | std::ios_base::app};
        file << "tests";
    }
    {
        std::ifstream file{tmpfile1.std_path()};
        std::string input_buffer;
        file >> input_buffer;
        BOOST_CHECK_EQUAL(input_buffer, "bitcointests");
    }
    {
        std::ofstream file{tmpfile2.std_path(), std::ios_base::out | std::ios_base::trunc};
        file << "bitcoin";
    }
    {
        std::ifstream file{tmpfile1.std_path()};
        std::string input_buffer;
        file >> input_buffer;
        BOOST_CHECK_EQUAL(input_buffer, "bitcoin");
    }
    {
        // Join an absolute path and a relative path.
        fs::path p = fsbridge::AbsPathJoin(tmpfolder, fs::u8path("fs_tests_₿_🏃"));
        BOOST_CHECK(p.is_absolute());
        BOOST_CHECK_EQUAL(tmpfile1, p);
    }
    {
        // Join two absolute paths.
        fs::path p = fsbridge::AbsPathJoin(tmpfile1, tmpfile2);
        BOOST_CHECK(p.is_absolute());
        BOOST_CHECK_EQUAL(tmpfile2, p);
    }
    {
        // Ensure joining with empty paths does not add trailing path components.
        BOOST_CHECK_EQUAL(tmpfile1, fsbridge::AbsPathJoin(tmpfile1, ""));
        BOOST_CHECK_EQUAL(tmpfile1, fsbridge::AbsPathJoin(tmpfile1, {}));
    }
}

BOOST_AUTO_TEST_CASE(rename)
{
    const fs::path tmpfolder{m_args.GetDataDirBase()};

    const fs::path path1{tmpfolder / "a"};
    const fs::path path2{tmpfolder / "b"};

    const std::string path1_contents{"1111"};
    const std::string path2_contents{"2222"};

    {
        std::ofstream file{path1.std_path()};
        file << path1_contents;
    }

    {
        std::ofstream file{path2.std_path()};
        file << path2_contents;
    }

    // Rename path1 -> path2.
    BOOST_CHECK(RenameOver(path1, path2));

    BOOST_CHECK(!fs::exists(path1));

    {
        std::ifstream file{path2.std_path()};
        std::string contents;
        file >> contents;
        BOOST_CHECK_EQUAL(contents, path1_contents);
    }
    fs::remove(path2);
}

BOOST_AUTO_TEST_SUITE_END()
