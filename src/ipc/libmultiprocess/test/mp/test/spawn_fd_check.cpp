// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <mp/util.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static int CheckCallbackFailure()
{
    // The helper has no background threads that could reuse descriptors while
    // checking the two lowest available slots. Do not change mptest's limits.
    const int first{open("/dev/null", O_RDONLY)};
    const int second{open("/dev/null", O_RDONLY)};
    if (first < 0 || second < 0) return 9;
    close(first);
    close(second);
    for (int i{0}; i < 100; ++i) {
        int pid{-1};
        int child_fd{-1};
        try {
            const int fd{mp::SpawnProcess(pid, [&](int child) -> std::vector<std::string> {
                child_fd = child;
                throw std::runtime_error("expected callback failure");
            })};
            close(fd);
            return 10;
        } catch (const std::runtime_error&) {
        }
        if (child_fd != first || pid != -1) return 11;
        errno = 0;
        if (fcntl(child_fd, F_GETFD) != -1 || errno != EBADF) return 12;
        const int check_first{open("/dev/null", O_RDONLY)};
        const int check_second{open("/dev/null", O_RDONLY)};
        const bool unchanged{check_first == first && check_second == second};
        if (check_first >= 0) close(check_first);
        if (check_second >= 0) close(check_second);
        if (!unchanged) return 13;
    }
    return 0;
}

// Separate executable so constrained limits never affect mptest's threads.
int main(int argc, char* argv[])
{
    if (argc == 2 && std::string{argv[1]} == "callback-failure") return CheckCallbackFailure();
    if (argc == 5 && std::string{argv[1]} == "child") {
        const int ipc_fd{std::atoi(argv[2])};
        for (int i{3}; i < argc; ++i) {
            errno = 0;
            if (fcntl(std::atoi(argv[i]), F_GETFD) != -1 || errno != EBADF) return 1;
        }
        const char message{'x'};
        return write(ipc_fd, &message, 1) == 1 ? 0 : 2;
    }
    if (argc != 2) return 3;
    // Put the helper and any descendants in their own group for timeout cleanup.
    if (setpgid(0, 0) != 0) return 4;

    struct rlimit limits{};
    if (getrlimit(RLIMIT_NOFILE, &limits) != 0) return 5;
    const rlim_t ceiling{limits.rlim_max == RLIM_INFINITY ? 4096 : std::min<rlim_t>(4096, limits.rlim_max)};
    if (ceiling < 64) return 77;
    limits.rlim_cur = ceiling;
    limits.rlim_max = ceiling;
    if (setrlimit(RLIMIT_NOFILE, &limits) != 0) return 77;
    // Kernels may clamp the requested limit.
    if (getrlimit(RLIMIT_NOFILE, &limits) != 0 || limits.rlim_cur < 64) return 77;
    const int high_fd{static_cast<int>(limits.rlim_cur - 1)};
    const int low_fd{open("/dev/null", O_RDONLY)};
    if (low_fd < 0) return 6;
    if (fcntl(low_fd, F_DUPFD, high_fd) != high_fd) return 77;

    int pid{-1};
    const int socket{mp::SpawnProcess(pid, [&](int child_fd) {
        return std::vector<std::string>{argv[0], "child", std::to_string(child_fd),
                                        std::to_string(low_fd), std::to_string(high_fd)};
    })};
    close(low_fd);
    close(high_fd);
    char message{};
    const auto received{read(socket, &message, 1)};
    close(socket);
    int status{0};
    if (waitpid(pid, &status, 0) != pid) return 7;
    return received == 1 && message == 'x' && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 8;
}
