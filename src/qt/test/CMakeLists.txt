# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

set(CMAKE_AUTOMOC_MOC_OPTIONS "-p${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(connectcoin-test-qt
  apptests.cpp
  optiontests.cpp
  rpcnestedtests.cpp
  test_main.cpp
  uritests.cpp
  util.cpp
  ../../init/bitcoin-qt.cpp
)
add_windows_application_manifest(connectcoin-test-qt)

target_link_libraries(connectcoin-test-qt
  core_interface
  connectcoin_qt
  test_util
  connectcoin_node
  Boost::headers
  Qt6::Test
)

import_plugins(connectcoin-test-qt)
deploy_windows_qt_plugin(connectcoin-test-qt Qt6::QWindowsIntegrationPlugin platforms)
deploy_windows_qt_plugin(connectcoin-test-qt Qt6::QModernWindowsStylePlugin styles)

if(ENABLE_WALLET)
  target_sources(connectcoin-test-qt
    PRIVATE
      addressbooktests.cpp
      wallettests.cpp
      ../../wallet/test/wallet_test_fixture.cpp
  )
endif()

add_test(NAME connectcoin-test-qt
  COMMAND connectcoin-test-qt
)
if(WIN32 AND VCPKG_TARGET_TRIPLET)
  target_compile_definitions(connectcoin-test-qt PRIVATE CONNECTCOIN_QT_TEST_USE_WINDOWS_PLATFORM)
  set(plugin_path "$<SHELL_PATH:$<PATH:GET_PARENT_PATH,$<PATH:GET_PARENT_PATH,$<TARGET_PROPERTY:Qt6::QWindowsIntegrationPlugin,LOCATION_$<CONFIG>>>>>")
  set_property(TEST connectcoin-test-qt APPEND PROPERTY
    ENVIRONMENT_MODIFICATION
      # On Windows, vcpkg configures Qt with `-opengl dynamic`, which makes
      # the "minimal" platform plugin unusable due to internal Qt bugs.
      QT_QPA_PLATFORM=set:windows
      QT_PLUGIN_PATH=set:${plugin_path}
  )
  unset(plugin_path)
endif()

install_binary_component(connectcoin-test-qt INTERNAL)
if(WIN32 AND NOT qt_lib_type STREQUAL "STATIC_LIBRARY")
  install(FILES "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
    DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/platforms"
    COMPONENT connectcoin-test-qt
  )
  install(FILES "$<TARGET_FILE:Qt6::QModernWindowsStylePlugin>"
    DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/styles"
    COMPONENT connectcoin-test-qt
  )
endif()
