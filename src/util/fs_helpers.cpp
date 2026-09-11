// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <util/fs_helpers.h>
#include <random.h>
#include <sync.h>
#include <tinyformat.h>
#include <util/byte_units.h> // IWYU pragma: keep
#include <util/check.h>
#include <util/fs.h>
#include <util/log.h>
#include <util/syserror.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifndef WIN32
#include <util/string.h>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <io.h>
#include <shlobj.h>
#endif // WIN32

#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#endif

/** Mutex to protect dir_locks. */
static GlobalMutex cs_dir_locks;
/** A map that contains all the currently held directory locks. After
 * successful locking, these will be held here until the global destructor
 * cleans them up and thus automatically unlocks them, or ReleaseDirectoryLocks
 * is called.
 */
static std::map<std::string, std::unique_ptr<fsbridge::FileLock>> dir_locks GUARDED_BY(cs_dir_locks);
namespace util {
LockResult LockDirectory(const fs::path& directory, const fs::path& lockfile_name, bool probe_only)
{
    LOCK(cs_dir_locks);
    fs::path pathLockFile = directory / lockfile_name;

    // If a lock for this directory already exists in the map, don't try to re-lock it
    if (dir_locks.contains(fs::PathToString(pathLockFile))) {
        return LockResult::Success;
    }

    // Create empty lock file if it doesn't exist.
    if (auto created{fsbridge::fopen(pathLockFile, "a")}) {
        std::fclose(created);
    } else {
        return LockResult::ErrorWrite;
    }
    auto lock = std::make_unique<fsbridge::FileLock>(pathLockFile);
    if (!lock->TryLock()) {
        LogError("Error while attempting to lock directory %s: %s\n", fs::PathToString(directory), lock->GetReason());
        return LockResult::ErrorLock;
    }
    if (!probe_only) {
        // Lock successful and we're not just probing, put it into the map
        dir_locks.emplace(fs::PathToString(pathLockFile), std::move(lock));
    }
    return LockResult::Success;
}
} // namespace util
void UnlockDirectory(const fs::path& directory, const fs::path& lockfile_name)
{
    LOCK(cs_dir_locks);
    dir_locks.erase(fs::PathToString(directory / lockfile_name));
}

void ReleaseDirectoryLocks()
{
    LOCK(cs_dir_locks);
    dir_locks.clear();
}

bool CheckDiskSpace(const fs::path& dir, uint64_t additional_bytes)
{
    constexpr uint64_t min_disk_space{50_MiB};

    uint64_t free_bytes_available = fs::space(dir).available;
    return free_bytes_available >= min_disk_space + additional_bytes;
}

std::streampos GetFileSize(const char* path, std::streamsize max)
{
    std::ifstream file{path, std::ios::binary};
    file.ignore(max);
    return file.gcount();
}

bool FileCommit(FILE* file)
{
    if (fflush(file) != 0) { // harmless if redundantly called
        LogError("fflush failed: %s", SysErrorString(errno));
        return false;
    }
#ifdef WIN32
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(file));
    if (FlushFileBuffers(hFile) == 0) {
        LogError("FlushFileBuffers failed: %s", Win32ErrorString(GetLastError()));
        return false;
    }
#elif defined(__APPLE__) && defined(F_FULLFSYNC)
    if (fcntl(fileno(file), F_FULLFSYNC, 0) == -1) { // Manpage says "value other than -1" is returned on success
        LogError("fcntl F_FULLFSYNC failed: %s", SysErrorString(errno));
        return false;
    }
#elif HAVE_FDATASYNC
    if (fdatasync(fileno(file)) != 0 && errno != EINVAL) { // Ignore EINVAL for filesystems that don't support sync
        LogError("fdatasync failed: %s", SysErrorString(errno));
        return false;
    }
#else
    if (fsync(fileno(file)) != 0 && errno != EINVAL) {
        LogError("fsync failed: %s", SysErrorString(errno));
        return false;
    }
