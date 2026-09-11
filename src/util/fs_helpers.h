// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_UTIL_FS_HELPERS_H
#define CONNECTCOIN_UTIL_FS_HELPERS_H

#include <util/fs.h>

#include <cstdint>
#include <cstdio>
#include <iosfwd>
#include <limits>
#include <optional>
#include <string>

#ifdef __APPLE__
enum class FSType {
    EXFAT,
    OTHER,
    ERROR
};

/**
 * Detect filesystem type for a given path.
 * Currently identifies exFAT filesystems which cause issues on macOS.
 *
 * @param[in] path The directory path to check
 * @return FSType enum indicating the filesystem type
 */
FSType GetFilesystemType(const fs::path& path);
#endif

/**
 * Ensure file contents are fully committed to disk, using a platform-specific
 * feature analogous to fsync().
 */
bool FileCommit(FILE* file);

/**
 * Sync directory contents. This is required on some environments to ensure that
 * newly created files are committed to disk.
 */
void DirectoryCommit(const fs::path& dirname);

/** Checked directory synchronization. Returns false on an open/sync/close
 * error, or nullopt when directory synchronization is unavailable (Windows).
 * A true result still depends on the filesystem and hardware honoring sync.
 */
std::optional<bool> DirectoryCommitChecked(const fs::path& dirname);

bool TruncateFile(FILE* file, unsigned int length);

/**
 * Try to raise this process's soft file descriptor limit to its inherited hard
 * limit on POSIX systems. The requested hard limit is never changed. If the
 * kernel rejects that value, try the highest accepted finite soft limit up to
 * INT_MAX. Connection counts and global system settings are not changed.
 * On Windows, independently try to raise CRT file streams to the documented
 * runtime ceiling (8192 for UCRT, 2048 for legacy MSVCRT). This does not change
 * the Winsock socket limit or install a global invalid-parameter handler.
 *
 * @returns The last observed soft limit, saturated at INT_MAX (also for
 *          RLIM_INFINITY) and capped by kern.maxfilesperproc when queryable on
 *          macOS, or 0 if the initial getrlimit fails. If verification fails
 *          after a raise, use the original soft limit for budgeting.
 *          Windows has no RLIMIT_NOFILE and returns its existing compatibility
 *          connection budget, not an actual OS file/socket limit.
 *
 */
int RaiseFileDescriptorLimit();

void AllocateFileRange(FILE* file, unsigned int offset, unsigned int length);

/**
 * Rename src to dest.
 * @return true if the rename was successful.
 */
[[nodiscard]] bool RenameOver(fs::path src, fs::path dest);

namespace util {
enum class LockResult {
    Success,
    ErrorWrite,
    ErrorLock,
};
[[nodiscard]] LockResult LockDirectory(const fs::path& directory, const fs::path& lockfile_name, bool probe_only = false);
} // namespace util
void UnlockDirectory(const fs::path& directory, const fs::path& lockfile_name);
bool CheckDiskSpace(const fs::path& dir, uint64_t additional_bytes = 0);

/** Get the size of a file by scanning it.
 *
 * @param[in] path The file path
 * @param[in] max Stop seeking beyond this limit
 * @return The file size or max
 */
std::streampos GetFileSize(const char* path, std::streamsize max = std::numeric_limits<std::streamsize>::max());

/** Release all directory locks. This is used for unit testing only, at runtime
 * the global destructor will take care of the locks.
 */
void ReleaseDirectoryLocks();

bool TryCreateDirectories(const fs::path& p);
fs::path GetDefaultDataDir();

/** Convert fs::perms to symbolic string of the form 'rwxrwxrwx'
 *
 * @param[in] p the perms to be converted
 * @return Symbolic permissions string
 */
std::string PermsToSymbolicString(fs::perms p);
/** Interpret a custom permissions level string as fs::perms
 *
 * @param[in] s Permission level string
 * @return Permissions as fs::perms
 */
std::optional<fs::perms> InterpretPermString(const std::string& s);

/** Check if a directory is writable by creating a temporary file on it.
 *
 * @param[in] dir_path Path of the directory to test
 * @return true if a temporary file could be created and removed, false otherwise.
 * @throw std::runtime_error if dir_path is not a directory.
 */
bool IsDirWritable(const fs::path& dir_path);

#ifdef WIN32
fs::path GetSpecialFolderPath(int nFolder, bool fCreate = true);
#endif

#endif // CONNECTCOIN_UTIL_FS_HELPERS_H
