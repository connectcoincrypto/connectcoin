# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(AddWindowsResources)
include(TargetDataSources)

configure_file(${PROJECT_SOURCE_DIR}/cmake/connectcoin-build-config.h.in connectcoin-build-config.h USE_SOURCE_PERMISSIONS @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

#=============================
# Subprojects
#=============================
# Subprojects include subdirectories that do or could have tests
# and/or benchmark binaries, such as all subtrees and univalue.
# These need to be included before CMAKE_*_OUTPUT_DIRECTORY variables
# are set, so output locations of subproject tests and libraries are
# not overridden.
include(../cmake/crc32c.cmake)
include(../cmake/leveldb.cmake)
include(../cmake/minisketch.cmake)
add_subdirectory(univalue)
if (ENABLE_IPC AND NOT WITH_EXTERNAL_LIBMULTIPROCESS)
  include(../cmake/libmultiprocess.cmake)
  add_libmultiprocess(ipc/libmultiprocess)
endif()
include(../cmake/secp256k1.cmake)
add_secp256k1(secp256k1)

# P2C certificate and TLS CertificateVerify validation is consensus-critical.
# Pin the exact Mbed TLS source archive and never use a platform TLS backend or
# the host operating system's mutable root store.
include(FetchContent)
set(ENABLE_PROGRAMS OFF CACHE BOOL "Build Mbed TLS programs" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "Build Mbed TLS tests" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "Treat Mbed TLS warnings as errors" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "Disable Mbed TLS installation" FORCE)

# Mbed TLS 3.6.7 still declares compatibility with CMake 3.5. CMake 4
# requires consumers to opt in to a newer policy floor before adding it as a
# subproject, and the Windows CI treats the otherwise emitted deprecation as
# a fatal developer warning. Keep the override scoped to the dependency.
set(connectcoin_mbedtls_policy_version_minimum_was_defined FALSE)
if(DEFINED CMAKE_POLICY_VERSION_MINIMUM)
  set(connectcoin_mbedtls_policy_version_minimum_was_defined TRUE)
  set(connectcoin_mbedtls_parent_policy_version_minimum "${CMAKE_POLICY_VERSION_MINIMUM}")
endif()
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0 AND
   (NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM OR CMAKE_POLICY_VERSION_MINIMUM VERSION_LESS 3.10))
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
endif()
FetchContent_Declare(connectcoin_mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
  URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(connectcoin_mbedtls)
if(connectcoin_mbedtls_policy_version_minimum_was_defined)
  set(CMAKE_POLICY_VERSION_MINIMUM "${connectcoin_mbedtls_parent_policy_version_minimum}")
else()
  unset(CMAKE_POLICY_VERSION_MINIMUM)
endif()
unset(connectcoin_mbedtls_parent_policy_version_minimum)
unset(connectcoin_mbedtls_policy_version_minimum_was_defined)

# Third-party diagnostics must not become fatal when the parent project uses
# CMAKE_COMPILE_WARNING_AS_ERROR. ConnectCoin's own targets retain the policy.
foreach(mbedtls_target IN ITEMS mbedcrypto mbedx509 mbedtls everest p256m)
  if(TARGET ${mbedtls_target})
    set_property(TARGET ${mbedtls_target} PROPERTY COMPILE_WARNING_AS_ERROR OFF)
    target_compile_options(${mbedtls_target} PRIVATE
      "$<$<COMPILE_LANGUAGE:C,CXX>:$<TARGET_PROPERTY:sanitize_interface,INTERFACE_COMPILE_OPTIONS>>"
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
      target_compile_definitions(${mbedtls_target} PRIVATE MBEDTLS_NO_PLATFORM_ENTROPY)
    endif()
  endif()
endforeach()
unset(mbedtls_target)

set(RANDOMX_BUILD_TESTS OFF CACHE BOOL "Build RandomX tests and benchmark tools" FORCE)
set(RANDOMX_INSTALL OFF CACHE BOOL "Install the RandomX library and public header" FORCE)
# The project's Debug flags enable -ftrapv globally, while RandomX relies on
# modular arithmetic. Remove it only from the RandomX subdirectory; restoring
# the parent value immediately keeps the policy unchanged for ConnectCoin code.
set(randomx_parent_cxx_flags_debug "${CMAKE_CXX_FLAGS_DEBUG}")
string(REGEX REPLACE "(^|[ \t])-ftrapv([ \t]|$)" "\\1\\2" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
add_subdirectory(randomx EXCLUDE_FROM_ALL)
set(CMAKE_CXX_FLAGS_DEBUG "${randomx_parent_cxx_flags_debug}")
unset(randomx_parent_cxx_flags_debug)
target_include_directories(randomx SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/randomx/src)
# RandomX executes consensus-critical, untrusted input. Apply the core hardening,
# platform definitions, and sanitizer options to its C and C++ sources, but not
# to its MASM source, which rejects compiler-specific C/C++ flags. Do not link
# core_interface: its transitive debug policy defines _GLIBCXX_DEBUG, which
# changes std::vector's layout and violates a requirement of the RandomX JIT.
get_target_property(randomx_core_compile_options core_interface INTERFACE_COMPILE_OPTIONS)
get_target_property(randomx_core_compile_definitions core_interface INTERFACE_COMPILE_DEFINITIONS)
if(randomx_core_compile_options)
  target_compile_options(randomx PRIVATE
    "$<$<COMPILE_LANGUAGE:C,CXX>:${randomx_core_compile_options}>"
  )
endif()
if(randomx_core_compile_definitions)
  target_compile_definitions(randomx PRIVATE
    "$<$<COMPILE_LANGUAGE:C,CXX>:${randomx_core_compile_definitions}>"
  )
endif()
target_compile_options(randomx PRIVATE
  "$<$<COMPILE_LANGUAGE:C,CXX>:$<TARGET_PROPERTY:sanitize_interface,INTERFACE_COMPILE_OPTIONS>>"
)
# Select defined wrapping semantics for RandomX's C++ modular arithmetic
# (notably needed by i686 LIGHT mode).
target_compile_options(randomx PRIVATE
  "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-fwrapv>"
)
# RandomX deliberately relies on defined unsigned wraparound and shifts.
# Keep the memory/thread and true-UB sanitizer coverage while suppressing only
# Clang's integer checks for defined behavior in this vendored implementation.
target_compile_options(randomx PRIVATE
  "$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-fno-sanitize=unsigned-integer-overflow,unsigned-shift-base,implicit-integer-sign-change,implicit-integer-truncation>"
  "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fno-sanitize=unsigned-integer-overflow,unsigned-shift-base,implicit-integer-sign-change,implicit-integer-truncation>"
)
unset(randomx_core_compile_options)
unset(randomx_core_compile_definitions)
target_link_options(randomx PRIVATE
  "$<TARGET_PROPERTY:sanitize_interface,INTERFACE_LINK_OPTIONS>"
)
if(TARGET Threads::Threads)
  target_link_libraries(randomx PRIVATE Threads::Threads)
endif()
if(MSVC)
  target_compile_definitions(randomx PRIVATE
    "$<$<COMPILE_LANGUAGE:C,CXX>:_CRT_SECURE_NO_WARNINGS>"
  )
endif()
# Keep upstream warnings non-fatal while retaining hardening flags.
set_property(TARGET randomx PROPERTY COMPILE_WARNING_AS_ERROR OFF)

# Set top-level target output locations.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
endif()
if(NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
endif()

set(IS_SOURCE_TARBALL FALSE)
# git will expand the next line to "set(IS_SOURCE_TARBALL TRUE)" inside archives:
#$Format:%nset(IS_SOURCE_TARBALL TRUE)$
if(NOT IS_SOURCE_TARBALL AND PROJECT_IS_TOP_LEVEL)
  find_package(Git QUIET)
endif()
add_custom_target(generate_build_info
  BYPRODUCTS ${PROJECT_BINARY_DIR}/src/connectcoin-build-info.h
  COMMAND ${CMAKE_COMMAND} -DGIT_EXECUTABLE=${GIT_EXECUTABLE} -DBUILD_INFO_HEADER_PATH=${PROJECT_BINARY_DIR}/src/connectcoin-build-info.h -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/script/GenerateBuildInfo.cmake
  COMMENT "Generating connectcoin-build-info.h"
  VERBATIM
)
add_library(connectcoin_clientversion STATIC
  clientversion.cpp
)
target_link_libraries(connectcoin_clientversion
  PRIVATE
    core_interface
)
add_dependencies(connectcoin_clientversion generate_build_info)

add_subdirectory(crypto)
add_subdirectory(util)
if(ENABLE_IPC)
  add_subdirectory(ipc)
else()
  add_library(connectcoin_ipc STATIC EXCLUDE_FROM_ALL ipc/stub.cpp)
endif()

add_library(connectcoin_consensus STATIC EXCLUDE_FROM_ALL
  arith_uint256.cpp
  consensus/merkle.cpp
  consensus/p2c.cpp
  consensus/p2c_x509.cpp
  consensus/tx_check.cpp
  hash.cpp
  primitives/block.cpp
  primitives/transaction.cpp
  pubkey.cpp
  script/interpreter.cpp
  script/script.cpp
  script/script_error.cpp
  uint256.cpp
)
target_link_libraries(connectcoin_consensus
  PRIVATE
    core_interface
    connectcoin_crypto
    MbedTLS::mbedx509
    MbedTLS::mbedcrypto
    secp256k1
)
target_include_directories(connectcoin_consensus SYSTEM PRIVATE
  ${connectcoin_mbedtls_SOURCE_DIR}/include
)
target_raw_data_sources(connectcoin_consensus NAMESPACE consensus::p2c::data
  consensus/p2c_roots_v1.pem
)
add_custom_target(generate_p2c_roots_v1_header
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/consensus/p2c_roots_v1.pem.h
)
add_dependencies(connectcoin_consensus generate_p2c_roots_v1_header)

if(WITH_ZMQ)
  add_subdirectory(zmq)
endif()

# Home for common functionality shared by different executables and libraries.
# Similar to `connectcoin_util` library, but higher-level.
add_library(connectcoin_common STATIC EXCLUDE_FROM_ALL
  addresstype.cpp
  base58.cpp
  bech32.cpp
  chain.cpp
  chainparams.cpp
  chainparamsbase.cpp
  coins.cpp
  common/args.cpp
  common/bloom.cpp
  common/config.cpp
  common/init.cpp
  common/interfaces.cpp
  common/license_info.cpp
  common/messages.cpp
  common/netif.cpp
  common/pcp.cpp
  common/run_command.cpp
  common/settings.cpp
  common/signmessage.cpp
  common/system.cpp
  common/url.cpp
  compressor.cpp
  core_io.cpp
  deploymentinfo.cpp
  external_signer.cpp
  init/common.cpp
  kernel/chainparams.cpp
  key.cpp
  key_io.cpp
  merkleblock.cpp
  musig.cpp
  net_permissions.cpp
  net_types.cpp
  netaddress.cpp
  netbase.cpp
  outputtype.cpp
  policy/feerate.cpp
  policy/policy.cpp
  pow.cpp
  crypto/randomx_util.cpp
  protocol.cpp
  psbt.cpp
  rpc/rawtransaction_util.cpp
  rpc/request.cpp
  rpc/util.cpp
  scheduler.cpp
  script/descriptor.cpp
  script/miniscript.cpp
  script/parsing.cpp
  script/sign.cpp
  script/signingprovider.cpp
  script/solver.cpp
)
target_link_libraries(connectcoin_common
  PRIVATE
    core_interface
    connectcoin_consensus
    randomx
    connectcoin_util
    univalue
    secp256k1
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
    $<$<PLATFORM_ID:Windows>:ws2_32>
)

include(InstallBinaryComponent)

if(ENABLE_WALLET)
  add_subdirectory(wallet)

  if(BUILD_WALLET_TOOL)
    add_executable(connectcoin-wallet
      bitcoin-wallet.cpp
      bitcoin-wallet-res.rc
      init/bitcoin-wallet.cpp
      wallet/wallettool.cpp
    )
    add_windows_application_manifest(connectcoin-wallet)
    target_link_libraries(connectcoin-wallet
      core_interface
      connectcoin_wallet
      connectcoin_common
      connectcoin_util
    )
    install_binary_component(connectcoin-wallet HAS_MANPAGE)
  endif()
endif()


# P2P and RPC server functionality used by the `connectcoind` and `connectcoin-qt` outputs.
add_library(connectcoin_node STATIC EXCLUDE_FROM_ALL
  addrdb.cpp
  addrman.cpp
  banman.cpp
  bip324.cpp
  blockencodings.cpp
  blockfilter.cpp
  consensus/tx_verify.cpp
  dbwrapper.cpp
  deploymentstatus.cpp
  flatfile.cpp
  headerssync.cpp
  httprpc.cpp
  httpserver.cpp
  i2p.cpp
  index/base.cpp
  index/blockfilterindex.cpp
  index/coinstatsindex.cpp
  index/txindex.cpp
  index/txospenderindex.cpp
  init.cpp
  kernel/chain.cpp
  kernel/checks.cpp
  kernel/coinstats.cpp
  kernel/context.cpp
  kernel/cs_main.cpp
  kernel/disconnected_transactions.cpp
  kernel/mempool_removal_reason.cpp
  mapport.cpp
  net.cpp
  net_processing.cpp
  netgroup.cpp
  node/abort.cpp
  node/blockmanager_args.cpp
  node/blockstorage.cpp
  node/caches.cpp
  node/chainstate.cpp
  node/chainstatemanager_args.cpp
  node/coin.cpp
  node/coins_view_args.cpp
  node/connection_types.cpp
  node/context.cpp
  node/database_args.cpp
  node/eviction.cpp
  node/interface_ui.cpp
  node/interfaces.cpp
  node/kernel_notifications.cpp
  node/mempool_args.cpp
  node/mempool_persist.cpp
  node/mempool_persist_args.cpp
  node/miner.cpp
  node/mining_args.cpp
  node/mini_miner.cpp
  node/minisketchwrapper.cpp
  node/peerman_args.cpp
  node/psbt.cpp
  node/timeoffsets.cpp
  node/transaction.cpp
  node/txdownloadman_impl.cpp
  node/txorphanage.cpp
  node/txreconciliation.cpp
  node/utxo_snapshot.cpp
  node/warnings.cpp
  noui.cpp
  policy/ephemeral_policy.cpp
  policy/fees/block_policy_estimator.cpp
  policy/fees/estimator_args.cpp
  policy/fees/estimator_man.cpp
  policy/fees/mempool_estimator.cpp
  policy/packages.cpp
  policy/rbf.cpp
  policy/settings.cpp
  policy/truc_policy.cpp
  private_broadcast.cpp
  rest.cpp
  rpc/blockchain.cpp
  rpc/external_signer.cpp
  rpc/fees.cpp
  rpc/mempool.cpp
  rpc/mining.cpp
  rpc/net.cpp
  rpc/node.cpp
  rpc/output_script.cpp
  rpc/rawtransaction.cpp
  rpc/server.cpp
  rpc/server_util.cpp
  rpc/signmessage.cpp
  rpc/txoutproof.cpp
  script/sigcache.cpp
  signet.cpp
  torcontrol.cpp
  txdb.cpp
  txgraph.cpp
  txmempool.cpp
  txrequest.cpp
  validation.cpp
  validationinterface.cpp
  versionbits.cpp
  $<$<TARGET_EXISTS:connectcoin_wallet>:wallet/init.cpp>
  $<$<NOT:$<TARGET_EXISTS:connectcoin_wallet>>:dummywallet.cpp>
)
target_link_libraries(connectcoin_node
  PRIVATE
    core_interface
    connectcoin_common
    connectcoin_util
    $<TARGET_NAME_IF_EXISTS:connectcoin_zmq>
    leveldb
    minisketch
    univalue
    $<TARGET_NAME_IF_EXISTS:Boost::headers>
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
)
if(WITH_EMBEDDED_ASMAP)
  target_compile_definitions(connectcoin_node PRIVATE ENABLE_EMBEDDED_ASMAP=1)
  include(TargetDataSources)
  target_raw_data_sources(connectcoin_node NAMESPACE node::data
    node/data/ip_asn.dat
  )
  # connectcoin_node is excluded from the default build when no node consumer is
  # enabled. Keep its generated ASMap header in the global codegen target so
  # source-analysis jobs can compile every entry in compile_commands.json.
  add_custom_target(generate_embedded_asmap ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/node/data/ip_asn.dat.h
  )
  add_dependencies(connectcoin_node generate_embedded_asmap)
endif()

# ConnectCoin wrapper executable that can call other executables.
if(BUILD_CONNECTCOIN_BIN)
  add_executable(connectcoin
    bitcoin.cpp
    bitcoin-res.rc
  )
  add_windows_application_manifest(connectcoin)
  target_link_libraries(connectcoin core_interface connectcoin_common connectcoin_util)
  install_binary_component(connectcoin HAS_MANPAGE)
endif()

# ConnectCoin Core daemon.
if(BUILD_DAEMON)
  add_executable(connectcoind
    bitcoind.cpp
    bitcoind-res.rc
    init/bitcoind.cpp
  )
  add_windows_application_manifest(connectcoind)
  target_link_libraries(connectcoind
    core_interface
    connectcoin_node
    $<TARGET_NAME_IF_EXISTS:connectcoin_wallet>
  )
  install_binary_component(connectcoind HAS_MANPAGE)
endif()
if(ENABLE_IPC AND BUILD_DAEMON)
  add_executable(connectcoin-node
    bitcoind.cpp
    init/bitcoin-node.cpp
  )
  target_link_libraries(connectcoin-node
    core_interface
    connectcoin_node
    connectcoin_ipc
    $<TARGET_NAME_IF_EXISTS:connectcoin_wallet>
  )
  install_binary_component(connectcoin-node INTERNAL)
endif()


add_library(connectcoin_cli STATIC EXCLUDE_FROM_ALL
  compat/stdin.cpp
  rpc/client.cpp
)
target_link_libraries(connectcoin_cli
  PUBLIC
    core_interface
    univalue
)


# ConnectCoin Core RPC client.
if(BUILD_CLI)
  add_executable(connectcoin-cli
    bitcoin-cli.cpp
    bitcoin-cli-res.rc
    init/basic.cpp
  )
  add_windows_application_manifest(connectcoin-cli)
  target_link_libraries(connectcoin-cli
    core_interface
    connectcoin_cli
    connectcoin_common
    connectcoin_ipc
    connectcoin_util
  )
  install_binary_component(connectcoin-cli HAS_MANPAGE)
endif()


if(BUILD_TX)
  add_executable(connectcoin-tx
    bitcoin-tx.cpp
    bitcoin-tx-res.rc
  )
  add_windows_application_manifest(connectcoin-tx)
  target_link_libraries(connectcoin-tx
    core_interface
    connectcoin_common
    connectcoin_util
    univalue
  )
  install_binary_component(connectcoin-tx HAS_MANPAGE)
endif()


if(BUILD_UTIL)
  add_executable(connectcoin-util
    bitcoin-util.cpp
    bitcoin-util-res.rc
  )
  add_windows_application_manifest(connectcoin-util)
  target_link_libraries(connectcoin-util
    core_interface
    connectcoin_common
    connectcoin_util
    univalue
  )
  install_binary_component(connectcoin-util HAS_MANPAGE)
endif()


if(BUILD_GUI)
  add_subdirectory(qt)
endif()


if(BUILD_KERNEL_LIB)
  add_subdirectory(kernel)
  if (BUILD_KERNEL_TEST)
    add_subdirectory(test/kernel)
  endif()
  if(BUILD_UTIL_CHAINSTATE)
    add_executable(connectcoin-chainstate
      connectcoin-chainstate.cpp
    )
    add_dependencies(connectcoin-chainstate generate_build_info)
    add_windows_application_manifest(connectcoin-chainstate)
    target_link_libraries(connectcoin-chainstate
      PRIVATE
        core_interface
        connectcoinkernel
    )
    install_binary_component(connectcoin-chainstate INTERNAL)
  endif()
endif()


add_subdirectory(test/util)
if(BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(BUILD_TESTS)
  add_subdirectory(test)
endif()

if(BUILD_FUZZ_BINARY)
  add_subdirectory(test/fuzz)
endif()
