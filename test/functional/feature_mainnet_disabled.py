#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Mainnet must not start, including with old data; testnet4 remains usable."""

import subprocess

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, write_config


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
        self.log.info("Default and explicit mainnet selection must fail")
        for args in ([], ["-chain=main"]):
            node.assert_start_raises_init_error(extra_args=args, expected_msg=error)
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

        self.log.info("The public test network still starts and connects peers")
        write_config(conf, n=0, chain="testnet4")
        node.chain = "testnet4"
        self.start_nodes()
        self.connect_nodes(0, 1)
        for peer in self.nodes:
            assert_equal(peer.getblockchaininfo()["chain"], "testnet4")
            assert_equal(peer.getblockcount(), 0)
            assert_equal(peer.getbestblockhash(), "d607fe5b7f8e498c08f34c740a3ba75af44eace9e9d9a1cbfc163cfa6ad16519")


if __name__ == "__main__":
    MainnetDisabledTest(__file__).main()
