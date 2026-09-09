#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Mainnet must not start, including with old data; testnet4 is the beta default."""

import subprocess

from test_framework.socks5 import AddressType, Socks5Command, start_socks5_server
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, rpc_port, write_config


class MainnetDisabledTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.chain = "testnet4"
        self.num_nodes = 2
        self.wallet_names = []
        self.extra_args = [["-prune=550", "-randomxfast=0"] for _ in range(self.num_nodes)]

    def setup_network(self):
        self.add_nodes(self.num_nodes, self.extra_args)

    def run_test(self):
        node = self.nodes[0]
        conf = node.datadir_path / "connectcoin.conf"
        write_config(conf, n=0, chain="")
        node.chain = ""
        error = "Error: Mainnet has not been launched: no genesis block is defined. Use -testnet4 for public testing or -regtest for local testing."
        self.log.info("Explicit mainnet selection must fail")
        node.assert_start_raises_init_error(extra_args=["-chain=main"], expected_msg=error)
        # Argument parsing may create an empty blocks directory, but must not
        # initialize a block index or write any block data.
        assert_equal(list((node.datadir_path / "blocks").iterdir()), [])
        assert not (node.datadir_path / "chainstate").exists()

        # Existing user data must not be opened or wiped, even with -reindex.
        old_index = node.datadir_path / "blocks" / "index"
        old_chainstate = node.datadir_path / "chainstate"
        for directory in (old_index, old_chainstate):
            directory.mkdir(parents=True)
            (directory / "untouched").write_text("old mainnet data", encoding="utf-8")
        node.assert_start_raises_init_error(
            extra_args=["-chain=main", "-reindex", "-minimumchainwork=0", "-assumevalid=0"],
            expected_msg=error,
        )
        for directory in (old_index, old_chainstate):
            sentinel = directory / "untouched"
            assert_equal(sentinel.read_text(encoding="utf-8"), "old mainnet data")
            assert_equal(list(directory.iterdir()), [sentinel])

        if self.is_connectcoin_chainstate_compiled():
            self.log.info("The standalone chainstate tool must also refuse mainnet")
            tool_datadir = node.datadir_path / "unlaunched-tool"
            result = subprocess.run(
                self.get_binaries().chainstate_argv() + [str(tool_datadir)],
                capture_output=True, text=True, timeout=self.rpc_timeout,
            )
            assert_equal(result.returncode, 1)
            assert "Mainnet has not been launched" in result.stderr
            assert not tool_datadir.exists()

        self.log.info("The beta default starts in testnet4 and connects to an explicit testnet4 peer")
        write_config(conf, n=0, chain="testnet4")
        node.replace_in_config([("testnet4=1\n", "")])
        node.chain = "testnet4"
        self.start_nodes()
        self.connect_nodes(0, 1)
        for peer in self.nodes:
            assert_equal(peer.getblockchaininfo()["chain"], "testnet4")
            assert_equal(peer.getblockcount(), 0)
            assert_equal(peer.getbestblockhash(), "06a1a1f822fed4a412aedb19315f1e85c963ad9b3c10e88ff12626b4b1389115")
        for directory in (old_index, old_chainstate):
            assert_equal(list(directory.iterdir()), [directory / "untouched"])

        if self.is_cli_compiled():
            assert_equal(node.create_new_rpc_connection(mode="CLI").getblockchaininfo()["chain"], "testnet4")

        self.log.info("No config file or network flag is needed for the beta default")
        self.stop_node(0)
        saved_conf = conf.with_suffix(".saved")
        conf.rename(saved_conf)
        self.start_node(0, extra_args=[
            "-server", f"-rpcport={rpc_port(0)}", "-listen=0", "-connect=0",
            "-dnsseed=0", "-fixedseeds=0", "-natpmp=0", "-randomxfast=0", "-prune=550",
        ])
        assert_equal(node.getblockchaininfo()["chain"], "testnet4")
        assert (node.datadir_path / "testnet4" / ".cookie").exists()
        assert not (node.datadir_path / ".cookie").exists()
        assert not conf.exists()
        if self.is_cli_compiled():
            assert_equal(node.create_new_rpc_connection(mode="CLI").getblockchaininfo()["chain"], "testnet4")
        self.stop_node(0)
        saved_conf.rename(conf)

        self.test_beta_seed()

    def test_beta_seed(self):
        self.log.info("The beta default uses all its DNS seeds and respects -dnsseed=0")
        seeds = ["connectcoin1.com", "connectcoin2.com", "connectcoin3.com", "dememzea.tplinkdns.com"]
        node = self.nodes[0]
        conf = node.datadir_path / "connectcoin.conf"
        write_config(conf, n=0, chain="testnet4", disable_autoconnect=False)
        node.replace_in_config([("testnet4=1\n", ""), ("dnsseed=0\n", "")])
        # A fresh addrman makes bootstrap immediate. Only this test's peer cache
        # is removed; no external DNS lookup or network connection is allowed.
        (node.chain_path / "peers.dat").unlink()
        for enabled in (True, False):
            proxy = start_socks5_server(destinations_factory=None)
            try:
                args = [f"-proxy=127.0.0.1:{proxy.conf.addr[1]}", "-v2transport=0"]
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
    MainnetDisabledTest(__file__).main()
