#!/usr/bin/env python3
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the generation of UTXO snapshots using `dumptxoutset`.
"""

import os

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.compressor import compress_amount
from test_framework.messages import CBlock, COutPoint, MAGIC_BYTES, from_hex, hash256, ser_varint
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    sha256sum_file,
)


class DumptxoutsetTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def check_snapshot_contents(self, out, path):
        # Build both commitments independently from the actual block outputs.
        # This fixture has exactly one unspent coinbase output per block,
        # including ConnectCoin's spendable genesis allocation.
        coins = {}
        for height in range(out['base_height'] + 1):
            block = from_hex(CBlock(), self.nodes[0].getblock(self.nodes[0].getblockhash(height), False))
            assert_equal(len(block.vtx), 1)
            tx = block.vtx[0]
            assert_equal(len(tx.vout), 1)
            output = tx.vout[0]
            outpoint = COutPoint(tx.txid_int, 0).serialize()
            code = height * 2 + 1
            coins[outpoint[:32]] = (
                outpoint + code.to_bytes(4, 'little') + output.serialize(),
                b'\x01\x00' + ser_varint(code) + ser_varint(compress_amount(output.nValue)) + output.serialize()[8:],
            )
        header = b'utxo\xff\x02\x00' + MAGIC_BYTES[self.chain]
        header += bytes.fromhex(out['base_hash'])[::-1] + len(coins).to_bytes(8, 'little')
        expected_snapshot = header + b''.join(txid + coins[txid][1] for txid in sorted(coins))
        assert_equal(path.read_bytes(), expected_snapshot)
        commitment = hash256(b''.join(coins[txid][0] for txid in sorted(coins)))[::-1].hex()
        assert_equal(out['txoutset_hash'], commitment)
        self.log.info("Independent snapshot: base=%s file=%s UTXOs=%s",
                      out['base_hash'], sha256sum_file(str(path)).hex(), commitment)

    def test_dumptxoutset_with_fork(self):
        node = self.nodes[0]
        tip = node.getbestblockhash()
        target_height = node.getblockcount() - 10
        target_hash = node.getblockhash(target_height)

        # Create a fork of two blocks at the target height
        invalid_block = node.getblockhash(target_height + 1)
        node.invalidateblock(invalid_block)
        # Reset mocktime to not regenerate the same blockhash
        node.setmocktime(0)
        self.generate(node, 2)

        # Move back on to actual main chain
        node.reconsiderblock(invalid_block)
        self.wait_until(lambda: node.getbestblockhash() == tip)

        # Use dumptxoutset at the forked height
        out = node.dumptxoutset("txoutset_fork.dat", "rollback", {"rollback": target_height})

        # Verify the snapshot was created at the target height and not the fork tip
        assert_equal(out['base_height'], target_height)
        assert_equal(out['base_hash'], target_hash)

        # Cover the same case as above with an in-memory database
        out_mem = node.dumptxoutset("txoutset_fork_mem.dat", "rollback", {"rollback": target_height, "in_memory": True})
        assert_equal(out_mem['base_height'], target_height)
        assert_equal(out_mem['base_hash'], target_hash)


    def run_test(self):
        """Test a trivial usage of the dumptxoutset RPC command."""
        node = self.nodes[0]
        mocktime = node.getblockheader(node.getblockhash(0))['time'] + 1
        node.setmocktime(mocktime)
        self.generate(node, COINBASE_MATURITY)

        FILENAME = 'txoutset.dat'
        out = node.dumptxoutset(FILENAME, "latest")
        expected_path = node.chain_path / FILENAME

        assert expected_path.is_file()

        assert_equal(out['coins_written'], 101)
        assert_equal(out['base_height'], 100)
        assert_equal(out['path'], str(expected_path))
        self.check_snapshot_contents(out, expected_path)
        # Blockhash should be deterministic based on mocked time and the PoW
        # implementation selected by the test environment.
        expected_base_hash = (
            '69a2fe82bde808f7c11562a800db81637a9b3dec6e746d692a160d228d60e129'
            if os.getenv('TEST_RANDOMX_MOCK_POW') is not None else
            'd8b7fb434815eb67fba4b0d6ea3c1f44a33ad80529a6e50867b1aef4cd530117'
        )
        assert_equal(
            out['base_hash'],
            expected_base_hash)

        # The snapshot includes the base block hash, so its file hash also
        # differs when the mock PoW implementation is active.
        expected_snapshot_hash = (
            'd99350f80286bf51846d3c51ee1f6c3951a2fb3176ecd27fb26d0f1be4b78c28'
            if os.getenv('TEST_RANDOMX_MOCK_POW') is not None else
            'f234b084392a334a06d3dd8a667e6d66ebcd9763f3a9a7e90d18de00d266b0bd'
        )
        assert_equal(
            sha256sum_file(str(expected_path)).hex(),
            expected_snapshot_hash)

        assert_equal(
            out['txoutset_hash'], 'efaf21d989d89cc0831e6c645f076ca104d2ce56d0f6979e1353c0351fd0f90f')
        assert_equal(out['nchaintx'], 101)

        # Specifying a path to an existing or invalid file will fail.
        assert_raises_rpc_error(
            -8, '{} already exists'.format(FILENAME),  node.dumptxoutset, FILENAME, "latest")
        invalid_path = node.datadir_path / "invalid" / "path"
        assert_raises_rpc_error(
            -8, "Couldn't open file {}.incomplete for writing".format(invalid_path), node.dumptxoutset, invalid_path, "latest")

        self.log.info("Test that dumptxoutset with unknown dump type fails")
        assert_raises_rpc_error(
            -8, 'Invalid snapshot type "bogus" specified. Please specify "rollback" or "latest"', node.dumptxoutset, 'utxos.dat', "bogus")

        self.log.info("Testing dumptxoutset with chain fork at target height")
        self.test_dumptxoutset_with_fork()


if __name__ == '__main__':
    DumptxoutsetTest(__file__).main()
