#!/usr/bin/env python3
# Copyright (c) 2026-present The ConnectCoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the dynamic relay floor, its P2P advertisement, and explicit overrides."""

from decimal import Decimal
import time

from test_framework.messages import COIN
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet


class EconomicRelayFeeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.noban_tx_relay = True
        self.extra_args = [[], [], ["-minrelaytxfee=0.0000001000"]]

    def assert_floor(self, rate):
        for node, expected in zip(self.nodes, [rate, rate, 1000]):
            expected_cc = Decimal(expected) / COIN
            assert_equal(node.getnetworkinfo()["relayfee"], expected_cc)
            info = node.getmempoolinfo()
            assert_equal(info["minrelaytxfee"], expected_cc)
            assert_equal(info["mempoolminfee"], expected_cc)

    def assert_advertised_floor(self, rate):
        # Keep network time fixed: a public subsidy change must refresh the
        # same connection without waiting for a randomized periodic broadcast.
        def received():
            self.peer.sync_with_ping()
            return self.peer.last_message.get("feefilter") is not None and self.peer.last_message["feefilter"].feerate == rate

        self.wait_until(received)

    def check_boundary_relay(self, rate):
        utxo = self.wallet.get_utxo(confirmed_only=True)
        # A single type-1 input/output has a stable 109 vB size. Check both
        # sides of the boundary in indivisible connects, not float fee rates.
        required_fee = (rate * 109 + 999) // 1000
        low = self.wallet.create_self_transfer(fee=Decimal(required_fee - 1) / COIN, utxo_to_spend=utxo)
        result = self.nodes[0].testmempoolaccept([low["hex"]])[0]
        assert_equal(result["allowed"], False)
        assert_equal(result["reject-reason"], "min relay fee not met")
        exact = self.wallet.create_self_transfer(fee=Decimal(required_fee) / COIN, utxo_to_spend=utxo)
        assert_equal(self.nodes[0].testmempoolaccept([exact["hex"]])[0]["allowed"], True)
        self.wallet.sendrawtransaction(from_node=self.nodes[0], tx_hex=exact["hex"])
        # No direct submission to node1: an excessive advertised feefilter
        # would strand this valid transaction on node0.
        self.sync_mempools()
        assert_equal(self.nodes[1].getrawmempool(), [exact["txid"]])

    def run_test(self):
        self.mocktime = int(time.time())
        for node in self.nodes:
            node.setmocktime(self.mocktime)
        self.wallet = MiniWallet(self.nodes[0])
        self.generate(self.wallet, 101)
        self.peer = self.nodes[0].add_p2p_connection(P2PInterface())

        self.log.info("Initial floor is advertised exactly and transactions at it relay")
        self.assert_floor(1_201_000)
        self.assert_advertised_floor(1_201_000)
        self.check_boundary_relay(1_201_000)
        self.generate(self.wallet, 47)
        self.assert_floor(1_201_000)  # tip 148, next block still before halving

        self.log.info("The next-block floor falls at tip 149, not tip 150")
        boundary = self.generate(self.wallet, 1)[0]
        self.assert_floor(601_000)
        self.assert_advertised_floor(601_000)

        self.log.info("A reorg back across the boundary restores the higher floor")
        for node in self.nodes:
            node.invalidateblock(boundary)
        self.sync_blocks()
        self.assert_floor(1_201_000)
        self.assert_advertised_floor(1_201_000)
        for node in self.nodes:
            node.reconsiderblock(boundary)
        self.sync_blocks()
        self.assert_floor(601_000)
        self.assert_advertised_floor(601_000)
        self.check_boundary_relay(601_000)

        self.log.info("Further halvings and a restart preserve the effective floor")
        self.generate(self.wallet, 150)
        self.assert_floor(301_000)
        self.assert_advertised_floor(301_000)
        self.restart_node(0)
        self.connect_nodes(0, 1)
        self.assert_floor(301_000)
        self.peer = self.nodes[0].add_p2p_connection(P2PInterface())
        self.assert_advertised_floor(301_000)
        self.check_boundary_relay(301_000)

        self.log.info("An incremental floor is respected; an explicit relay override stays fixed")
        self.restart_node(0, extra_args=["-incrementalrelayfee=0.0002000000"])
        assert_equal(self.nodes[0].getnetworkinfo()["relayfee"], Decimal("0.0002000000"))
        self.restart_node(0, extra_args=["-incrementalrelayfee=0.0002000000", "-minrelaytxfee=0.0000001000"])
        assert_equal(self.nodes[0].getnetworkinfo()["relayfee"], Decimal("0.0000001000"))


if __name__ == '__main__':
    EconomicRelayFeeTest(__file__).main()