#endif
    return true;
}

void DirectoryCommit(const fs::path& dirname)
{
    (void)DirectoryCommitChecked(dirname);
}

std::optional<bool> DirectoryCommitChecked(const fs::path& dirname)
{
#ifndef WIN32
    FILE* file = fsbridge::fopen(dirname, "r");
    if (!file) return false;
    const bool synced{fsync(fileno(file)) == 0};
    const bool closed{fclose(file) == 0};
    return synced && closed;
#else
    return std::nullopt;
#endif
}

bool TruncateFile(FILE* file, unsigned int length)
{
#if defined(WIN32)
    return _chsize(_fileno(file), length) == 0;
#else
    return ftruncate(fileno(file), length) == 0;
#endif
}

int RaiseFileDescriptorLimit()
{
#if defined(WIN32)
    // CRT streams start at 512, independently of Winsock sockets. Request only
    // the documented ceiling of the linked runtime; probing arbitrary larger
    // values can invoke its fatal invalid-parameter handler. Older MinGW
    // builds can still use MSVCRT rather than the Universal CRT (UCRT).
#if defined(_MSC_VER) || defined(_UCRT)
    constexpr int max_stdio{8192};
#else
    constexpr int max_stdio{2048};
#endif
    const int original_stdio{_getmaxstdio()};
    if (original_stdio < max_stdio && _setmaxstdio(max_stdio) == -1) {
        LogWarning("Could not raise CRT stream limit to %d: %s\n", max_stdio, SysErrorString(errno));
    }
    LogInfo("CRT stream limit: %d (previously %d); separate from Winsock sockets\n", _getmaxstdio(), original_stdio);
    // Winsock sockets do not share a POSIX RLIMIT_NOFILE with CRT file streams.
    // Retain the existing connection budget; this is not a queried OS limit.
    return 2048;
#else
    struct rlimit original;
    if (getrlimit(RLIMIT_NOFILE, &original) != 0) {
        LogError("Cannot query file descriptor limit: %s\n", SysErrorString(errno));
        return 0; // Let startup's minimum-capacity check fail, rather than guess.
    }

    // P2C HTTPS sockets, wallet databases and block files all share this limit.
    // Raise to the inherited hard limit, not merely the P2P/RPC socket budget.
    // This requests a change only to this process's soft limit, never a new hard
    // limit, and does not allocate descriptors or change connection counts.
    struct rlimit effective{original};
    if (original.rlim_cur != RLIM_INFINITY && original.rlim_cur != original.rlim_max) {
        struct rlimit requested{original};
        requested.rlim_cur = original.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &requested) != 0) {
            const int raise_error{errno};
            // Some kernels reject an inherited unlimited/oversized hard limit
            // as a soft limit (e.g. Darwin). Find the
            // highest accepted finite limit without lowering the current one.
            // Bound the fallback by our int descriptor-budget representation.
            if (raise_error == EINVAL || raise_error == EPERM) {
                static_assert(std::in_range<rlim_t>(std::numeric_limits<int>::max()));
                rlim_t low{original.rlim_cur};
                rlim_t high{static_cast<rlim_t>(std::numeric_limits<int>::max())};
                if (original.rlim_max != RLIM_INFINITY) high = std::min(high, original.rlim_max);
                while (low < high) {
                    const rlim_t candidate{low + (high - low) / 2 + (high - low) % 2};
                    requested.rlim_cur = candidate;
                    if (setrlimit(RLIMIT_NOFILE, &requested) == 0) {
                        low = candidate;
                    } else {
                        if (errno != EINVAL && errno != EPERM) break;
                        high = candidate - 1;
                    }
                }
            }
            LogWarning("Could not raise file descriptor soft limit directly to the hard limit: %s\n", SysErrorString(raise_error));
        }
        // Kernels may clamp even a successful request. Never report the
        // requested value as if it were an observed limit.
        struct rlimit observed;
        if (getrlimit(RLIMIT_NOFILE, &observed) == 0) {
            effective = observed;
        } else {
            LogWarning("Cannot verify file descriptor limit; using previous limit for budgeting: %s\n", SysErrorString(errno));
        }
    }
    LogInfo("File descriptor limits: soft=%s, hard=%s\n",
            effective.rlim_cur == RLIM_INFINITY ? "unlimited" : util::ToString(effective.rlim_cur),
            effective.rlim_max == RLIM_INFINITY ? "unlimited" : util::ToString(effective.rlim_max));
    // Limit only the returned budget, not the actual OS limit. Handle the
    // platform-specific infinity sentinel before converting to a signed int.
    int descriptor_budget{std::numeric_limits<int>::max()};
    if (effective.rlim_cur != RLIM_INFINITY &&
        std::cmp_less(effective.rlim_cur, descriptor_budget)) {
        descriptor_budget = static_cast<int>(effective.rlim_cur);
    }
