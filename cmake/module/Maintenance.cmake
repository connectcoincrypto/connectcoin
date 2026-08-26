# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include_guard(GLOBAL)

function(setup_split_debug_script)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(OBJCOPY ${CMAKE_OBJCOPY})
    set(STRIP ${CMAKE_STRIP})
    configure_file(
      contrib/devtools/split-debug.sh.in split-debug.sh
      FILE_PERMISSIONS OWNER_READ OWNER_EXECUTE
                       GROUP_READ GROUP_EXECUTE
                       WORLD_READ
      @ONLY
    )
  endif()
endfunction()

function(add_windows_deploy_target)
  configure_file(${PROJECT_SOURCE_DIR}/cmake/script/GenerateWindowsInstaller.cmake.in ${PROJECT_BINARY_DIR}/GenerateWindowsInstaller.cmake USE_SOURCE_PERMISSIONS @ONLY)
  if(MINGW AND TARGET connectcoin AND TARGET connectcoin-qt AND TARGET connectcoind AND TARGET connectcoin-cli AND TARGET connectcoin-tx AND TARGET connectcoin-wallet AND TARGET connectcoin-util AND TARGET connectcoin-test)
    add_custom_command(
      OUTPUT ${PROJECT_BINARY_DIR}/connectcoin-win64-setup.exe
      WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E make_directory release
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:connectcoin> -o release/$<TARGET_FILE_NAME:connectcoin>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:connectcoin-qt> -o release/$<TARGET_FILE_NAME:connectcoin-qt>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:connectcoind> -o release/$<TARGET_FILE_NAME:connectcoind>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:connectcoin-cli> -o release/$<TARGET_FILE_NAME:connectcoin-cli>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:connectcoin-tx> -o release/$<TARGET_FILE_NAME:connectcoin-tx>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:connectcoin-wallet> -o release/$<TARGET_FILE_NAME:connectcoin-wallet>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:connectcoin-util> -o release/$<TARGET_FILE_NAME:connectcoin-util>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:connectcoin-test> -o release/$<TARGET_FILE_NAME:connectcoin-test>
      COMMAND ${CMAKE_COMMAND} -D BIN_DIR=release -D LIBEXEC_DIR=release -P GenerateWindowsInstaller.cmake
    )
    add_custom_target(deploy DEPENDS ${PROJECT_BINARY_DIR}/connectcoin-win64-setup.exe)
  endif()
endfunction()

function(add_macos_deploy_target)
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND TARGET connectcoin-qt)
    set(macos_app "ConnectCoin-Qt.app")
    # Populate Contents subdirectory.
    configure_file(${PROJECT_SOURCE_DIR}/share/qt/Info.plist.in ${macos_app}/Contents/Info.plist NO_SOURCE_PERMISSIONS)
    file(CONFIGURE OUTPUT ${macos_app}/Contents/PkgInfo CONTENT "APPL????")
    # Populate Contents/Resources subdirectory.
    file(CONFIGURE OUTPUT ${macos_app}/Contents/Resources/empty.lproj CONTENT "")
    configure_file(${PROJECT_SOURCE_DIR}/src/qt/res/icons/connectcoin.icns ${macos_app}/Contents/Resources/connectcoin.icns NO_SOURCE_PERMISSIONS COPYONLY)
    file(CONFIGURE OUTPUT ${macos_app}/Contents/Resources/Base.lproj/InfoPlist.strings
      CONTENT "{ CFBundleDisplayName = \"@CLIENT_NAME@\"; CFBundleName = \"@CLIENT_NAME@\"; }"
    )

    add_custom_command(
      OUTPUT ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/ConnectCoin-Qt
      COMMAND ${CMAKE_COMMAND} --install ${PROJECT_BINARY_DIR} --config $<CONFIG> --component connectcoin-qt --prefix ${macos_app}/Contents/MacOS --strip
      COMMAND ${CMAKE_COMMAND} -E rename ${macos_app}/Contents/MacOS/bin/$<TARGET_FILE_NAME:connectcoin-qt> ${macos_app}/Contents/MacOS/ConnectCoin-Qt
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}/Contents/MacOS/bin
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}/Contents/MacOS/share
      VERBATIM
    )

    set(macos_zip "connectcoin-macos-app")
    if(CMAKE_HOST_APPLE)
      add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/${macos_zip}.zip
        COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/contrib/macdeploy/macdeployqtplus ${macos_app} -translations-dir=${QT_TRANSLATIONS_DIR} -zip=${macos_zip}
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/ConnectCoin-Qt
        VERBATIM
      )
      add_custom_target(deploydir
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_zip}.zip
      )
      add_custom_target(deploy
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_zip}.zip
      )
    else()
      add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/dist/${macos_app}/Contents/MacOS/ConnectCoin-Qt
        COMMAND ${CMAKE_COMMAND} -E env OBJDUMP=${CMAKE_OBJDUMP} $<TARGET_FILE:Python3::Interpreter> ${PROJECT_SOURCE_DIR}/contrib/macdeploy/macdeployqtplus ${macos_app} -translations-dir=${QT_TRANSLATIONS_DIR}
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/ConnectCoin-Qt
        VERBATIM
      )
      add_custom_target(deploydir
        DEPENDS ${PROJECT_BINARY_DIR}/dist/${macos_app}/Contents/MacOS/ConnectCoin-Qt
      )

      find_program(ZIP_EXECUTABLE zip)
      if(NOT ZIP_EXECUTABLE)
        add_custom_target(deploy
          COMMAND ${CMAKE_COMMAND} -E echo "Error: ZIP not found"
        )
      else()
        add_custom_command(
          OUTPUT ${PROJECT_BINARY_DIR}/dist/${macos_zip}.zip
          WORKING_DIRECTORY dist
          COMMAND ${PROJECT_SOURCE_DIR}/cmake/script/macos_zip.sh ${ZIP_EXECUTABLE} ${macos_zip}.zip
          VERBATIM
        )
        add_custom_target(deploy
          DEPENDS ${PROJECT_BINARY_DIR}/dist/${macos_zip}.zip
        )
      endif()
    endif()
    add_dependencies(deploydir connectcoin-qt)
    add_dependencies(deploy deploydir)
  endif()
endfunction()
