#!/usr/bin/env python3
# Copyright (c) 2017-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test explicit fallback fees and automatic next-block economic minimum fees."""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import COIN, tx_from_hex
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

HIGH_TX_FEE_PER_KB = Decimal('0.01')


class WalletFallbackFeeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def check_fee(self, fee, tx_hex, rate):
        tx = tx_from_hex(tx_hex)
        # Funding budgets the maximum 65-byte Schnorr signature (including a
        # sighash byte), even when signing selects the 64-byte default form.
        for witness in tx.wit.vtxinwit:
            assert_equal(len(witness.scriptWitness.stack), 1)
            assert len(witness.scriptWitness.stack[0]) in (64, 65)
            witness.scriptWitness.stack[0] = bytes(65)
        rate_connects = int(rate * COIN)
        assert_equal(fee * COIN, (rate_connects * tx.get_vsize() + 999) // 1000)

    def sending_succeeds(self, node, rate, reason="Minimum Required Fee"):
        # This is a wallet policy fallback, not a fabricated estimator result.
        assert "errors" in node.estimatesmartfee(6)
        sent = node.sendtoaddress(node.getnewaddress(), 1, verbose=True)
        assert_equal(sent["fee_reason"], reason)
        entry = node.getmempoolentry(sent["txid"])
        self.check_fee(entry["fees"]["base"], node.gettransaction(sent["txid"])["hex"], rate)
        funded = node.fundrawtransaction(node.createrawtransaction([], {node.getnewaddress(): 1}))
        signed = node.signrawtransactionwithwallet(funded["hex"])
        assert_equal(signed["complete"], True)
        self.check_fee(funded["fee"], signed["hex"], rate)
        assert_equal(node.testmempoolaccept([signed["hex"]])[0]["allowed"], True)
        sent = node.sendmany("", {node.getnewaddress(): 1}, verbose=True)
        assert_equal(sent["fee_reason"], reason)
        entry = node.getmempoolentry(sent["txid"])
        self.check_fee(entry["fees"]["base"], node.gettransaction(sent["txid"])["hex"], rate)

        # sendall previously had a separate "fallback disabled" rejection.
        unspent = node.listunspent(minconf=1)[0]
        swept = node.sendall([node.getnewaddress()], inputs=[unspent], add_to_wallet=False)
        assert_equal(swept["complete"], True)
        decoded = node.decoderawtransaction(swept["hex"])
        fee = unspent["amount"] - sum(out["value"] for out in decoded["vout"])
        self.check_fee(fee, swept["hex"], rate)
        assert_equal(node.testmempoolaccept([swept["hex"]])[0]["allowed"], True)

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, COINBASE_MATURITY + 10)

        # By default, the test framework sets a fallback fee for nodes,
        # in order to test default behavior, comment this line out.
        node.replace_in_config([("fallbackfee=", "spendzeroconfchange=0\n#fallbackfee=")])
        self.restart_node(0)

        # Both unset and explicit zero select the current economic minimum.
        initial_rate = Decimal("0.0001201000")
        assert_equal(node.getnetworkinfo()["relayfee"], initial_rate)
        self.sending_succeeds(node, initial_rate)

        self.restart_node(0, extra_args=["-fallbackfee=0"])
        self.sending_succeeds(node, initial_rate)

        # The fee follows the NEXT block's subsidy and a reorg over a halving.
        self.generate(node, 148 - node.getblockcount())
        self.sending_succeeds(node, initial_rate)
        boundary = self.generate(node, 1)[0]
        halved_rate = Decimal("0.0000601000")
        assert_equal(node.getnetworkinfo()["relayfee"], halved_rate)
        self.sending_succeeds(node, halved_rate)
        node.invalidateblock(boundary)
        self.sending_succeeds(node, initial_rate)
        node.reconsiderblock(boundary)
        self.restart_node(0, extra_args=["-fallbackfee=0"])
        self.sending_succeeds(node, halved_rate)

        # A low operator relay override does not undercut the economic floor.
        self.restart_node(0, extra_args=["-fallbackfee=0", "-minrelaytxfee=0.0000001000"])
        self.sending_succeeds(node, halved_rate)
        higher_rate = Decimal("0.0003000000")
        self.restart_node(0, extra_args=["-fallbackfee=0", f"-mintxfee={higher_rate}"])
        self.sending_succeeds(node, higher_rate)

        # Sending a transaction with a fallback fee set succeeds. Use the
        # largest fallbackfee value that doesn't trigger a warning.
        self.restart_node(0, extra_args=[f"-fallbackfee={HIGH_TX_FEE_PER_KB}"])
        self.sending_succeeds(node, HIGH_TX_FEE_PER_KB, "Fallback fee")
        self.stop_node(0, expected_stderr='')

        # Starting a node with a large fallback fee set...
        excessive_fallback = HIGH_TX_FEE_PER_KB + Decimal('0.0000000001')
        self.start_node(0, extra_args=[f"-fallbackfee={excessive_fallback}"])
        # ...works...
        self.sending_succeeds(node, excessive_fallback, "Fallback fee")
        # ...but results in a warning message.
        expected_error = "Warning: -fallbackfee is set very high! This is the transaction fee you may pay when fee estimates are not available."
        self.stop_node(0, expected_stderr=expected_error)


if __name__ == '__main__':
    WalletFallbackFeeTest(__file__).main()
