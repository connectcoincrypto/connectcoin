# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Wallet functionality used by connectcoind and connectcoin-wallet executables.
add_library(connectcoin_wallet STATIC EXCLUDE_FROM_ALL
  coincontrol.cpp
  coinselection.cpp
  context.cpp
  crypter.cpp
  db.cpp
  dump.cpp
  export.cpp
  external_signer_scriptpubkeyman.cpp
  feebumper.cpp
  fees.cpp
  history.cpp
  interfaces.cpp
  load.cpp
  migrate.cpp
  p2c.cpp
  p2c_claim.cpp
  p2c_tls.cpp
  p2c_tls_private.c
  p2c_worker_impl.cpp
  receive.cpp
  rpc/addresses.cpp
  rpc/backup.cpp
  rpc/coins.cpp
  rpc/encrypt.cpp
  rpc/p2c.cpp
  rpc/signmessage.cpp
  rpc/spend.cpp
  rpc/transactions.cpp
  rpc/util.cpp
  rpc/wallet.cpp
  scriptpubkeyman.cpp
  spend.cpp
  sqlite.cpp
  transaction.cpp
  wallet.cpp
  walletdb.cpp
  walletutil.cpp
)
target_link_libraries(connectcoin_wallet
  PRIVATE
    core_interface
    connectcoin_common
    SQLite3::SQLite3
    univalue
    Boost::headers
    MbedTLS::mbedtls
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
)
target_include_directories(connectcoin_wallet SYSTEM PRIVATE
  ${connectcoin_mbedtls_SOURCE_DIR}/library
)
