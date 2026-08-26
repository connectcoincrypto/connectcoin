# Copyright (c) 2025-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include_guard(GLOBAL)
include(GNUInstallDirs)

function(install_binary_component component)
  cmake_parse_arguments(PARSE_ARGV 1
    IC                          # prefix
    "HAS_MANPAGE;INTERNAL"      # options
    ""                          # one_value_keywords
    ""                          # multi_value_keywords
  )
  set(target_name ${component})
  get_target_property(output_name ${target_name} OUTPUT_NAME)
  if(NOT output_name)
    set(output_name ${target_name})
  endif()
  if(IC_INTERNAL)
    set(runtime_dest ${CMAKE_INSTALL_LIBEXECDIR})
  else()
    set(runtime_dest ${CMAKE_INSTALL_BINDIR})
  endif()
  install(TARGETS ${target_name}
    RUNTIME DESTINATION ${runtime_dest}
    COMPONENT ${component}
  )
  # Native Windows vcpkg builds link against DLLs. The toolchain copies them
  # beside build-tree executables, but does not install them unless its
  # experimental install hook is enabled. Invoke the same dependency installer
  # explicitly so a normal `cmake --install` tree is runnable as well.
  if(COMMAND x_vcpkg_install_local_dependencies AND NOT X_VCPKG_APPLOCAL_DEPS_INSTALL)
    x_vcpkg_install_local_dependencies(
      TARGETS ${target_name}
      DESTINATION ${runtime_dest}
      COMPONENT ${component}
    )
  endif()
  if(INSTALL_MAN AND IC_HAS_MANPAGE)
    install(FILES ${PROJECT_SOURCE_DIR}/doc/man/${output_name}.1
      DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
      COMPONENT ${component}
    )
  endif()
endfunction()
