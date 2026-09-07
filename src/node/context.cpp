// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/context.h>
#include <node/cpu_miner.h>

#include <addrman.h>
#include <banman.h>
#include <interfaces/chain.h>
#include <interfaces/mining.h>
#include <kernel/context.h>
#include <key.h>
#include <net.h>
#include <net_processing.h>
#include <netgroup.h>
#include <node/kernel_notifications.h>
#include <node/warnings.h>
#include <policy/fees/estimator_man.h>
#include <scheduler.h>
#include <torcontrol.h>
#include <txmempool.h>
#include <validation.h>
#include <validationinterface.h>

namespace node {
NodeContext::NodeContext() = default;
NodeContext::~NodeContext()
{
    // Also handle owners that destroy the context without the normal Shutdown
    // path: notifications and the scheduler are declared after cpu_miner and
    // would otherwise be destroyed before its workers had been joined.
    cpu_miner.reset();
}
} // namespace node
