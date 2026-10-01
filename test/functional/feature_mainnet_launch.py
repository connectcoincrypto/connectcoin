#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Exercise the launched mainnet, its allocations, and explicit test networks."""

import json
from pathlib import Path
import shutil
import subprocess

from test_framework.messages import hash256
from test_framework.socks5 import AddressType, Socks5Command, start_socks5_server
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port, rpc_port, write_config


MAINNET_GENESIS = "30a3a7543f593b6343873a16aeb61005dce0fe3f4169ab34039316b2a9bb373e"
MAINNET_GENESIS_TXID = "2ff1604a1a6ed04110972a78c808d7b967f8f3d6ee754fcf33bded563edc2c8c"
MAINNET_PUBKEYS = (
    "29a6b41260ed25e3019d18236192afecec9ea0b7312837bacf81a3c40d7b2034",
    "2c83a568529b55cc93e2d20754f079f6072d77ac88266d9b054cb3e0e92a7845",
)
MAINNET_ADDRESSES = (
    "cc1p9xntgynqa5j7xqvarq3kry40ankfag9hxy5r0wk0sx3ugrtmyq6qty6p8u",
    "cc1p9jp626zjnd2ueylz6gr4fure7crj6aav3qnxmxc9fje7p6f20pzsmlvv0j",
)
MAINNET_HEADLINE = b"Cloudflare 01/Oct/2026 Support for modern cryptographic algorithms in Workers"
TESTNET4_GENESIS = "710dc5910cbef40216bd82ccfb66af2273b2b1d336b034c5794966904cb603bf"


class MainnetLaunchTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.chain = ""
        self.num_nodes = 2
        self.wallet_names = []
        self.extra_args = [
            ["-randomxfast=0", "-txindex", "-coinstatsindex"],
            ["-randomxfast=0"],
        ]

    def setup_network(self):
        self.add_nodes(self.num_nodes, self.extra_args)

    def run_test(self):
        self.test_chainparams()
        self.log.info("Implicit and explicit mainnet start on the same launch genesis")
        self.start_node(0)
        self.start_node(1, extra_args=["-chain=main", "-randomxfast=0"])
        self.connect_nodes(0, 1)
        for peer in self.nodes:
            self.check_mainnet_genesis(peer)
        self.check_mainnet_indexes(self.nodes[0])
        self.test_mainnet_reindex()
        self.stop_nodes()

        self.test_default_without_config()
        self.test_explicit_testnet4()
        self.test_wrong_genesis_database()
        self.test_chainstate_tool()
        self.test_testnet4_seeds()
        self.test_regtest_genesis_display()

    def check_mainnet_genesis(self, node):
        assert_equal(node.getblockchaininfo()["chain"], "main")
        assert_equal(node.getblockcount(), 0)
        assert_equal(node.getbestblockhash(), MAINNET_GENESIS)
        block = node.getblock(MAINNET_GENESIS, 2)
        assert_equal(block["time"], 1790872995)
        assert_equal(block["nonce"], 215364)
        assert_equal(block["bits"], "1e333300")
        assert_equal(block["merkleroot"], MAINNET_GENESIS_TXID)
        assert_equal(len(block["tx"]), 1)
        coinbase = block["tx"][0]
        assert_equal(coinbase["txid"], MAINNET_GENESIS_TXID)
        assert MAINNET_HEADLINE in bytes.fromhex(coinbase["vin"][0]["coinbase"])
        assert_equal(len(coinbase["vout"]), 2)
        for index, (output, pubkey) in enumerate(zip(coinbase["vout"], MAINNET_PUBKEYS)):
            assert_equal(output["n"], index)
            assert_equal(output["value"], 5_000_000)
            assert_equal(output["type"], 1)
            assert_equal(output["pubkey"], pubkey)
            assert_equal(output["scriptPubKey"]["hex"], "5120" + pubkey)
            assert_equal(output["scriptPubKey"]["address"], MAINNET_ADDRESSES[index])
            assert_equal(node.validateaddress(MAINNET_ADDRESSES[index])["isvalid"], True)
            coin = node.gettxout(MAINNET_GENESIS_TXID, index)
            assert coin is not None
            assert_equal(coin["value"], 5_000_000)
            assert_equal(coin["confirmations"], 1)
            assert_equal(coin["coinbase"], True)
            assert_equal(coin["scriptPubKey"]["hex"], "5120" + pubkey)
        self.check_genesis_display(node, "mainnet")

    def check_mainnet_indexes(self, node):
        self.log.info("Both launch allocations appear in the UTXO set, coinstatsindex and txindex")
        self.wait_until(lambda: all(node.getindexinfo().get(name, {}).get("synced", False)
                                   for name in ("txindex", "coinstatsindex")))
        for use_index in (False, True):
            stats = node.gettxoutsetinfo(hash_type="muhash", use_index=use_index)
            assert_equal(stats["height"], 0)
            assert_equal(stats["bestblock"], MAINNET_GENESIS)
            assert_equal(stats["txouts"], 2)
            assert_equal(stats["total_amount"], 10_000_000)
            if use_index:
                assert_equal(stats["total_unspendable_amount"], 0)
                assert_equal(stats["block_info"]["coinbase"], 10_000_000)
                assert_equal(stats["block_info"]["unspendables"]["genesis_block"], 0)
            else:
                scanned_muhash = stats["muhash"]
        assert_equal(stats["muhash"], scanned_muhash)
        indexed = node.getrawtransaction(MAINNET_GENESIS_TXID, True)
        assert_equal(indexed["blockhash"], MAINNET_GENESIS)
        assert_equal([output["value"] for output in indexed["vout"]], [5_000_000, 5_000_000])

    def test_mainnet_reindex(self):
        self.log.info("Both mainnet allocations survive full and chainstate-only reindex")
        for flag in ("-reindex", "-reindex-chainstate"):
            self.restart_node(0, extra_args=["-randomxfast=0", "-txindex", "-coinstatsindex", flag])
            self.check_mainnet_genesis(self.nodes[0])
            self.check_mainnet_indexes(self.nodes[0])

    def test_default_without_config(self):
        node = self.nodes[0]
        conf = node.datadir_path / "connectcoin.conf"
        self.log.info("No config file or network flag is needed for the mainnet default")
        saved_conf = conf.with_suffix(".saved")
        conf.rename(saved_conf)
        try:
            self.start_node(0, extra_args=[
                "-server", f"-rpcport={rpc_port(0)}", "-listen=0", "-connect=0",
                "-dnsseed=0", "-fixedseeds=0", "-natpmp=0", "-randomxfast=0",
            ])
            self.check_mainnet_genesis(node)
            assert (node.datadir_path / ".cookie").exists()
            assert not (node.datadir_path / "testnet4" / ".cookie").exists()
            assert not conf.exists()
            if self.is_cli_compiled():
                assert_equal(node.create_new_rpc_connection(mode="CLI").getblockchaininfo()["chain"], "main")
        finally:
            self.stop_node(0)
            saved_conf.rename(conf)

    def test_explicit_testnet4(self):
        self.log.info("Explicit testnet4 remains isolated and retains its genesis")
        for i, node in enumerate(self.nodes):
            write_config(node.datadir_path / "connectcoin.conf", n=i, chain="testnet4")
            node.chain = "testnet4"
            self.start_node(i, extra_args=["-randomxfast=0", f"-bind=127.0.0.1:{p2p_port(i)}"])
        self.connect_nodes(0, 1)
        for peer in self.nodes:
            assert_equal(peer.getblockchaininfo()["chain"], "testnet4")
            assert_equal(peer.getblockcount(), 0)
            assert_equal(peer.getbestblockhash(), TESTNET4_GENESIS)
            for address in MAINNET_ADDRESSES:
                assert_equal(peer.validateaddress(address)["isvalid"], False)
            self.check_genesis_display(peer, "testnet4")
        if self.is_cli_compiled():
            assert_equal(self.nodes[0].create_new_rpc_connection(mode="CLI").getblockchaininfo()["chain"], "testnet4")
        self.stop_nodes()

    def test_wrong_genesis_database(self):
        self.log.info("Mainnet refuses a copied testnet block index instead of replacing its genesis")
        node = self.nodes[0]
        wrong_datadir = node.datadir_path / "wrong-network"
        wrong_datadir.mkdir()
        # Both source and destination are this test's isolated directories;
        # the source node is stopped before copying its LevelDB files.
        shutil.copytree(self.nodes[1].chain_path / "blocks", wrong_datadir / "blocks")
        write_config(wrong_datadir / "connectcoin.conf", n=0, chain="")
        node.assert_start_raises_init_error(
            extra_args=[f"-datadir={wrong_datadir}", "-chain=main", "-randomxfast=0"],
            expected_msg="Error: Incorrect or no genesis block found. Wrong datadir for network?",
        )
        assert (wrong_datadir / "blocks" / "index").is_dir()
        assert not (wrong_datadir / "chainstate").exists()

    def test_chainstate_tool(self):
        if not self.is_connectcoin_chainstate_compiled():
            return
        self.log.info("The standalone chainstate tool initializes mainnet without a network flag")
        tool_datadir = self.nodes[0].datadir_path / "mainnet-tool"
        result = subprocess.run(
            self.get_binaries().chainstate_argv() + [str(tool_datadir)],
            input="", capture_output=True, text=True, timeout=self.rpc_timeout,
        )
        assert_equal(result.returncode, 0)
        assert (tool_datadir / "blocks" / "index").is_dir()
        assert (tool_datadir / "chainstate").is_dir()

    def test_chainparams(self):
        self.utility_chainparams = {}
        if not self.is_connectcoin_util_compiled():
            self.log.info("Skipping utility chain-parameter checks: connectcoin-util is not compiled")
            return

        self.log.info("Compiled utility parameters match every network fixture and the mainnet default")
        # Cross-compiled utilities may be 32-bit even when Python is 64-bit.
        # Derive the expected default from the target compiler, never the output
        # under test, so an incorrect LIGHT/FAST default still fails this check.
        target_pointer_size = self.config.getint("environment", "TARGET_POINTER_SIZE")
        assert target_pointer_size in (4, 8)
        expected_default_mode = "fast" if target_pointer_size == 8 else "light"
        fixture_dir = Path(self.config["environment"]["SRCDIR"]) / "test" / "functional" / "data" / "util"
        cases = (
            ([], "mainnet"),
            (["-chain=main"], "mainnet"),
            (["-regtest"], "regtest"),
            (["-testnet"], "testnet"),
            (["-testnet4"], "testnet4"),
            (["-signet"], "signet"),
            (["-signet", "-signetchallenge=60"], "signet-custom"),
        )
        for arguments, fixture in cases:
            self.log.debug(f"Checking getchainparams {arguments or '[default]'} against {fixture}")
            # The standalone utility reads hardcoded parameters without starting
            # a node or opening a chain/wallet database for any of these networks.
            result = subprocess.run(
                self.get_binaries().util_argv() + arguments + ["getchainparams"],
                capture_output=True, text=True, timeout=self.rpc_timeout,
            )
            assert_equal(result.returncode, 0)
            assert_equal(result.stderr, "")
            expected = json.loads((fixture_dir / f"getchainparams-{fixture}.json").read_text(encoding="utf-8"))
            if "pow" in expected:
                expected["pow"]["randomx_mode"] = expected_default_mode
            actual = json.loads(result.stdout)
            assert_equal(actual, expected)
            self.utility_chainparams[fixture] = actual
            if "pow" in expected:
                # Parameter inspection does not allocate a RandomX dataset or
                # mine a block, including when FAST is requested on 32-bit.
                for flag, mode in ((0, "light"), (1, "fast")):
                    result = subprocess.run(
                        self.get_binaries().util_argv() + arguments + [f"-randomxfast={flag}", "getchainparams"],
                        capture_output=True, text=True, timeout=self.rpc_timeout,
                    )
                    assert_equal(result.returncode, 0)
                    assert_equal(result.stderr, "")
                    mode_expected = {**expected, "pow": {**expected["pow"], "randomx_mode": mode}}
                    assert_equal(json.loads(result.stdout), mode_expected)

    def check_genesis_display(self, node, fixture):
        if not self.is_connectcoin_util_compiled():
            return
        # Do not only compare with another hardcoded fixture: a fixture copied
        # from the utility previously preserved its reversed-byte display bug.
        rpc_hash = node.getblockhash(0)
        header = bytes.fromhex(node.getblockheader(rpc_hash, False))
        assert_equal(self.utility_chainparams[fixture]["genesis"], rpc_hash, hash256(header)[::-1].hex())

    def test_regtest_genesis_display(self):
        if not self.is_connectcoin_util_compiled():
            return
        self.log.info("Regtest utility genesis matches the RPC block ID and independently hashed header")
        # Reuse an already-stopped isolated test node. This never reads a user's
        # regtest data or requires another daemon to run concurrently.
        node = self.nodes[0]
        conf = node.datadir_path / "connectcoin.conf"
        previous_config = conf.read_text(encoding="utf-8")
        previous_chain = node.chain
        try:
            node.chain = "regtest"
            write_config(conf, n=0, chain="regtest")
            self.start_node(0)
            self.check_genesis_display(node, "regtest")
        finally:
            self.stop_node(0)
            node.chain = previous_chain
            conf.write_text(previous_config, encoding="utf-8")

    def test_testnet4_seeds(self):
        self.log.info("Explicit testnet4 uses all its DNS seeds and respects -dnsseed=0")
        seeds = ["connectcoin1.com", "connectcoin2.com", "connectcoin3.com", "dememzea.tplinkdns.com"]
        node = self.nodes[0]
        conf = node.datadir_path / "connectcoin.conf"
        write_config(conf, n=0, chain="testnet4", disable_autoconnect=False)
        node.replace_in_config([("dnsseed=0\n", "")])
        # A fresh addrman makes bootstrap immediate. Only this test's peer cache
        # is removed; no external DNS lookup or network connection is allowed.
        (node.chain_path / "peers.dat").unlink()
        for enabled in (True, False):
            proxy = start_socks5_server(destinations_factory=None)
            try:
                args = [f"-proxy=127.0.0.1:{proxy.conf.addr[1]}", "-v2transport=0", "-randomxfast=0"]
                if not enabled:
                    args.append("-dnsseed=0")
                with node.assert_debug_log(
                    expected_msgs=[f"Loading addresses from DNS seed {seed}" for seed in seeds] if enabled else ["DNS seeding disabled"],
                    unexpected_msgs=[] if enabled else ["Loading addresses from DNS seed"],
                ):
                    self.start_node(0, extra_args=args)
                    if enabled:
                        requested_seeds = []
                        for _ in seeds:
                            request = proxy.queue.get(timeout=self.rpc_timeout)
                            assert isinstance(request, Socks5Command)
                            assert_equal(request.atyp, AddressType.DOMAINNAME)
                            assert_equal(request.port, 48179)
                            requested_seeds.append(request.addr)
                        # Seeds are shuffled; compare the complete list without
                        # relying on order or allowing duplicate/missing names.
                        assert_equal(sorted(requested_seeds), sorted(seed.encode("ascii") for seed in seeds))
                    self.stop_node(0)
                assert proxy.queue.empty()
            finally:
                self.stop_node(0)
                proxy.stop()


if __name__ == "__main__":
    MainnetLaunchTest(__file__).main()
