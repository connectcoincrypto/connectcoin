# Copyright (c) 2026 The ConnectCoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

cmake_minimum_required(VERSION 3.22)
project(LinuxPackagingFixture VERSION 1.2.3 LANGUAGES NONE)

# Model native Linux on any test host without a compiler, Qt, or package tools.
# The archive test exercises CPack's real component filtering and install rules.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_CROSSCOMPILING FALSE)
set(CMAKE_INSTALL_PREFIX /usr)
set(CMAKE_INSTALL_LIBDIR lib)
set(ENABLE_IPC OFF)
set(CLIENT_VERSION_STRING "${PROJECT_VERSION}")
set(PROJECT_SOURCE_DIR "${REPO_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${REPO_SOURCE_DIR}/cmake/module")
set(CPACK_GENERATOR TGZ)
set(CONNECTCOIN_PACKAGE_RELEASE "1.test")

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/application" "#!/bin/sh\nexit 0\n")
foreach(application IN ITEMS connectcoin connectcoind connectcoin-qt connectcoin-cli connectcoin-tx connectcoin-wallet connectcoin-util)
  if(TEST_CASE STREQUAL "missing-target" AND application STREQUAL "connectcoin-qt")
    continue()
  endif()
  add_custom_target(${application})
  install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/application"
    DESTINATION bin RENAME "${application}" COMPONENT "${application}")
endforeach()
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/application"
  DESTINATION lib RENAME forbidden-vendor.a)
install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/application"
  DESTINATION bin RENAME forbidden-test COMPONENT connectcoin-test)
install(FILES "${REPO_SOURCE_DIR}/share/applications/org.connectcoin.ConnectCoin.desktop"
  DESTINATION share/applications COMPONENT connectcoin-qt)
install(FILES "${REPO_SOURCE_DIR}/src/qt/res/icons/connectcoin.png"
  DESTINATION share/pixmaps COMPONENT connectcoin-qt)

# Populate a local fixture instead of downloading the pinned dependency. Its
# global FetchContent record matches the one produced in src/CMakeLists.txt.
set(fixture_mbedtls "${CMAKE_CURRENT_BINARY_DIR}/mbedtls-source")
file(MAKE_DIRECTORY "${fixture_mbedtls}/3rdparty/everest" "${fixture_mbedtls}/3rdparty/p256-m")
file(WRITE "${fixture_mbedtls}/LICENSE" "Fixture Mbed TLS license\n")
file(WRITE "${fixture_mbedtls}/3rdparty/everest/README.md" "Fixture Everest notice\n")
file(WRITE "${fixture_mbedtls}/3rdparty/p256-m/README.md" "Fixture p256-m notice\n")
include(FetchContent)
FetchContent_Declare(connectcoin_mbedtls SOURCE_DIR "${fixture_mbedtls}")
FetchContent_MakeAvailable(connectcoin_mbedtls)

if(TEST_CASE STREQUAL "ipc")
  set(ENABLE_IPC ON)
elseif(TEST_CASE STREQUAL "prefix")
  set(CMAKE_INSTALL_PREFIX /opt/connectcoin)
elseif(TEST_CASE STREQUAL "release")
  set(CONNECTCOIN_PACKAGE_RELEASE "1/bad")
elseif(TEST_CASE STREQUAL "cross")
  set(CMAKE_CROSSCOMPILING TRUE)
endif()

include(LinuxPackaging)

if(NOT CPACK_DEBIAN_PACKAGE_SHLIBDEPS OR NOT CPACK_RPM_PACKAGE_AUTOREQPROV)
  message(FATAL_ERROR "Native dependency scanning must be enabled.")
endif()
if(NOT CPACK_DEBIAN_PACKAGE_DEPENDS STREQUAL "qt6-qpa-plugins, qt6-wayland"
    OR NOT CPACK_RPM_PACKAGE_REQUIRES STREQUAL "qt6-qtbase-gui, qt6-qtwayland")
  message(FATAL_ERROR "Native packages must declare the dynamic Qt platform plugins.")
endif()
if(NOT CPACK_DEBIAN_PACKAGE_RELEASE STREQUAL "1.test"
    OR NOT CPACK_RPM_PACKAGE_RELEASE STREQUAL "1.test")
  message(FATAL_ERROR "Native package revision was not preserved.")
endif()
