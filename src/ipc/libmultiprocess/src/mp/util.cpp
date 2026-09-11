// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <mp/config.h>
#include <mp/util.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <kj/common.h>
#include <kj/string-tree.h>
#include <limits>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <system_error>
#include <thread> // NOLINT(misc-include-cleaner) // IWYU pragma: keep
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#ifdef HAVE_PTHREAD_GETTHREADID_NP
#include <pthread_np.h>
#endif // HAVE_PTHREAD_GETTHREADID_NP

namespace fs = std::filesystem;

namespace mp {
namespace {

class ScopedFd
{
    int m_fd;

public:
    explicit ScopedFd(int fd) : m_fd{fd} {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ~ScopedFd()
    {
        if (m_fd >= 0) close(Release());
    }
    // Relinquish ownership before close(): even a failed close must not be
    // retried after another thread could reuse the descriptor number.
    int Release() noexcept { return std::exchange(m_fd, -1); }
};

std::vector<char*> MakeArgv(const std::vector<std::string>& args)
{
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

//! Inclusive fallback bound. Query in the parent, not in the post-fork child.
int MaxFd()
{
    int max_fd{std::numeric_limits<int>::max()};
    struct rlimit nofile{};
    if (getrlimit(RLIMIT_NOFILE, &nofile) == 0 &&
        nofile.rlim_max != RLIM_INFINITY && nofile.rlim_max > 0 &&
        std::cmp_less_equal(nofile.rlim_max - 1, max_fd)) {
        // Use the hard limit so lowering only the soft limit does not hide
        // already-open descriptors. Do not narrow the infinity sentinel.
        max_fd = static_cast<int>(nofile.rlim_max - 1);
    }
#ifdef __APPLE__
    // Darwin can report an unlimited resource limit while its descriptor
    // table still has this finite kernel ceiling.
    int kernel_limit{0};
    size_t size{sizeof(kernel_limit)};
    if (sysctlbyname("kern.maxfilesperproc", &kernel_limit, &size, nullptr, 0) == 0 &&
        size == sizeof(kernel_limit) && kernel_limit > 0) {
        max_fd = std::min(max_fd, kernel_limit - 1);
    }
#endif
    return max_fd;
}

//! Close an inclusive range without allocating or using library locks after fork.
bool CloseDescriptors(int first, int last, int max_fd)
{
    if (first > last) return true;
#if defined(__linux__) && defined(SYS_close_range)
    // Raw syscall also works with libc versions predating close_range(). The
    // child's descriptor table is already private after fork().
    if (syscall(SYS_close_range, static_cast<unsigned int>(first),
                static_cast<unsigned int>(last), 0U) == 0) return true;
#endif
#if defined(__FreeBSD__)
    if (last == std::numeric_limits<int>::max()) {
        closefrom(first);
        return true;
    }
#elif defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    if (last == std::numeric_limits<int>::max()) {
        int result;
        do {
            result = closefrom(first);
        } while (result != 0 && errno == EINTR);
        if (result == 0 || errno == EBADF) return true;
    }
#endif
    // Portable fallback, including older Linux kernels. Close the last valid
    // descriptor too, without incrementing past INT_MAX for unlimited limits.
    last = std::min(last, max_fd);
    // If no finite bound is known and the range API is unavailable, fail
    // closed instead of leaking descriptors or making billions of syscalls.
    if (last == std::numeric_limits<int>::max()) return false;
    for (int fd{first}; fd < last; ++fd) close(fd);
    if (first <= last) close(last);
    return true;
}

bool CloseChildDescriptors(int keep_fd, int max_fd)
{
    if (!CloseDescriptors(3, keep_fd - 1, max_fd)) return false;
    if (keep_fd < std::numeric_limits<int>::max()) {
        return CloseDescriptors(std::max(3, keep_fd + 1), std::numeric_limits<int>::max(), max_fd);
    }
    return true;
}

} // namespace

std::string ThreadName(const char* exe_name)
{
    char thread_name[16] = {0};
#ifdef HAVE_PTHREAD_GETNAME_NP
    pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name));
#endif // HAVE_PTHREAD_GETNAME_NP