#ifdef __APPLE__
    // Recent Darwin kernels can report an unlimited RLIMIT_NOFILE while
    // enforcing a separate, finite per-process cap. Query it for budgeting
    // only; never change global settings or lower an inherited process limit.
    int kernel_max_files{0};
    size_t kernel_max_files_size{sizeof(kernel_max_files)};
    if (sysctlbyname("kern.maxfilesperproc", &kernel_max_files, &kernel_max_files_size, nullptr, 0) != 0) {
        LogWarning("Cannot query kernel file descriptor limit; using RLIMIT_NOFILE for budgeting: %s\n", SysErrorString(errno));
    } else if (kernel_max_files_size != sizeof(kernel_max_files) || kernel_max_files <= 0) {
        LogWarning("Invalid kern.maxfilesperproc value; using RLIMIT_NOFILE for budgeting\n");
    } else {
        LogInfo("Kernel file descriptor limit: kern.maxfilesperproc=%d\n", kernel_max_files);
        descriptor_budget = std::min(descriptor_budget, kernel_max_files);
    }
#endif
    return descriptor_budget;
#endif
}

/**
 * this function tries to make a particular range of a file allocated (corresponding to disk space)
 * it is advisory, and the range specified in the arguments will never contain live data
 */
void AllocateFileRange(FILE* file, unsigned int offset, unsigned int length)
{
#if defined(WIN32)
    // Windows-specific version
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(file));
    LARGE_INTEGER nFileSize;
    int64_t nEndPos = (int64_t)offset + length;
    nFileSize.u.LowPart = nEndPos & 0xFFFFFFFF;
    nFileSize.u.HighPart = nEndPos >> 32;
    SetFilePointerEx(hFile, nFileSize, nullptr, FILE_BEGIN);
    SetEndOfFile(hFile);
#elif defined(__APPLE__)
    // OSX specific version
    // NOTE: Contrary to other OS versions, the OSX version assumes that
    // NOTE: offset is the size of the file.
    fstore_t fst;
    fst.fst_flags = F_ALLOCATECONTIG;
    fst.fst_posmode = F_PEOFPOSMODE;
    fst.fst_offset = 0;
    fst.fst_length = length; // mac os fst_length takes the # of free bytes to allocate, not desired file size
    fst.fst_bytesalloc = 0;
    if (fcntl(fileno(file), F_PREALLOCATE, &fst) == -1) {
        fst.fst_flags = F_ALLOCATEALL;
        fcntl(fileno(file), F_PREALLOCATE, &fst);
    }
    ftruncate(fileno(file), static_cast<off_t>(offset) + length);
#else
#if defined(HAVE_POSIX_FALLOCATE)
    // Version using posix_fallocate
    off_t nEndPos = (off_t)offset + length;
    if (0 == posix_fallocate(fileno(file), 0, nEndPos)) return;
