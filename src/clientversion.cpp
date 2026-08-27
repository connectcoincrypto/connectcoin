// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <clientversion.h>
#include <clientversion_build.h>

#include <tinyformat.h>
#include <util/string.h>

#include <string>
#include <vector>

using util::Join;

/**
 * Name of client reported in the 'version' message. Report the same name
 * for both connectcoind and connectcoin-qt, to make it harder for attackers to
 * target servers or GUI users specifically.
 */
const std::string UA_NAME("ConnectCoin");

static std::string FormatVersion(int nVersion)
{
    return strprintf("%d.%d.%d", nVersion / 10000, (nVersion / 100) % 100, nVersion % 100);
}

std::string FormatFullVersion()
{
    static const std::string CLIENT_BUILD{clientversion::BuildDescription()};
    return CLIENT_BUILD;
}

/**
 * Format the subversion field according to BIP 14 spec (https://github.com/bitcoin/bips/blob/master/bip-0014.mediawiki)
 */
std::string FormatSubVersion(const std::string& name, int nClientVersion, const std::vector<std::string>& comments)
{
    std::string comments_str;
    if (!comments.empty()) comments_str = strprintf("(%s)", Join(comments, "; "));
    return strprintf("/%s:%s%s/", name, FormatVersion(nClientVersion), comments_str);
}
