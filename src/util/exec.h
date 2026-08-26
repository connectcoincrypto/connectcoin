// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_UTIL_EXEC_H
#define CONNECTCOIN_UTIL_EXEC_H

#include <util/fs.h>

#include <string_view>

namespace util {
//! Cross-platform wrapper for POSIX execvp function.
//! Arguments and return value are the same as for POSIX execvp, and the argv
//! array should consist of null terminated strings and be null terminated
//! itself, like the POSIX function.
int ExecVp(const char* file, char* const argv[]);
#ifdef WIN32
//! Execute a process on Windows, wait for it to finish, and return its exit code.
//! Returns -1 and sets errno if the process could not be started.
int SpawnVpWait(const char* file, char* const argv[]);
#endif
//! Return path to current executable assuming it was invoked with argv0.
//! If path could not be determined, returns an empty path.
fs::path GetExePath(std::string_view argv0);
} // namespace util

#endif // CONNECTCOIN_UTIL_EXEC_H
