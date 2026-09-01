# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(GNUInstallDirs)

# TODO: libconnectcoinkernel is a work in progress consensus engine
#       library, as more and more modules are decoupled from the
#       consensus engine, this list will shrink to only those
#       which are absolutely necessary.
add_library(connectcoinkernel
  connectcoinkernel.cpp
  chain.cpp
  checks.cpp
  chainparams.cpp
  coinstats.cpp
  context.cpp
  cs_main.cpp
  disconnected_transactions.cpp
  mempool_removal_reason.cpp
  ../arith_uint256.cpp
  ../chain.cpp
  ../coins.cpp
  ../compressor.cpp
  ../consensus/merkle.cpp
  ../consensus/p2c.cpp
  ../consensus/p2c_x509.cpp
  ../consensus/tx_check.cpp
  ../consensus/tx_verify.cpp
  ../crypto/randomx_util.cpp
  ../dbwrapper.cpp
  ../deploymentinfo.cpp
  ../deploymentstatus.cpp
  ../flatfile.cpp
  ../hash.cpp
  ../logging.cpp
  ../node/blockstorage.cpp
  ../node/chainstate.cpp
  ../node/utxo_snapshot.cpp
  ../policy/ephemeral_policy.cpp
  ../policy/feerate.cpp
  ../policy/packages.cpp
  ../policy/policy.cpp
  ../policy/rbf.cpp
  ../policy/settings.cpp
  ../policy/truc_policy.cpp
  ../pow.cpp
  ../primitives/block.cpp
  ../primitives/transaction.cpp
  ../pubkey.cpp
  ../random.cpp
  ../randomenv.cpp
  ../script/interpreter.cpp
  ../script/script.cpp
  ../script/script_error.cpp
  ../script/sigcache.cpp
  ../script/solver.cpp
  ../signet.cpp
  ../streams.cpp
  ../sync.cpp
  ../txdb.cpp
  ../txgraph.cpp
  ../txmempool.cpp
  ../uint256.cpp
  ../util/chaintype.cpp
  ../util/check.cpp
  ../util/exception.cpp
  ../util/expected.cpp
  ../util/feefrac.cpp
  ../util/fs.cpp
  ../util/fs_helpers.cpp
  ../util/hasher.cpp
  ../util/moneystr.cpp
  ../util/rbf.cpp
  ../util/signalinterrupt.cpp
  ../util/syserror.cpp
  ../util/thread.cpp
  ../util/threadnames.cpp
  ../util/time.cpp
  ../util/tokenpipe.cpp
  ../validation.cpp
  ../validationinterface.cpp
  ../versionbits.cpp
  $<TARGET_OBJECTS:connectcoin_clientversion>
  $<TARGET_OBJECTS:connectcoin_crypto>
  $<TARGET_OBJECTS:leveldb>
  $<TARGET_OBJECTS:crc32c>
)

target_raw_data_sources(connectcoinkernel NAMESPACE consensus::p2c::data
  ../consensus/p2c_roots_v1.pem
)

# Compiler warnings that apply only to the kernel and its dependencies.
# These can be more strict and/or warnings that only apply to shared
# libs.
add_library(kernel_warn_interface INTERFACE)
if(NOT MSVC)
  try_append_cxx_flags("-Wunique-object-duplication" TARGET kernel_warn_interface SKIP_LINK)
endif()

# Also manually apply the warnings to the kernel's internal dependencies
target_link_libraries(connectcoin_clientversion PRIVATE kernel_warn_interface)
target_link_libraries(connectcoin_crypto PRIVATE kernel_warn_interface)
target_link_libraries(leveldb PRIVATE kernel_warn_interface)
target_link_libraries(crc32c PRIVATE kernel_warn_interface)

target_link_libraries(connectcoinkernel
  PRIVATE
    core_interface
    kernel_warn_interface
    MbedTLS::mbedx509
    MbedTLS::mbedcrypto
    randomx
    secp256k1_objs
    $<$<PLATFORM_ID:Windows>:bcrypt>
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
    Boost::headers
)

target_include_directories(connectcoinkernel PRIVATE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/leveldb/include>)

# Add a convenience libconnectcoinkernel target as a synonym for connectcoinkernel.
add_custom_target(libconnectcoinkernel)
add_dependencies(libconnectcoinkernel connectcoinkernel)

get_target_property(connectcoinkernel_type connectcoinkernel TYPE)
if(connectcoinkernel_type STREQUAL "STATIC_LIBRARY")
  target_compile_definitions(connectcoinkernel PUBLIC CONNECTCOINKERNEL_STATIC)
endif()

configure_file(${PROJECT_SOURCE_DIR}/libconnectcoinkernel.pc.in ${PROJECT_BINARY_DIR}/libconnectcoinkernel.pc @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/libconnectcoinkernel.pc DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" COMPONENT libconnectcoinkernel)

install(TARGETS connectcoinkernel
  RUNTIME
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT libconnectcoinkernel
  LIBRARY
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT libconnectcoinkernel
  ARCHIVE
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT libconnectcoinkernel
)

# Static libconnectcoinkernel consumers also need the pinned private RandomX
# archive. Shared kernel builds absorb these position-independent objects.
set_property(TARGET randomx PROPERTY PUBLIC_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/../randomx/src/randomx.h"
)
install(TARGETS randomx
  ARCHIVE
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT libconnectcoinkernel
  PUBLIC_HEADER
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/connectcoin/randomx
    COMPONENT libconnectcoinkernel
)

install(FILES connectcoinkernel.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT libconnectcoinkernel)
