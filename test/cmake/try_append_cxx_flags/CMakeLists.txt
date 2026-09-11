# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Standalone regression test; configure with the compiler under test, e.g.:
# cmake -S test/cmake/try_append_cxx_flags -B build/flag-probe -T ClangCL
cmake_minimum_required(VERSION 3.22)
project(ConnectCoinCXXFlagProbeTest LANGUAGES CXX)

if(MSVC)
  set(supported_flag "/nologo")
  set(warning_test_flag "/W4")
  set(unsupported_flags "-fno-extended-identifiers" "-fmacro-prefix-map=A=B" "-fstack-reuse=none")
else()
  set(supported_flag "-Werror")
  set(warning_test_flag "-Wall")
  set(unsupported_flags "-fconnectcoin-unsupported-probe-option")
endif()

# Simulate a build directory configured when ignored options were accepted.
foreach(flag IN LISTS unsupported_flags)
  string(MAKE_C_IDENTIFIER "${flag}" flag_id)
  string(TOUPPER "${flag_id}" flag_id)
  set(CXX_SUPPORTS_${flag_id} 1 CACHE INTERNAL "Legacy result without strict warning checks" FORCE)
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/module/TryAppendLinkerFlag.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/module/TryAppendCXXFlags.cmake")
if(NOT working_compiler_werror_flag)
  message(FATAL_ERROR "No working warnings-as-errors option")
endif()

add_library(probe_interface INTERFACE)
foreach(flag IN LISTS unsupported_flags)
  try_append_cxx_flags("${flag}" TARGET probe_interface VAR selected_flags RESULT_VAR accepted SKIP_LINK)
  if(accepted OR selected_flags)
    message(FATAL_ERROR "Ignored option accepted: ${flag}")
  endif()
endforeach()

# Prove a real warning succeeds normally and fails under the helper's policy.
# Use MSVC's C4068 unknown-pragma warning and #warning on other compilers.
set(warning_source [=[
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(1: 4068)
#pragma connectcoin_flag_probe_warning
#else
#warning connectcoin_flag_probe_warning
#endif
int main() { return 0; }
]=])
include(CMakePushCheckState)
cmake_push_check_state(RESET)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# The parent project may promote all warnings or this specific diagnostic.
if(MSVC)
  set(CMAKE_REQUIRED_FLAGS "/WX-")
else()
  set(CMAKE_REQUIRED_FLAGS "-Wno-error")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  string(APPEND CMAKE_REQUIRED_FLAGS " -Wno-error=#warnings")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  string(APPEND CMAKE_REQUIRED_FLAGS " -Wno-error=cpp")
endif()
# Rerun the fixture baseline, including after a previous warning-policy failure.
unset(WARNING_COMPILES_WITHOUT_WERROR CACHE)
check_cxx_source_compiles("${warning_source}" WARNING_COMPILES_WITHOUT_WERROR)
cmake_pop_check_state()
if(NOT WARNING_COMPILES_WITHOUT_WERROR)
  message(FATAL_ERROR "Warning fixture must compile without warning promotion")
endif()
try_append_cxx_flags("${warning_test_flag}" SOURCE "${warning_source}"
  TARGET probe_interface VAR selected_flags RESULT_VAR accepted SKIP_LINK
)
if(accepted OR selected_flags)
  message(FATAL_ERROR "Compiler warning was not rejected")
endif()

try_append_cxx_flags("${supported_flag}" TARGET probe_interface VAR selected_flags RESULT_VAR accepted)
get_target_property(target_flags probe_interface INTERFACE_COMPILE_OPTIONS)
get_target_property(target_link_flags probe_interface INTERFACE_LINK_OPTIONS)
if(NOT accepted OR NOT selected_flags STREQUAL supported_flag OR
   NOT target_flags STREQUAL supported_flag OR NOT target_link_flags STREQUAL supported_flag)
  message(FATAL_ERROR "Supported option was not appended correctly: ${supported_flag}")
endif()
message(STATUS "Flag probes reject warnings and legacy results; compile/link options propagate (${CMAKE_CXX_COMPILER_ID})")
