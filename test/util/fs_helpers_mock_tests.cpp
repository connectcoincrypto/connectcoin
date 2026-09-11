// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Run through fs_helpers_mock_tests.py, which inserts the current function.
// Utility declarations are real; OS calls and logging are simulated.
#include <util/string.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

// On POSIX hosts deliberately load the real resource declarations too. Keep
// simulated declarations separate so their signedness never conflicts with
// the host's rlim_t, and neither test code nor the extracted function calls
// the host's resource-limit syscalls.
#ifndef _WIN32
#include <sys/resource.h>
#endif
#undef RLIM_INFINITY
#undef RLIMIT_NOFILE

namespace mock {
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int warning_count{0};
template <typename... Args> void LogInfo(const char*, const Args&...) {}
template <typename... Args> void LogWarning(const char*, const Args&...) { ++warning_count; }
template <typename... Args> void LogError(const char*, const Args&...) {}
std::string SysErrorString(int error) { return util::ToString(error); }

#if defined(TEST_CRT_UCRT) || defined(TEST_CRT_LEGACY)
constexpr int STREAM_CEILING{
#ifdef TEST_CRT_UCRT
    8192
#else
    2048
#endif
};
int streams{512};
int set_calls{0};
bool fail_set{false};
int clamp_streams{0};
int MockGetMaxStdio() { return streams; }
int MockSetMaxStdio(int requested)
{
    Check(requested == STREAM_CEILING && requested >= streams, "invalid CRT request");
    ++set_calls;
    if (fail_set) { errno = ENOMEM; return -1; }
    streams = clamp_streams ? clamp_streams : requested;
    return streams;
}
#else
#if defined(TEST_POSIX_SIGNED) || defined(TEST_DARWIN_SIGNED)
using rlim_t = std::int64_t;
constexpr rlim_t RLIM_INFINITY{-1};
#else
using rlim_t = std::uint64_t;
constexpr rlim_t RLIM_INFINITY{std::numeric_limits<rlim_t>::max()};
#endif
constexpr int RLIMIT_NOFILE{7};
constexpr int MAX_BUDGET{std::numeric_limits<int>::max()};
struct rlimit { rlim_t rlim_cur; rlim_t rlim_max; };
enum class Behavior { ACCEPT, CAP, REJECT, CLAMP };
struct State {
    rlimit current{1024, 65536};
    rlim_t hard{65536};
    Behavior behavior{Behavior::ACCEPT};
    rlim_t cap{4096};
    int error{EINVAL};
    bool query_fails{false};
    bool verify_fails{false};
    int gets{0};
    int sets{0};
    int kernel_cap{MAX_BUDGET};
    size_t kernel_size{sizeof(int)};
    bool sysctl_fails{false};
    int sysctls{0};
} state;

int getrlimit(int resource, rlimit* result)
{
    Check(resource == RLIMIT_NOFILE, "wrong queried resource");
    ++state.gets;
    if (state.query_fails || (state.gets > 1 && state.verify_fails)) { errno = EIO; return -1; }
    *result = state.current;
    return 0;
}
int setrlimit(int resource, const rlimit* requested)
{
    Check(resource == RLIMIT_NOFILE && requested->rlim_max == state.hard, "hard limit changed");
    Check(requested->rlim_cur == RLIM_INFINITY ||
          (state.current.rlim_cur != RLIM_INFINITY && requested->rlim_cur >= state.current.rlim_cur), "soft limit lowered");
    Check(++state.sets <= 33, "unbounded fallback");
    if (state.behavior == Behavior::REJECT ||
        (state.behavior == Behavior::CAP && (requested->rlim_cur == RLIM_INFINITY || requested->rlim_cur > state.cap))) {
        errno = state.error;
        return -1;
    }
    state.current = *requested;
    if (state.behavior == Behavior::CLAMP) state.current.rlim_cur = state.cap;
    return 0;
}
int sysctlbyname(const char* name, void* result, size_t* size, const void* new_value, size_t new_size)
{
    Check(std::string{name} == "kern.maxfilesperproc" && *size == sizeof(int), "wrong sysctl query");
    Check(!new_value && !new_size, "global settings modified");
    ++state.sysctls;
    if (state.sysctl_fails) { errno = EPERM; return -1; }
    *static_cast<int*>(result) = state.kernel_cap;
    *size = state.kernel_size;
    return 0;
}
#endif

// Select the simulated platform only after the real C++ and project headers.
#undef WIN32
#undef __APPLE__
#if defined(TEST_CRT_UCRT) || defined(TEST_CRT_LEGACY)
#define WIN32 1
#undef _MSC_VER
#undef _UCRT
#ifdef TEST_CRT_UCRT
#define _UCRT 1
#endif
#define _getmaxstdio MockGetMaxStdio
#define _setmaxstdio MockSetMaxStdio
#elif defined(TEST_DARWIN_UNSIGNED) || defined(TEST_DARWIN_SIGNED)
#define __APPLE__ 1
#endif

/* FUNCTION_UNDER_TEST */

int RunTests()
{
    int passed{0};
#if defined(TEST_CRT_UCRT) || defined(TEST_CRT_LEGACY)
    struct Case { int original; bool fail; int clamp; int expected; int calls; int warnings; };
    for (const auto& test : {
             Case{512, false, 0, STREAM_CEILING, 1, 0},
             Case{STREAM_CEILING, false, 0, STREAM_CEILING, 0, 0},
             Case{STREAM_CEILING + 1, false, 0, STREAM_CEILING + 1, 0, 0},
             Case{512, true, 0, 512, 1, 1},
             Case{512, false, STREAM_CEILING - 1, STREAM_CEILING - 1, 1, 0}}) {
        streams = test.original;
        fail_set = test.fail;
        clamp_streams = test.clamp;
        set_calls = warning_count = 0;
        Check(RaiseFileDescriptorLimit() == 2048, "CRT changed socket budget");
        Check(streams == test.expected && set_calls == test.calls && warning_count == test.warnings, "CRT outcome mismatch");
        ++passed;
    }
#else
    const auto reset = [](rlim_t soft, rlim_t hard) {
        state = State{};
        state.current = {soft, hard};
        state.hard = hard;
        warning_count = 0;
    };
    const auto expect = [&passed](int budget, rlim_t actual, int sets = -1) {
        Check(RaiseFileDescriptorLimit() == budget, "wrong returned budget");
        Check(state.current.rlim_cur == actual && state.current.rlim_max == state.hard, "wrong observed limits");
        Check(sets < 0 || state.sets == sets, "wrong set call count");
        ++passed;
    };
    reset(1024, 65536); expect(65536, 65536, 1);
    reset(128, 4096); expect(4096, 4096, 1);
    reset(4096, 4096); expect(4096, 4096, 0);
    reset(1024, RLIM_INFINITY); expect(MAX_BUDGET, RLIM_INFINITY, 1);
    reset(RLIM_INFINITY, RLIM_INFINITY); expect(MAX_BUDGET, RLIM_INFINITY, 0);
    for (const auto hard : {RLIM_INFINITY, rlim_t{65536}}) {
        for (const int error : {EINVAL, EPERM}) {
            for (const rlim_t cap : {rlim_t{1024}, rlim_t{1025}, rlim_t{4097}}) {
                reset(1024, hard); state.behavior = Behavior::CAP; state.error = error; state.cap = cap; expect(static_cast<int>(cap), cap);
            }
        }
    }
    reset(1024, 65536); state.query_fails = true; expect(0, 1024, 0);
    reset(1024, 65536); state.verify_fails = true; expect(1024, 65536, 1);
    reset(1024, 65536); state.behavior = Behavior::REJECT; state.error = EIO; expect(1024, 1024, 1);
    reset(1024, 65536); state.behavior = Behavior::REJECT; state.error = EPERM; expect(1024, 1024);
    reset(1024, 65536); state.behavior = Behavior::CLAMP; expect(4096, 4096, 1);
    const rlim_t above_int{static_cast<rlim_t>(MAX_BUDGET) + 1};
    reset(above_int, above_int + 1); expect(MAX_BUDGET, above_int + 1, 1);
    reset(above_int, above_int); expect(MAX_BUDGET, above_int, 0);
    reset(above_int, RLIM_INFINITY); state.behavior = Behavior::REJECT; expect(MAX_BUDGET, above_int, 1);
    reset(1024, RLIM_INFINITY); state.behavior = Behavior::CAP; state.cap = MAX_BUDGET; expect(MAX_BUDGET, MAX_BUDGET);
#if defined(TEST_DARWIN_UNSIGNED) || defined(TEST_DARWIN_SIGNED)
    reset(1024, 65536); state.kernel_cap = 12288; expect(12288, 65536, 1);
    reset(1024, 4096); state.kernel_cap = 12288; expect(4096, 4096, 1);
    reset(1024, RLIM_INFINITY); state.kernel_cap = 12288; expect(12288, RLIM_INFINITY, 1);
    reset(RLIM_INFINITY, RLIM_INFINITY); state.kernel_cap = 12288; expect(12288, RLIM_INFINITY, 0);
    reset(65536, 65536); state.kernel_cap = 12288; expect(12288, 65536, 0);
    reset(1024, RLIM_INFINITY); state.behavior = Behavior::CAP; state.cap = state.kernel_cap = 12288; expect(12288, 12288);
    reset(1024, 65536); state.sysctl_fails = true; expect(65536, 65536, 1); Check(warning_count == 1, "unreported sysctl error");
    reset(RLIM_INFINITY, RLIM_INFINITY); state.sysctl_fails = true; expect(MAX_BUDGET, RLIM_INFINITY, 0);
    reset(1024, 65536); state.kernel_size = sizeof(int) - 1; expect(65536, 65536, 1);
    reset(1024, 65536); state.kernel_cap = 0; expect(65536, 65536, 1);
    reset(1024, 65536); state.kernel_cap = -1; expect(65536, 65536, 1);
    reset(1024, 65536); state.query_fails = true; expect(0, 1024, 0); Check(state.sysctls == 0, "sysctl after initial query failure");
#endif
#endif
    std::cout << passed << " simulated OS cases passed; real util/string.h\n";
    return 0;
}
} // namespace mock

int main()
{
    return mock::RunTests();
}