#endif
    // Fallback version
    // TODO: just write one byte per block
    static const char buf[65536] = {};
    if (fseek(file, offset, SEEK_SET)) {
        return;
    }
    while (length > 0) {
        unsigned int now = 65536;
        if (length < now)
            now = length;
        fwrite(buf, 1, now, file); // allowed to fail; this function is advisory anyway
        length -= now;
    }
#endif
}

#ifdef WIN32
fs::path GetSpecialFolderPath(int nFolder, bool fCreate)
{
    WCHAR pszPath[MAX_PATH] = L"";

    if (SHGetSpecialFolderPathW(nullptr, pszPath, nFolder, fCreate)) {
        return fs::path(pszPath);
    }

    LogError("SHGetSpecialFolderPathW() failed, could not obtain requested path.");
    return fs::path("");
}
#endif

bool RenameOver(fs::path src, fs::path dest)
{
    std::error_code error;
    fs::rename(src, dest, error);
    return !error;
}

/**
 * Ignores exceptions thrown by create_directories if the requested directory exists.
 * Specifically handles case where path p exists, but it wasn't possible for the user to
 * write to the parent directory.
 */
bool TryCreateDirectories(const fs::path& p)
{
    try {
        return fs::create_directories(p);
    } catch (const fs::filesystem_error&) {
        if (!fs::exists(p) || !fs::is_directory(p))
            throw;
    }

    // create_directories didn't create the directory, it had to have existed already
    return false;
}

std::string PermsToSymbolicString(fs::perms p)
{
    std::string perm_str(9, '-');

    auto set_perm = [&](size_t pos, fs::perms required_perm, char letter) {
        if ((p & required_perm) != fs::perms::none) {
            perm_str[pos] = letter;
        }
    };

    set_perm(0, fs::perms::owner_read,   'r');
    set_perm(1, fs::perms::owner_write,  'w');
    set_perm(2, fs::perms::owner_exec,   'x');
    set_perm(3, fs::perms::group_read,   'r');
    set_perm(4, fs::perms::group_write,  'w');
    set_perm(5, fs::perms::group_exec,   'x');
    set_perm(6, fs::perms::others_read,  'r');
    set_perm(7, fs::perms::others_write, 'w');
    set_perm(8, fs::perms::others_exec,  'x');

    return perm_str;
}

std::optional<fs::perms> InterpretPermString(const std::string& s)
{
    if (s == "owner") {
        return fs::perms::owner_read | fs::perms::owner_write;
    } else if (s == "group") {
        return fs::perms::owner_read | fs::perms::owner_write |
               fs::perms::group_read;
    } else if (s == "all") {
        return fs::perms::owner_read | fs::perms::owner_write |
               fs::perms::group_read |
               fs::perms::others_read;
    } else {
        return std::nullopt;
    }
}

bool IsDirWritable(const fs::path& dir_path)
{
    // Attempt to create a tmp file in the directory
    if (!fs::is_directory(dir_path)) throw std::runtime_error(strprintf("Path %s is not a directory", fs::PathToString(dir_path)));
    FastRandomContext rng;
    const auto tmp = dir_path / fs::PathFromString(strprintf(".tmp_%d", rng.rand64()));

    const char* mode;
#ifdef __MINGW64__
    mode = "w"; // Temporary workaround for https://github.com/bitcoin/bitcoin/issues/30210
#else
    mode = "wx";
#endif

    if (const auto created{fsbridge::fopen(tmp, mode)}) {
        std::fclose(created);
        std::error_code ec;
        fs::remove(tmp, ec); // clean up, ignore errors
        return true;
    }
    return false;
}

#ifdef __APPLE__
FSType GetFilesystemType(const fs::path& path)
{
    if (struct statfs fs_info; statfs(path.c_str(), &fs_info)) {
        return FSType::ERROR;
    } else if (std::string_view{fs_info.f_fstypename} == "exfat") {
        return FSType::EXFAT;
    }
    return FSType::OTHER;
}
#endif
