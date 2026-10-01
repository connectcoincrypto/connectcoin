# Copyright (c) 2026 The ConnectCoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include_guard(GLOBAL)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "Linux packages must be built natively on their target distribution.")
endif()
if(ENABLE_IPC)
  message(FATAL_ERROR "Linux release packages require -DENABLE_IPC=OFF.")
endif()

include(GNUInstallDirs)
if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr"
    OR NOT CMAKE_INSTALL_BINDIR STREQUAL "bin"
    OR NOT CMAKE_INSTALL_DATADIR STREQUAL "share"
    OR NOT CMAKE_INSTALL_MANDIR STREQUAL "share/man")
  message(FATAL_ERROR "Linux packages require /usr with bin, share, and share/man install directories.")
endif()

# Component mode is intentional: a monolithic ALL install also picks up tests,
# experimental libraries, and third-party development files. Group only these
# application components into one native package.
set(CPACK_COMPONENTS_ALL
  connectcoin
  connectcoind
  connectcoin-qt
  connectcoin-cli
  connectcoin-tx
  connectcoin-wallet
  connectcoin-util
)
foreach(component IN LISTS CPACK_COMPONENTS_ALL)
  if(NOT TARGET ${component})
    message(FATAL_ERROR "Linux release packages require the ${component} target to be enabled.")
  endif()
endforeach()
list(APPEND CPACK_COMPONENTS_ALL connectcoin-licenses)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)

set(CONNECTCOIN_PACKAGE_RELEASE "1" CACHE STRING "Native package revision, including an optional distribution suffix.")
set(CONNECTCOIN_PACKAGE_MAINTAINER "ConnectCoin Core developers" CACHE STRING "Native package maintainer or contact.")
if(NOT CONNECTCOIN_PACKAGE_RELEASE MATCHES "^[0-9][A-Za-z0-9.+]*$")
  message(FATAL_ERROR "CONNECTCOIN_PACKAGE_RELEASE must start with a digit and contain only letters, digits, dots, and plus signs.")
endif()
if(NOT CONNECTCOIN_PACKAGE_MAINTAINER OR CONNECTCOIN_PACKAGE_MAINTAINER MATCHES "[\r\n;]")
  message(FATAL_ERROR "CONNECTCOIN_PACKAGE_MAINTAINER must be a nonempty, single-line contact.")
endif()

set(CPACK_PACKAGE_NAME connectcoin-core)
set(CPACK_PACKAGE_VENDOR "ConnectCoin Core developers")
set(CPACK_PACKAGE_CONTACT "${CONNECTCOIN_PACKAGE_MAINTAINER}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/connectcoincrypto/connectcoin")
set(CPACK_PACKAGE_VERSION "${CLIENT_VERSION_STRING}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ConnectCoin full node and wallet")
set(CPACK_PACKAGE_DESCRIPTION "ConnectCoin Core includes the graphical wallet, full-node daemon, command-line client, and transaction, wallet, and utility tools.")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/COPYING")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_PACKAGE_RELOCATABLE OFF)
set(CPACK_STRIP_FILES ON)
set(CPACK_VERBATIM_VARIABLES ON)
if(NOT CPACK_GENERATOR)
  set(CPACK_GENERATOR "DEB;RPM")
endif()

# Shared library requirements are derived on the target distribution. Qt's
# platform plugins are loaded dynamically, so dependency scanners miss them.
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_RELEASE "${CONNECTCOIN_PACKAGE_RELEASE}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CONNECTCOIN_PACKAGE_MAINTAINER}")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_DEBIAN_PACKAGE_SECTION utils)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins")
set(CPACK_DEBIAN_COMPRESSION_TYPE xz)

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_RELEASE "${CONNECTCOIN_PACKAGE_RELEASE}")
set(CPACK_RPM_PACKAGE_LICENSE "MIT AND BSD-3-Clause AND Apache-2.0")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
set(CPACK_RPM_PACKAGE_AUTOREQPROV yes)
set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtbase-gui")
set(CPACK_RPM_COMPRESSION_TYPE xz)
# These shared directories belong to the distribution's filesystem/icon theme
# packages. Only our files and private documentation directory belong to us.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
  /usr/share/applications
  /usr/share/icons
  /usr/share/icons/hicolor
  /usr/share/icons/hicolor/1024x1024
  /usr/share/icons/hicolor/1024x1024/apps
  /usr/share/pixmaps
)

set(package_docdir "${CMAKE_INSTALL_DATADIR}/doc/${CPACK_PACKAGE_NAME}")
install(FILES "${PROJECT_SOURCE_DIR}/COPYING"
  DESTINATION "${package_docdir}" RENAME copyright COMPONENT connectcoin-licenses)
foreach(dependency IN ITEMS randomx crc32c leveldb minisketch)
  install(FILES "${PROJECT_SOURCE_DIR}/src/${dependency}/LICENSE"
    DESTINATION "${package_docdir}/licenses" RENAME "${dependency}.txt" COMPONENT connectcoin-licenses)
endforeach()
install(FILES "${PROJECT_SOURCE_DIR}/src/secp256k1/COPYING"
  DESTINATION "${package_docdir}/licenses" RENAME secp256k1.txt COMPONENT connectcoin-licenses)

# FetchContent's source-directory variable is scoped to src/. Read its global
# population record so reused/offline FetchContent sources work as well.
include(FetchContent)
FetchContent_GetProperties(connectcoin_mbedtls SOURCE_DIR package_mbedtls_source)
if(NOT EXISTS "${package_mbedtls_source}/LICENSE")
  message(FATAL_ERROR "The pinned Mbed TLS license is required for Linux packages.")
endif()
install(FILES "${package_mbedtls_source}/LICENSE"
  DESTINATION "${package_docdir}/licenses" RENAME mbedtls.txt COMPONENT connectcoin-licenses)
foreach(dependency IN ITEMS everest p256-m)
  install(FILES "${package_mbedtls_source}/3rdparty/${dependency}/README.md"
    DESTINATION "${package_docdir}/licenses" RENAME "mbedtls-${dependency}.txt" COMPONENT connectcoin-licenses)
endforeach()
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/mbedtls-notice.txt" CONTENT
"ConnectCoin Core uses the pinned Mbed TLS source under the Apache License 2.0.
The accompanying mbedtls.txt contains its upstream license text.
ConnectCoin modifies Mbed TLS's CMake compatibility declaration, RSA-PSS key
restriction handling, and X.509 verification, and supplies feature configuration
and an RSA-PSS adapter. The corresponding source and patches are available at:
https://github.com/connectcoincrypto/connectcoin
" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/mbedtls-notice.txt"
  DESTINATION "${package_docdir}/licenses" COMPONENT connectcoin-licenses)

# No maintainer scripts or service units are installed. Installing/removing the
# package manages program files only; wallets and node data are user-owned.
include(CPack)
