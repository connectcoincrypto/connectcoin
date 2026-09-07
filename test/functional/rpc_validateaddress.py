#!/usr/bin/env python3
# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test validateaddress over RPC on a launched network.

The 35 mainnet address/error vectors are preserved in the C++ rpc_tests suite,
using src/test/data/validateaddress_main.json and a test-only chain fixture.
"""

from test_framework.address import (
    create_deterministic_address_ccrt1_p2pk,
    output_key_to_p2tr,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class ValidateAddressTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def run_test(self):
        address, pubkey = create_deterministic_address_ccrt1_p2pk()
        for encoded in (address, address.upper()):
            info = self.nodes[0].validateaddress(encoded)
            assert_equal(info["isvalid"], True)
            assert_equal(info["scriptPubKey"], "5120" + pubkey.hex())
            assert "error" not in info
            assert "error_locations" not in info

        main_address = output_key_to_p2tr(pubkey, main=True)
        for invalid in (main_address, "", "not-an-address", address[:-1], address[:-1] + ("q" if address[-1] != "q" else "p")):
            info = self.nodes[0].validateaddress(invalid)
            assert_equal(info["isvalid"], False)
            assert "error" in info
            assert "error_locations" in info


if __name__ == "__main__":
    ValidateAddressTest(__file__).main()
