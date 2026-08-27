// Copyright (c) 2026-present The ConnectCoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_CLIENTVERSION_BUILD_H
#define CONNECTCOIN_CLIENTVERSION_BUILD_H

#include <connectcoin-build-config.h> // IWYU pragma: keep
#include <connectcoin-build-info.h> // IWYU pragma: keep

#include <string_view>

// Git expands the following line in source archives, where build information
// cannot be obtained from a .git directory.
//$Format:%n#define GIT_COMMIT_ID "%H"$

namespace clientversion {

constexpr std::string_view BuildDescription()
{
#ifdef BUILD_GIT_TAG
    return BUILD_GIT_TAG;
#else
    #if CLIENT_VERSION_IS_RELEASE
    return "v" CLIENT_VERSION_STRING;
    #elif defined(BUILD_GIT_COMMIT)
    return "v" CLIENT_VERSION_STRING "-" BUILD_GIT_COMMIT;
    #elif defined(GIT_COMMIT_ID)
    return "v" CLIENT_VERSION_STRING "-g" GIT_COMMIT_ID;
    #else
    return "v" CLIENT_VERSION_STRING "-unk";
    #endif
#endif
}

} // namespace clientversion

#endif // CONNECTCOIN_CLIENTVERSION_BUILD_H
