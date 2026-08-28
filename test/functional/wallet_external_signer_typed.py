#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Test that external-signer wallets expose only type-1 P2PK outputs."""

import os
import sys

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class ExternalSignerTypedOutputTest(BitcoinTestFramework):
    def signer_path(self, name, *args):
        path = os.path.join(os.path.dirname(os.path.realpath(__file__)), "mocks", name)
        return " ".join([sys.executable, path, *args])

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            [f"-signer={self.signer_path('signer.py')}", "-keypool=10"],
            [],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_external_signer()
        self.skip_if_no_wallet()

    def run_test(self):
        self.nodes[0].createwallet(
            wallet_name="typed_signer",
            disable_private_keys=True,
            external_signer=True,
        )
        wallet = self.nodes[0].get_wallet_rpc("typed_signer")
        address = wallet.getnewaddress(address_type="bech32m")
        assert address.startswith("ccrt1p")

        descriptors = wallet.listdescriptors()["descriptors"]
        assert_equal(len(descriptors), 2)
        assert all(entry["desc"].startswith("tr(") for entry in descriptors)

        self.generate(self.nodes[1], 101)
        funding_txid = self.nodes[1].sendtoaddress(address, 1)
        self.generate(self.nodes[1], 1)
        assert_equal(wallet.gettransaction(funding_txid)["confirmations"], 1)

        destination = self.nodes[1].getnewaddress(address_type="bech32m")
        spend = wallet.send(outputs=[{destination: 0.5}])
        assert spend["complete"]
        assert spend["txid"] in self.nodes[0].getrawmempool()
        self.sync_mempools()
        assert spend["txid"] in self.nodes[1].getrawmempool()

        self.restart_node(0, [
            f"-signer={self.signer_path('legacy_signer.py')}",
            "-keypool=10",
        ])
        assert_raises_rpc_error(
            -1,
            "No compatible external signers found. The signer must advertise protocol 'connectcoin-typed-v1'.",
            self.nodes[0].createwallet,
            wallet_name="incompatible_signer",
            disable_private_keys=True,
            external_signer=True,
        )

        self.restart_node(0, [
            f"-signer={self.signer_path('legacy_signer.py', '--connectcoin-compatible')}",
            "-keypool=10",
        ])
        assert_raises_rpc_error(
            -1,
            "External signer must provide at least one receive and one internal key-path-only tr() or rawtr() type-1 descriptor",
            self.nodes[0].createwallet,
            wallet_name="legacy_signer",
            disable_private_keys=True,
            external_signer=True,
        )


if __name__ == "__main__":
    ExternalSignerTypedOutputTest(__file__).main()
