#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Exercise the opt-in, wallet-independent continuous CPU miner."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.segwit_addr import encode_segwit_address
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error
from test_framework.wallet import MiniWallet


class CpuMiningTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1 if self.options.testnet4 else 2
        if self.options.testnet4:
            self.chain = "testnet4"
        self.wallet_names = []
        self.uses_wallet = None  # Exercise wallet defaults when compiled, without requiring wallet support.
        self.setup_clean_chain = self.options.fresh or self.options.testnet4
        self.extra_args = [[f"-randomxfast={int(self.options.fast)}"] for _ in range(self.num_nodes)]

    def add_options(self, parser):
        parser.add_argument("--testnet4", action="store_true", help="Check public beta mining from genesis without peers")
        parser.add_argument("--fresh", action="store_true", help="Start at genesis without a cached mock-PoW chain (for real RandomX smoke tests)")
        parser.add_argument("--fast", action="store_true", help="Use FAST RandomX; requires memory for two node datasets")

    def stop_miner(self):
        node = self.nodes[0]
        node.stopmining()
        self.wait_until(lambda: not node.getcpumininginfo()["running"])
        info = node.getcpumininginfo()
        assert_equal(info["state"], "stopped")
        assert_equal(info["error"], "")
        assert_equal(info["hashespersecond"], 0)
        return info

    def run_test(self):
        node = self.nodes[0]
        if self.options.testnet4:
            self.log.info("An isolated testnet4 node can start mining with a testnet reward address")
            address = encode_segwit_address("tcc", 1, bytes.fromhex("79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"))
            assert_equal(node.getcpumininginfo()["running"], False)
            node.startmining(address)
            self.wait_until(lambda: node.getcpumininginfo()["hashes"] > 0)
            assert_equal(self.stop_miner()["address"], address)
            return
        wallet = MiniWallet(node)
        address = wallet.get_address()
        info = node.getcpumininginfo()
        assert_equal(info["running"], False)
        assert_equal(info["hashes"], 0)
        assert_equal(info["threads"], 1)
        assert_equal(info["max_threads"], 1024)
        logical_cpus = info["logical_cpus"]
        assert_greater_than(logical_cpus, 0)
        self.stop_miner()  # Idempotent while stopped.

        self.log.info("Reject invalid parameters without starting workers")
        assert_raises_rpc_error(-18, "specify a reward address", node.startmining)
        assert_raises_rpc_error(-8, "P2PK address", node.startmining, "not-an-address")
        for threads in (0, -1, info["max_threads"] + 1):
            assert_raises_rpc_error(-8, "Threads must be", node.startmining, address, threads)
        assert_equal(node.getcpumininginfo()["running"], False)

        self.log.info("Mine blocks and pay the requested key without a Core wallet (include a signed transaction when using the funded chain)")
        tx = None if self.options.fresh else wallet.send_self_transfer(from_node=node)
        height = node.getblockcount()
        node.startmining(address, min(2, info["max_threads"]))
        assert_raises_rpc_error(-1, "already running", node.startmining, address)
        self.wait_until(lambda: node.getcpumininginfo()["blocks"] >= 2)
        info = self.stop_miner()
        assert_equal(info["max_threads"], 1024)
        assert_equal(info["logical_cpus"], logical_cpus)
        assert_greater_than(info["hashes"], 0)
        assert_greater_than(info["blocks"], 0)
        assert_equal(info["address"], address)
        self.sync_blocks()
        blocks = [node.getblock(node.getblockhash(h), 2) for h in range(height + 1, node.getblockcount() + 1)]
        if tx is not None:
            assert any(tx["txid"] == entry["txid"] for block in blocks for entry in block["tx"])
        coinbases = set()
        for block in blocks:
            coinbase = block["tx"][0]
            assert_equal(coinbase["vout"][0]["scriptPubKey"]["address"], address)
            assert coinbase["txid"] not in coinbases
            coinbases.add(coinbase["txid"])
        assert_equal(node.getcpumininginfo()["hashes"], info["hashes"])

        self.log.info("Pause for known headers ahead, then resume on a newly received block")
        peer = self.nodes[1]
        self.disconnect_nodes(0, 1)
        external = self.generate(peer, 1, sync_fun=lambda: None)[0]
        node.submitheader(peer.getblockheader(external, False))
        # Exercise the inclusive upper bound without allocating 1024 RandomX
        # VMs in CI: a known header ahead prevents hash-worker creation.
        node.startmining(address, 1024)
        self.wait_until(lambda: node.getcpumininginfo()["state"] == "waiting")
        assert_equal(node.getcpumininginfo()["threads"], 1024)
        assert_equal(node.getcpumininginfo()["max_threads"], 1024)
        assert_equal(node.getcpumininginfo()["logical_cpus"], logical_cpus)
        assert_equal(node.getcpumininginfo()["hashes"], 0)
        self.stop_miner()
        node.startmining(address)
        self.wait_until(lambda: node.getcpumininginfo()["state"] == "waiting")
        assert_equal(node.getcpumininginfo()["hashes"], 0)
        assert_equal(node.submitblock(peer.getblock(external, False)), None)
        self.wait_until(lambda: node.getcpumininginfo()["blocks"] >= 1)
        self.stop_miner()
        self.connect_nodes(0, 1)
        self.sync_blocks()

        if self.is_wallet_compiled():
            self.log.info("Default to the selected wallet, but allow an explicit payout with multiple wallets")
            name = "mining wallet"
            node.createwallet(name)
            selected = node.get_wallet_rpc(name)
            for rpc, args in ((node, ()), (selected, ("",))):
                rpc.startmining(*args)
                self.wait_until(lambda: node.getcpumininginfo()["blocks"] >= 1)
                target = self.stop_miner()["address"]
                assert_equal(selected.getaddressinfo(target)["ismine"], True)
                assert_equal(node.getblock(node.getbestblockhash(), 2)["tx"][0]["vout"][0]["scriptPubKey"]["address"], target)
            node.createwallet("second-miner")
            second = node.get_wallet_rpc("second-miner")
            assert_raises_rpc_error(-19, "wallet", node.startmining)
            assert_raises_rpc_error(-18, "wallet", node.get_wallet_rpc("missing").startmining)
            second.startmining()
            self.wait_until(lambda: node.getcpumininginfo()["blocks"] >= 1)
            assert_equal(second.getaddressinfo(self.stop_miner()["address"])["ismine"], True)
            node.startmining(address)
            self.wait_until(lambda: node.getcpumininginfo()["blocks"] >= 1)
            assert_equal(self.stop_miner()["address"], address)

        self.log.info("Node shutdown joins active workers, and mining never auto-starts")
        node.startmining(address)
        self.restart_node(0, extra_args=self.extra_args[0])
        assert_equal(node.getcpumininginfo()["running"], False)
        assert_equal(node.getcpumininginfo()["hashes"], 0)


if __name__ == "__main__":
    CpuMiningTest(__file__).main()
