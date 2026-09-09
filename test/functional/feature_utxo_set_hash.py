#!/usr/bin/env python3
# Copyright (c) 2020-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test UTXO set hash value calculation in gettxoutsetinfo."""

from test_framework.messages import (
    CBlock,
    COutPoint,
    from_hex,
    hash256,
)
from test_framework.crypto.muhash import MuHash3072
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet

class UTXOSetHashTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def test_muhash_implementation(self):
        self.log.info("Test MuHash implementation consistency")

        node = self.nodes[0]
        wallet = MiniWallet(node)
        mocktime = node.getblockheader(node.getblockhash(0))['time'] + 1
        node.setmocktime(mocktime)

        # Generate 100 blocks and remove the first since we plan to spend its
        # coinbase
        block_hashes = self.generate(wallet, 1) + self.generate(node, 99)
        blocks = list(map(lambda block: from_hex(CBlock(), node.getblock(block, False)), block_hashes))
        blocks.pop(0)

        # Create a spending transaction and mine a block which includes it
        txid = wallet.send_self_transfer(from_node=node)['txid']
        tx_block = self.generateblock(node, output=wallet.get_address(), transactions=[txid])
        blocks.append(from_hex(CBlock(), node.getblock(tx_block['hash'], False)))

        genesis = from_hex(CBlock(), node.getblock(node.getblockhash(0), False))
        blocks_with_heights = [(0, genesis)] + list(enumerate(blocks, start=2))

        # Serialize the outputs that should be in the UTXO set and add them to
        # a MuHash object
        muhash = MuHash3072()
        serialized_coins = {}

        for height, block in blocks_with_heights:
            # The spendable genesis output remains, while the first mined
            # coinbase (height 1) was spent above.
            for tx in block.vtx:
                for n, tx_out in enumerate(tx.vout):
                    coinbase = 1 if not tx.vin[0].prevout.hash else 0

                    # Skip witness commitment
                    if (coinbase and n > 0):
                        continue

                    data = COutPoint(tx.txid_int, n).serialize()
                    data += (height * 2 + coinbase).to_bytes(4, "little")
                    data += tx_out.serialize()

                    muhash.insert(data)
                    serialized_coins[(data[:32], n)] = data

        finalized = muhash.digest()
        node_muhash = node.gettxoutsetinfo("muhash")['muhash']

        assert_equal(finalized[::-1].hex(), node_muhash)
        # LevelDB groups transactions by their serialized hash bytes, then
        # Core orders each transaction's outputs by numeric vout index.
        serialized_hash = hash256(b''.join(serialized_coins[key] for key in sorted(serialized_coins)))[::-1].hex()
        assert_equal(serialized_hash, node.gettxoutsetinfo()['hash_serialized_3'])

        self.log.info("Test deterministic UTXO set hash results")
        self.log.info("Independently calculated commitments: %s / %s", serialized_hash, node_muhash)
        assert_equal(serialized_hash, "a544c68fd763c3f2ade7eb325406936fcc94f807fc3e83e7f1474d7b0e867c1b")
        assert_equal(finalized[::-1].hex(), "2146e399748cecf18775e5aa5a71fdbe5be90f86e21d97c24d703566bf5f3a7e")

    def run_test(self):
        self.test_muhash_implementation()


if __name__ == '__main__':
    UTXOSetHashTest(__file__).main()
