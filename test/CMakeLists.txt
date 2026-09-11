# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(create_test_config)
  set(abs_top_srcdir ${PROJECT_SOURCE_DIR})
  set(abs_top_builddir ${PROJECT_BINARY_DIR})
  set(EXEEXT ${CMAKE_EXECUTABLE_SUFFIX})

  macro(set_configure_variable var conf_var)
    if(${var})
      set(${conf_var}_VALUE true)
    else()
      set(${conf_var}_VALUE false)
    endif()
  endmacro()

  set_configure_variable(ENABLE_WALLET ENABLE_WALLET)
  set_configure_variable(BUILD_BENCH BUILD_BENCH)
  set_configure_variable(BUILD_CLI BUILD_CONNECTCOIN_CLI)
  set_configure_variable(BUILD_TX BUILD_CONNECTCOIN_TX)
  set_configure_variable(BUILD_UTIL BUILD_CONNECTCOIN_UTIL)
  set_configure_variable(BUILD_UTIL_CHAINSTATE BUILD_CONNECTCOIN_CHAINSTATE)
  set_configure_variable(BUILD_WALLET_TOOL BUILD_CONNECTCOIN_WALLET)
  set_configure_variable(BUILD_DAEMON BUILD_CONNECTCOIND)
  set_configure_variable(BUILD_FUZZ_BINARY ENABLE_FUZZ_BINARY)
  set_configure_variable(WITH_ZMQ ENABLE_ZMQ)
  set_configure_variable(WITH_EMBEDDED_ASMAP ENABLE_EMBEDDED_ASMAP)
  set_configure_variable(ENABLE_EXTERNAL_SIGNER ENABLE_EXTERNAL_SIGNER)
  set_configure_variable(WITH_USDT ENABLE_USDT_TRACEPOINTS)
  set_configure_variable(ENABLE_IPC ENABLE_IPC)
  set_configure_variable(BUILD_GUI BUILD_GUI)

  configure_file(config.ini.in config.ini USE_SOURCE_PERMISSIONS @ONLY)
endfunction()

create_test_config()

if(BUILD_TESTS)
  # Configure separately so the fixture does not inherit the module's GLOBAL
  # include guard or mutate flags in this project (or any of its dependencies).
  set(flag_probe_args
    -S "${CMAKE_CURRENT_SOURCE_DIR}/cmake/try_append_cxx_flags"
    -B "${CMAKE_CURRENT_BINARY_DIR}/cmake/try_append_cxx_flags"
    -G "${CMAKE_GENERATOR}"
    -U "CXX_SUPPORTS_*"
    -U "LINKER_SUPPORTS_*"
    -U "WARNING_COMPILES_WITHOUT_WERROR"
  )
  foreach(generator_option IN ITEMS PLATFORM TOOLSET)
    if(CMAKE_GENERATOR_${generator_option})
      if(generator_option STREQUAL "PLATFORM")
        list(APPEND flag_probe_args -A "${CMAKE_GENERATOR_PLATFORM}")
      else()
        list(APPEND flag_probe_args -T "${CMAKE_GENERATOR_TOOLSET}")
      endif()
    endif()
  endforeach()
  set(flag_probe_variables
    CMAKE_GENERATOR_INSTANCE CMAKE_MAKE_PROGRAM CMAKE_TOOLCHAIN_FILE
    CMAKE_CXX_COMPILER CMAKE_CXX_COMPILER_ARG1 CMAKE_CXX_COMPILER_TARGET
    CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN CMAKE_AR CMAKE_RANLIB CMAKE_LINKER
    CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS CMAKE_BUILD_TYPE
    CMAKE_SYSROOT CMAKE_SYSROOT_COMPILE CMAKE_SYSROOT_LINK CMAKE_FIND_ROOT_PATH
    CMAKE_OSX_SYSROOT CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_ARCHITECTURES
    CMAKE_TRY_COMPILE_CONFIGURATION CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    VCPKG_INSTALLED_DIR VCPKG_TARGET_TRIPLET VCPKG_HOST_TRIPLET
    VCPKG_CHAINLOAD_TOOLCHAIN_FILE
    ${CMAKE_TRY_COMPILE_PLATFORM_VARIABLES}
  )
  if(CMAKE_CROSSCOMPILING)
    list(APPEND flag_probe_variables CMAKE_SYSTEM_NAME CMAKE_SYSTEM_VERSION CMAKE_SYSTEM_PROCESSOR)
  endif()
  foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    list(APPEND flag_probe_variables CMAKE_CXX_FLAGS_${config} CMAKE_EXE_LINKER_FLAGS_${config})
  endforeach()
  foreach(var IN LISTS flag_probe_variables)
    if(DEFINED ${var})
      # Each -D must remain one command argument, including list-valued settings.
      string(REPLACE ";" "\\;" flag_probe_value "${${var}}")
      list(APPEND flag_probe_args "-D${var}:STRING=${flag_probe_value}")
    endif()
  endforeach()
  # Keep the fixture dependency-free even when a forwarded toolchain uses vcpkg.
  list(APPEND flag_probe_args -DVCPKG_MANIFEST_INSTALL:BOOL=OFF)
  add_test(NAME cmake_try_append_cxx_flags COMMAND "${CMAKE_COMMAND}" ${flag_probe_args})
  set_tests_properties(cmake_try_append_cxx_flags PROPERTIES LABELS "cmake" TIMEOUT 180)
endif()


function(create_test_directory_links)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/functional)
  file(GLOB functional
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES FALSE
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    functional/*.html
    functional/*.py
  )

  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/functional/data)
  file(GLOB functional_data
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES FALSE
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    functional/data/*.json
    functional/data/*.py
  )

  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/functional/mocks)
  file(GLOB functional_mocks
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES FALSE
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    functional/mocks/*.py
  )

  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/functional/test_framework)
  file(GLOB functional_test_framework
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES FALSE
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    functional/test_framework/*.csv
    functional/test_framework/*.py
  )

  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/functional/test_framework/crypto)
  file(GLOB functional_test_framework_crypto
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES FALSE
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    functional/test_framework/crypto/*.csv
    functional/test_framework/crypto/*.py
  )

  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fuzz)
  set(files_to_link
    ${functional}
    ${functional_data}
    ${functional_mocks}
    ${functional_test_framework}
    ${functional_test_framework_crypto}
    fuzz/test_runner.py
  )

  foreach(f IN LISTS files_to_link)
    if(CMAKE_HOST_WIN32)
      set(symlink)
    else()
      set(symlink SYMBOLIC)
    endif()
    file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/${f} ${CMAKE_CURRENT_BINARY_DIR}/${f} COPY_ON_ERROR ${symlink})
  endforeach()

  configure_file(${PROJECT_SOURCE_DIR}/src/test/data/siphash.json ${PROJECT_BINARY_DIR}/src/test/data/siphash.json COPYONLY)
endfunction()

create_test_directory_links()
