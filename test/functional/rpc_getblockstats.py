#!/usr/bin/env python3
# Copyright (c) 2017-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Compare every getblockstats field with native typed transactions.

The oracle uses serialized blocks and known previous outputs, not captured
getblockstats responses. Bitcoin Script/OP_RETURN fixtures are not applicable.
"""
from decimal import Decimal
from io import BytesIO

from test_framework.messages import CBlock, COIN, CTxOut
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet import MiniWallet


def median(values):
    if not values:
        return 0
    values = sorted(values)
    middle = len(values) // 2
    return values[middle] if len(values) % 2 else (values[middle - 1] + values[middle]) // 2


class GetblockstatsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.wallet_names = []  # Keep coverage available without a compiled wallet.

    def expected_stats(self, height, blockhash, block, coins, times):
        # The RPC adds a 36-byte outpoint and 4-byte height per UTXO.
        overhead = 40
        fees, sizes, weights, rates = [], [], [], []
        inputs = total_out = output_bytes = spent_bytes = 0
        outputs = sum(len(tx.vout) for tx in block.vtx)
        for index, tx in enumerate(block.vtx):
            spent = [] if index == 0 else [coins.pop((vin.prevout.hash, vin.prevout.n)) for vin in tx.vin]
            spent_bytes += sum(len(out.serialize()) + overhead for out in spent)
            output_bytes += sum(len(out.serialize()) + overhead for out in tx.vout)
            for n, out in enumerate(tx.vout):
                assert out.type in (CTxOut.TYPE_P2PK, CTxOut.TYPE_PAY_TO_CONNECT)
                coins[(tx.txid_int, n)] = out
            if index == 0:
                continue
            inputs += len(tx.vin)
            amount = sum(out.nValue for out in tx.vout)
            total_out += amount
            fees.append(sum(out.nValue for out in spent) - amount)
            sizes.append(len(tx.serialize()))
            weights.append(3 * len(tx.serialize_without_witness()) + len(tx.serialize()))
            rates.append(4 * fees[-1] // weights[-1])

        count = len(fees)
        total_weight = sum(weights)
        percentiles = []
        for percentile in (10, 25, 50, 75, 90):
            cumulative = selected = 0
            for rate, weight in sorted(zip(rates, weights)):
                cumulative += weight
                if cumulative * 100 >= total_weight * percentile:
                    selected = rate
                    break
            percentiles.append(selected)
        witnessed = [i for i, tx in enumerate(block.vtx[1:]) if not tx.wit.is_null()]
        subsidy = 10_000_000 * COIN if height == 0 else (15 * COIN >> (height // 150))
        if height:
            subsidy -= subsidy * min(total_weight, 50_000_000) // 500_000_000
        return {
            "avgfee": sum(fees) // count if count else 0,
            "avgfeerate": 4 * sum(fees) // total_weight if total_weight else 0,
            "avgtxsize": sum(sizes) // count if count else 0,
            "blockhash": blockhash,
            "feerate_percentiles": percentiles,
            "height": height,
            "ins": inputs,
            "maxfee": max(fees, default=0),
            "maxfeerate": max(rates, default=0),
            "maxtxsize": max(sizes, default=0),
            "medianfee": median(fees),
            "mediantime": sorted(times[-11:])[len(times[-11:]) // 2],
            "mediantxsize": median(sizes),
            "minfee": min(fees, default=0),
            "minfeerate": min(rates, default=0),
            "mintxsize": min(sizes, default=0),
            "outs": outputs,
            "subsidy": subsidy,
            "swtotal_size": sum(sizes[i] for i in witnessed),
            "swtotal_weight": sum(weights[i] for i in witnessed),
            "swtxs": len(witnessed),
            "time": block.nTime,
            "total_out": total_out,
            "total_size": sum(sizes),
            "total_weight": total_weight,
            "totalfee": sum(fees),
            "txs": len(block.vtx),
            "utxo_increase": outputs - inputs,
            "utxo_size_inc": output_bytes - spent_bytes,
            # Both native types create UTXOs; Script burns do not exist.
            "utxo_increase_actual": outputs - inputs,
            "utxo_size_inc_actual": output_bytes - spent_bytes,
        }

    def run_test(self):
        node = self.nodes[0]
        wallet = MiniWallet(node)
        self.log.info("Create empty, single-transaction, and mixed typed-output blocks")
        self.generate(wallet, 101)
        single = wallet.send_self_transfer(from_node=node, fee=Decimal("0.000060"))
        self.generate(wallet, 1)
        parent = wallet.send_self_transfer_multi(
            from_node=node, utxos_to_spend=[single["new_utxo"]], num_outputs=3, fee_per_output=300_000)
        wallet.send_self_transfer_multi(
            from_node=node, utxos_to_spend=parent["new_utxos"][:2], num_outputs=4, fee_per_output=400_000)
        bounty = wallet.create_self_transfer(
            utxo_to_spend=parent["new_utxos"][2], fee=Decimal("0.00010"))["tx"]
        bounty.vout[0].nValue -= COIN
        domain = b"stats.example"
        p2c_script = b"\x52" + bytes([len(domain)]) + domain + b"\xff" * 32 + (1).to_bytes(4, "little")
        bounty.vout.append(CTxOut(COIN, p2c_script))
        wallet.sign_tx(bounty, utxos_to_spend=[parent["new_utxos"][2]])
        wallet.sendrawtransaction(from_node=node, tx_hex=bounty.serialize().hex())
        wallet.send_self_transfer(from_node=node, confirmed_only=True, fee=Decimal("0.000080"))
        assert_equal(len(node.getrawmempool()), 4)
        self.generate(wallet, 1)
        assert_equal(node.getrawmempool(), [])

        coins, times, expected = {}, [], {}
        for height in range(104):
            blockhash = node.getblockhash(height)
            block = CBlock()
            block.deserialize(BytesIO(bytes.fromhex(node.getblock(blockhash, 0))))
            times.append(block.nTime)
            stats = self.expected_stats(height, blockhash, block, coins, times)
            if height in (0, 101, 102, 103):
                expected[height] = stats

        self.log.info("Check every statistic, individual selection, and hash/height lookup")
        for height, stats in expected.items():
            assert_equal(node.getblockstats(height), stats)
            assert_equal(node.getblockstats(stats["blockhash"]), stats)
            for name, value in stats.items():
                assert_equal(node.getblockstats(height, [name]), {name: value})
            names = ["minfee", "maxfee", "utxo_size_inc_actual"]
            assert_equal(node.getblockstats(height, names), {name: stats[name] for name in names})
        assert_equal(expected[0]["utxo_size_inc_actual"], 81)
        assert_equal(expected[101]["totalfee"], 0)
        assert_equal(expected[103]["txs"], 5)
        assert expected[103]["subsidy"] < 15 * COIN
        assert expected[103]["maxfeerate"] > expected[103]["minfeerate"]

        self.log.info("Retain invalid-argument and unavailable-block coverage")
        assert_raises_rpc_error(-8, "Target block height 104 after current tip 103", node.getblockstats, 104)
        assert_raises_rpc_error(-8, "Target block height -1 is negative", node.getblockstats, -1)
        for names in (["invalid"], ["minfee", "invalid"], ["invalid", "minfee"], ["minfee", "invalid", "maxfee"]):
            assert_raises_rpc_error(-8, "Invalid selected statistic 'invalid'", node.getblockstats, 101, names)
        assert_raises_rpc_error(-8, "Invalid selected statistic 'aaa'", node.getblockstats, 101, ["invalid", "aaa"])
        assert_raises_rpc_error(-5, "Block not found", node.getblockstats,
                               "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f")
        assert_raises_rpc_error(-1, "getblockstats hash_or_height ( stats )", node.getblockstats, "00", 1, 2)
        assert_raises_rpc_error(-1, "getblockstats hash_or_height ( stats )", node.getblockstats)
        header_only = self.generateblock(node, output=wallet.get_address(), transactions=[], submit=False)
        node.submitheader(header_only["hex"][:160])
        assert_raises_rpc_error(-1, "Block not available (not fully downloaded)", node.getblockstats, header_only["hash"])
        block_file = node.blocks_path / "blk00000.dat"
        backup = node.blocks_path / "blk00000.dat.backup"
        block_file.rename(backup)
        try:
            assert_raises_rpc_error(-1, "Block not found on disk", node.getblockstats, 1)
        finally:
            backup.rename(block_file)


if __name__ == '__main__':
    GetblockstatsTest(__file__).main()
