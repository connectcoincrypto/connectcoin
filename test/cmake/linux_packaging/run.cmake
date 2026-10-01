# Copyright (c) 2026 The ConnectCoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

cmake_minimum_required(VERSION 3.22)
if(NOT REPO_SOURCE_DIR OR NOT TEST_BINARY_DIR)
  message(FATAL_ERROR "REPO_SOURCE_DIR and TEST_BINARY_DIR are required.")
endif()
get_filename_component(cmake_bindir "${CMAKE_COMMAND}" DIRECTORY)
find_program(CPACK_COMMAND cpack HINTS "${cmake_bindir}" REQUIRED)

function(configure_fixture test_case expected_error)
  execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${CMAKE_CURRENT_LIST_DIR}"
    -B "${TEST_BINARY_DIR}/${test_case}"
    "-DREPO_SOURCE_DIR=${REPO_SOURCE_DIR}"
    "-DTEST_CASE=${test_case}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(expected_error)
    if(result EQUAL 0 OR NOT "${output}\n${error}" MATCHES "${expected_error}")
      message(FATAL_ERROR "Fixture ${test_case} did not report ${expected_error}:\n${output}\n${error}")
    endif()
  elseif(NOT result EQUAL 0)
    message(FATAL_ERROR "Fixture configure failed:\n${output}\n${error}")
  endif()
endfunction()

configure_fixture(valid "")
configure_fixture(missing-target "require the connectcoin-qt target")
configure_fixture(ipc "require -DENABLE_IPC=OFF")
configure_fixture(prefix "require /usr")
configure_fixture(release "CONNECTCOIN_PACKAGE_RELEASE must start")
configure_fixture(cross "must be built natively")

execute_process(COMMAND "${CPACK_COMMAND}" -G TGZ
  WORKING_DIRECTORY "${TEST_BINARY_DIR}/valid"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Fixture packaging failed:\n${output}\n${error}")
endif()
file(GLOB archives "${TEST_BINARY_DIR}/valid/*.tar.gz")
list(LENGTH archives archive_count)
if(NOT archive_count EQUAL 1)
  message(FATAL_ERROR "Expected exactly one combined package, found ${archive_count}.")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar tf ${archives}
  RESULT_VARIABLE result OUTPUT_VARIABLE manifest ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot inspect fixture package: ${error}")
endif()
string(REPLACE "\r" "" manifest "${manifest}")
foreach(application IN ITEMS connectcoin connectcoind connectcoin-qt connectcoin-cli connectcoin-tx connectcoin-wallet connectcoin-util)
  if(NOT manifest MATCHES "(^|\n)([^\n]*/)?usr/bin/${application}\n")
    message(FATAL_ERROR "Package is missing /usr/bin/${application}:\n${manifest}")
  endif()
endforeach()
foreach(path IN ITEMS
    share/applications/org.connectcoin.ConnectCoin.desktop
    share/pixmaps/connectcoin.png
    share/doc/connectcoin-core/copyright
    share/doc/connectcoin-core/licenses/randomx.txt
    share/doc/connectcoin-core/licenses/mbedtls.txt
    share/doc/connectcoin-core/licenses/mbedtls-notice.txt
    share/doc/connectcoin-core/licenses/mbedtls-everest.txt
    share/doc/connectcoin-core/licenses/mbedtls-p256-m.txt)
  if(NOT manifest MATCHES "(^|\n)([^\n]*/)?usr/${path}\n")
    message(FATAL_ERROR "Package is missing /usr/${path}:\n${manifest}")
  endif()
endforeach()
if(manifest MATCHES "forbidden|usr/local/|home/|systemd/")
  message(FATAL_ERROR "Package contains files outside the release components:\n${manifest}")
endif()
message(STATUS "Linux packaging fixture passed: combined payload, notices, metadata, dependency policy, and invalid configurations.")
