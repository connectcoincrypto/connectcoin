// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_IPC_CAPNP_PROTOCOL_H
#define CONNECTCOIN_IPC_CAPNP_PROTOCOL_H

#include <memory>

namespace ipc {
class Protocol;
namespace capnp {
std::unique_ptr<Protocol> MakeCapnpProtocol(const char* exe_name);
} // namespace capnp
} // namespace ipc

#endif // CONNECTCOIN_IPC_CAPNP_PROTOCOL_H
