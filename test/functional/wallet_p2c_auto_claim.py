#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Automatic-claim lifecycle; optional live TLS smoke test is never enabled in CI."""

from concurrent.futures import ThreadPoolExecutor
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class P2CAutoClaimTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-fallbackfee=0"]]

    def add_options(self, parser):
        parser.add_argument("--live-domain", help="Explicit opt-in to a real public TLS server; not used in CI")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        if self.options.live_domain:
            self.mocktime = int(time.time())
            node.setmocktime(self.mocktime)
        self.generate(node, 101)
        funder = node.get_wallet_rpc(self.default_wallet_name)
        node.createwallet("auto-claimant")
        claimant = node.get_wallet_rpc("auto-claimant")
        initial_status = claimant.getp2cclaimstatus()
        assert_equal(initial_status["connections_per_second"], 0)
        assert_equal(initial_status["concurrency"], 1000)
        assert_equal(initial_status["reward_address"], "")
        # Omitting concurrency must apply the same default as a fresh wallet,
        # including when disabling claims. This does not start HTTPS.
        assert_equal(claimant.setp2cclaiming(0)["concurrency"], 1000)
        assert_equal(claimant.setp2cclaiming(0, None)["concurrency"], 1000)
        assert "domain_round_seconds" not in initial_status
        for rate, concurrency in ((-2, 1), (1, 0), (1, -1)):
            assert_raises_rpc_error(-4, "Use rate", claimant.setp2cclaiming, rate, concurrency)
        assert_raises_rpc_error(-4, "Invalid P2C domain", claimant.setp2cclaiming, 1, 1, ["UPPER.example"])
        node.createwallet("watch-auto", disable_private_keys=True)
        assert_raises_rpc_error(-4, "requires local wallet keys", node.get_wallet_rpc("watch-auto").setp2cclaiming, 1)

        external = funder.getnewaddress()
        for rpc in (claimant, node.get_wallet_rpc("watch-auto")):
            progress = rpc.setp2cclaiming(1, 2, ["never-funded.invalid"], external)
            assert_equal(progress["reward_address"], external)
            self.wait_until(lambda: rpc.getp2cclaimstatus()["state"] == "waiting for bounties")
            assert_raises_rpc_error(-4, "P2PK reward address", rpc.setp2cclaiming, 1, 2, [], "invalid")
            assert_equal(rpc.getp2cclaimstatus()["reward_address"], external)
            assert_equal(rpc.getp2cclaimstatus()["connections_per_second"], 1)
            assert_equal(rpc.setp2cclaiming(0)["reward_address"], "")
            assert_equal(rpc.getp2cclaimstatus()["attempts"], 0)

        # No bounty matches this filter: the normal test never resolves a name
        # or creates a network connection, even with an unlimited rate.
        for rate in (1, -1):
            claimant.setp2cclaiming(rate, 2, ["never-funded.invalid"])
            self.wait_until(lambda: claimant.getp2cclaimstatus()["state"] in ("waiting for bounties", "scanning confirmed bounties"))
            assert_equal(claimant.setp2cclaiming(0)["connections_per_second"], 0)
            assert_equal(claimant.getp2cclaimstatus()["attempts"], 0)

        # Accept the full positive RPC integer range, not an arbitrary cap of
        # 64. An unmatched filter ensures no threads or connections are started.
        for concurrency in (65, 128, 1000, 2**31 - 1):
            progress = claimant.setp2cclaiming(1, concurrency, ["never-funded.invalid"])
            assert_equal(progress["concurrency"], concurrency)
            self.wait_until(lambda: claimant.getp2cclaimstatus()["state"] == "waiting for bounties")
            assert_equal(claimant.getp2cclaimstatus()["attempts"], 0)
            claimant.setp2cclaiming(0)

        self.log.info("Serialize concurrent reconfigurations and stop before unloading")
        def configure(rate):
            # Wallet proxies share their parent's connection. CLI processes
            # give each concurrent request a genuinely separate connection.
            rpc = node.cli("-rpcwallet=auto-claimant")
            return rpc.setp2cclaiming(rate, 2, ["never-funded.invalid"])
        with ThreadPoolExecutor(max_workers=2) as executor:
            list(executor.map(configure, [1, 0, -1, 0]))
        claimant.setp2cclaiming(1, 2, ["never-funded.invalid"])
        node.unloadwallet("auto-claimant")
        node.loadwallet("auto-claimant")
        assert_equal(claimant.getp2cclaimstatus()["connections_per_second"], 0)
        assert_equal(claimant.getp2cclaimstatus()["concurrency"], 1000)
        claimant.setp2cclaiming(1, 2, ["never-funded.invalid"])
        claimant.setp2cclaiming(0)
        assert_equal(claimant.getp2cclaimstatus()["attempts"], 0)

        self.log.info("Ignore an unprofitable confirmed bounty, including a zero work target")
        funder.sendtop2c("unprofitable.invalid", 1, {"work_bits": 256}, fee_rate=10000)
        self.generate(node, 1)
        claimant.setp2cclaiming(-1, 1, ["unprofitable.invalid"])
        try:
            self.wait_until(lambda: claimant.getp2cclaimstatus()["state"] == "waiting for eligible bounties")
            assert_equal(claimant.getp2cclaimstatus()["attempts"], 0)
        finally:
            claimant.setp2cclaiming(0)

        self.log.info("Only discover the last 600 blocks; older bounties remain manually claimable")
        old = funder.sendtop2c("old-bounty.invalid", 1, {"work_bits": 0}, fee_rate=10000)
        self.generate(node, 1)
        old_txid = old["txids"][0]
        old_tx = funder.gettransaction(old_txid, verbose=True)["decoded"]
        old_vout = next(output["n"] for output in old_tx["vout"] if output.get("type") == 2)
        # 600 confirmations is still in the inclusive discovery window. Lock
        # it so this boundary check cannot issue DNS/TLS, even if regressed.
        funder.lockunspent(False, [{"txid": old_txid, "vout": old_vout}])
        # Real RandomX mining can exceed the per-RPC timeout for one 599-block
        # request. Keep the same chain boundary while bounding each request.
        for generated in range(0, 599, 25):
            self.generate(node, min(25, 599 - generated))
        funder.setp2cclaiming(-1, 1, ["old-bounty.invalid"])
        try:
            self.wait_until(lambda: funder.getp2cclaimstatus()["state"] == "waiting for eligible bounties")
            assert_equal(funder.getp2cclaimstatus()["attempts"], 0)
            # At 601 confirmations it is absent from discovery, rather than
            # individually looked up and rejected by wallet eligibility.
            newest = self.generate(node, 1)[0]
            self.wait_until(lambda: funder.getp2cclaimstatus()["state"] == "waiting for bounties")
            node.invalidateblock(newest)
            self.wait_until(lambda: funder.getp2cclaimstatus()["state"] == "waiting for eligible bounties")
        finally:
            funder.setp2cclaiming(0)
            funder.lockunspent(True, [{"txid": old_txid, "vout": old_vout}])
        # This cutoff is not a new consensus rule or a manual-claim restriction.
        node.reconsiderblock(newest)
        assert claimant.preparep2cclaim(old_txid, old_vout)["hex"]

        if self.options.live_domain:
            self.log.info("Explicit live TLS test: generate proof, auto-submit, then mine the claim on regtest")
            funded = funder.sendtop2c(self.options.live_domain, 1, {"work_bits": 0}, fee_rate=10000, output_count=3)
            self.generate(node, 1)
            claimant.setp2cclaiming(1, 3, [self.options.live_domain])
            try:
                def result_ready():
                    progress = claimant.getp2cclaimstatus()
                    return progress["submitted"] >= 3 or bool(progress["last_error"])
                self.wait_until(result_ready, timeout=45)
            finally:
                status = claimant.getp2cclaimstatus()
                claimant.setp2cclaiming(0)
                self.log.info("Final live status: %s", status)
            assert_equal(status["submitted"], 3)
            assert status["domain_rounds"] >= 3
            txid = status["last_txid"]
            tx = claimant.gettransaction(txid, verbose=True)["decoded"]
            assert_equal(tx["vin"][0]["txid"], funded["txids"][0])
            assert_equal(len(tx["vin"]), 1)
            assert_equal(len(tx["vout"]), 1)
            assert txid in node.getrawmempool()
            assert_equal(len(node.getrawmempool()), 3)
            self.generate(node, 1)
            assert claimant.getbalance() > 0
            assert_equal(claimant.gettransaction(txid)["confirmations"], 1)


if __name__ == "__main__":
    P2CAutoClaimTest(__file__).main()
