// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <common/license_info.h>

#include <tinyformat.h>
#include <util/translation.h>

#include <string>

std::string CopyrightHolders(const std::string& strPrefix)
{
    const auto copyright_devs = strprintf(_(COPYRIGHT_HOLDERS), COPYRIGHT_HOLDERS_SUBSTITUTION).translated;
    std::string strCopyrightHolders = strPrefix + copyright_devs;

    // Make sure Bitcoin Core copyright is not removed by accident
    if (copyright_devs.find("Bitcoin Core") == std::string::npos) {
        strCopyrightHolders += "\n" + strPrefix + "The Bitcoin Core developers";
    }
    return strCopyrightHolders;
}

std::string LicenseInfo()
{
    const bool has_project_url{!std::string{CLIENT_URL}.empty()};
    const std::string project_info = !has_project_url
        ? strprintf(_("Please contribute if you find %s useful."), CLIENT_NAME).translated
        : strprintf(_("Please contribute if you find %s useful. Visit %s for further information about the software."),
                    CLIENT_NAME, "<" CLIENT_URL ">").translated;
    const std::string source_info = !has_project_url
        ? std::string{_("No project-owned public source URL is configured for this development build.")}
        : strprintf(_("The source code is available from %s."), "<" CLIENT_URL ">").translated;

    return CopyrightHolders(strprintf(_("Copyright (C) %i-%i"), 2009, COPYRIGHT_YEAR).translated + " ") + "\n" +
           "\n" +
           project_info +
           "\n" +
           source_info +
           "\n" +
           "\n" +
           _("This is experimental software.") + "\n" +
           strprintf(_("Distributed under the MIT software license, see the accompanying file %s or %s"), "COPYING", "<https://opensource.org/license/MIT>").translated +
           "\n";
}