    std::ostringstream buffer;
    buffer << (exe_name ? exe_name : "") << "-" << getpid() << "/";

    if (thread_name[0] != '\0') {
        buffer << thread_name << "-";
    }

    // Prefer platform specific thread ids over the standard C++11 ones because
    // the former are shorter and are the same as what gdb prints "LWP ...".
#ifdef __linux__
    buffer << syscall(SYS_gettid);
#elif defined(HAVE_PTHREAD_THREADID_NP)
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    buffer << tid;
#elif defined(HAVE_PTHREAD_GETTHREADID_NP)
    buffer << pthread_getthreadid_np();
#else
    buffer << std::this_thread::get_id();
#endif

    return std::move(buffer).str();
}

std::string LogEscape(const kj::StringTree& string, size_t max_size)
{
    std::string result;
    string.visit([&](const kj::ArrayPtr<const char>& piece) {
        if (result.size() > max_size) return;
        for (const char c : piece) {
            if (c == '\\') {
                result.append("\\\\");
            } else if (c < 0x20 || c > 0x7e) {
                char escape[4];
                snprintf(escape, sizeof(escape), "\\%02x", static_cast<unsigned char>(c));
                result.append(escape);
            } else {
                result.push_back(c);
            }
            if (result.size() > max_size) {
                result += "...";
                break;
            }
        }
    });
    return result;
}

int SpawnProcess(int& pid, FdToArgsFn&& fd_to_args)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        throw std::system_error(errno, std::system_category(), "socketpair");
    }
    ScopedFd child_socket{fds[0]};
    ScopedFd parent_socket{fds[1]};

    // Evaluate the callback and build the argv array before forking.
    //
    // The parent process may be multi-threaded and holding internal library
    // locks at fork time. In that case, running code that allocates memory or
    // takes locks in the child between fork() and exec() can deadlock
    // indefinitely. Precomputing arguments in the parent avoids this.
    const std::vector<std::string> args{fd_to_args(fds[0])};
    const std::vector<char*> argv{MakeArgv(args)};
    const int max_fd{MaxFd()};

    pid = fork();
    if (pid == -1) {
        throw std::system_error(errno, std::system_category(), "fork");
    }
    // Parent process closes the descriptor for socket 0, child closes the
    // descriptor for socket 1. On failure, the parent throws, but the child
    // must _exit(126) (post-fork child must not throw).
    if (close((pid ? child_socket : parent_socket).Release()) != 0) {
        if (pid) {
            throw std::system_error(errno, std::system_category(), "close");
        }
        static constexpr char msg[] = "SpawnProcess(child): close(fds[1]) failed\n";
        const ssize_t writeResult = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)writeResult;
        _exit(126);
    }

    if (!pid) {
        // Close inherited descriptor ranges except socket 0. The portable
        // fallback uses the known limit bound. Do not throw, allocate, or do
        // non-fork-safe work here.
        if (!CloseChildDescriptors(fds[0], max_fd)) {
            static constexpr char msg[] = "SpawnProcess(child): cannot safely bound descriptor cleanup\n";
            const ssize_t write_result{::write(STDERR_FILENO, msg, sizeof(msg) - 1)};
            (void)write_result;
            _exit(126);
        }

        execvp(argv[0], argv.data());
        // Do not call stdio or format a message in the post-fork child.
        static constexpr char msg[] = "SpawnProcess(child): execvp failed\n";
        const ssize_t write_result{::write(STDERR_FILENO, msg, sizeof(msg) - 1)};
        (void)write_result;
        _exit(127);
    }
    return parent_socket.Release();
}

void ExecProcess(const std::vector<std::string>& args)
{
    const std::vector<char*> argv{MakeArgv(args)};
    if (execvp(argv[0], argv.data()) != 0) {
        perror("execvp failed");
        if (errno == ENOENT && !args.empty()) {
            std::cerr << "Missing executable: " << fs::weakly_canonical(args.front()) << '\n';
        }
        _exit(1);
    }
}

int WaitProcess(int pid)
{
    int status;
    if (::waitpid(pid, &status, /*options=*/0) != pid) {
        throw std::system_error(errno, std::system_category(), "waitpid");
    }
    return status;
}

} // namespace mp
