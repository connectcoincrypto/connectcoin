#!/usr/bin/env python3
# Copyright (c) 2022-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test signet miner tool"""

import os.path
import shlex
import subprocess
import sys
import time

from test_framework.blocktools import DIFF_1_N_BITS, SIGNET_HEADER
from test_framework.script import CScript
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


def get_coinbase_metadata(node):
    coinbase = node.getblock(node.getbestblockhash(), 2)['tx'][0]
    return coinbase['vin'][0]['coinbase']

def get_signet_commitment(coinbase_metadata):
    for el in CScript.fromhex(coinbase_metadata):
        if isinstance(el, bytes) and el[0:4] == SIGNET_HEADER:
            return el[4:].hex()
    return None

class SignetMinerTest(BitcoinTestFramework):
    def set_test_params(self):
        self.chain = "signet"
        self.setup_clean_chain = True
        self.num_nodes = 3

        self.extra_args = [
            ["-signetchallenge=51"], # OP_TRUE
            ["-signetchallenge=60"], # OP_16
            ["-signetchallenge=202cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"], # sha256("hello")
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_cli()
        self.skip_if_no_wallet()
        self.skip_if_no_connectcoin_util()

    def setup_network(self):
        self.setup_nodes()
        # Nodes with different signet networks are not connected

    # generate block with signet miner tool
    def mine_block(self, node, *, expect_success=True):
        n_blocks = node.getblockcount()
        base_dir = self.config["environment"]["SRCDIR"]
        signet_miner_path = os.path.join(base_dir, "contrib", "signet", "miner")
        rpc_argv = node.binaries.rpc_argv() + [f"-datadir={node.datadir_path}"]
        util_argv = node.binaries.util_argv() + ["grind"]
        result = subprocess.run([
                sys.executable,
                signet_miner_path,
                f'--cli={shlex.join(rpc_argv)}',
                'generate',
                f'--address={node.getnewaddress()}',
                f'--grind-cmd={shlex.join(util_argv)}',
                f'--nbits={DIFF_1_N_BITS:08x}',
                f'--set-block-time={int(time.time())}',
                '--poolnum=99',
            ], check=False, stderr=subprocess.STDOUT)
        assert_equal(result.returncode == 0, expect_success)
        assert_equal(node.getblockcount(), n_blocks + int(expect_success))

    def run_test(self):
        self.log.info("Mine the ConnectCoin default trivial OP_TRUE signet")
        node = self.nodes[0]
        self.mine_block(node)
        # Trivial BIP325 challenges omit the optional signet solution. The
        # witness commitment remains present in the coinbase input metadata.
        metadata = get_coinbase_metadata(node)
        assert bytes.fromhex("24aa21a9ed") in bytes.fromhex(metadata)
        assert get_signet_commitment(metadata) is None

        node = self.nodes[1]
        self.log.info("Signet node with trivial challenge (OP_16)")
        self.mine_block(node)
        assert get_signet_commitment(get_coinbase_metadata(node)) is None

        node = self.nodes[2]
        self.log.info("Signet node with trivial challenge (push sha256 hash)")
        self.mine_block(node)
        assert get_signet_commitment(get_coinbase_metadata(node)) is None


if __name__ == "__main__":
    SignetMinerTest(__file__).main()
