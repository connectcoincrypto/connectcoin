#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""End-to-end tests for ConnectCoin typed transaction outputs."""

import json
import subprocess

from test_framework.test_framework import BitcoinTestFramework
from test_framework.messages import tx_from_hex
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class TypedOutputsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-fallbackfee=0.00001000"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_connectcoin_tx()

    @staticmethod
    def assert_typed_transaction(decoded):
        for txout in decoded["vout"]:
            assert_equal(txout["type"], 1)
            assert_equal(len(txout["pubkey"]), 64)
        for txin in decoded["vin"]:
            if "coinbase" in txin:
                continue
            assert_equal(txin["scriptSig"]["hex"], "")
            assert_equal(len(txin["txinwitness"]), 1)
            assert_equal(len(txin["txinwitness"][0]), 128)

    def run_test(self):
        node = self.nodes[0]
        address = node.getnewaddress(address_type="bech32m")
        self.generate(node, 101)

        self.log.info("Check type-2 P2C wire decoding and Python framework round-trip")
        target = "00" * 31 + "01"
        p2c_hex = node.createrawtransaction(
            # A dummy input avoids the legacy marker/flags ambiguity of a
            # zero-input transaction in the Python wire decoder.
            inputs=[{"txid": "11" * 32, "vout": 0}],
            outputs=[{"p2c": {
                "amount": 1,
                "domain": "example.com",
                "connection_work_target": target,
                "root_certificates_version": 1,
            }}],
        )
        p2c_tx = tx_from_hex(p2c_hex)
        assert_equal(len(p2c_tx.vout), 1)
        assert_equal(p2c_tx.vout[0].type, 2)
        assert_equal(p2c_tx.vout[0].domain, b"example.com")
        assert_equal(p2c_tx.vout[0].connection_work_target, bytes.fromhex(target)[::-1])
        assert_equal(p2c_tx.vout[0].root_certificates_version, 1)
        assert_equal(p2c_tx.serialize().hex(), p2c_hex)
        decoded_p2c = node.decoderawtransaction(p2c_hex)["vout"][0]
        assert_equal(decoded_p2c["type"], 2)
        assert_equal(decoded_p2c["domain"], "example.com")
        assert_equal(decoded_p2c["connection_work_target"], target)
        assert_equal(decoded_p2c["root_certificates_version"], 1)
        challenge = node.getp2cchallenge(p2c_hex, 0)
        assert_equal(challenge["txid"], node.decoderawtransaction(p2c_hex)["txid"])
        assert_equal(challenge["input_index"], 0)
        assert_equal(len(challenge["clienthello_random"]), 64)
        witnessed_p2c = node.setp2cproof(p2c_hex, 0, "01")
        witnessed_decoded = node.decoderawtransaction(witnessed_p2c)
        assert_equal(witnessed_decoded["txid"], challenge["txid"])
        assert_equal(witnessed_decoded["vin"][0]["txinwitness"], ["01"])

        self.log.info("Check wallet funding and signing preserve a P2C recipient")
        funded_p2c = node.send(
            outputs=[{"p2c": {
                "amount": 1,
                "domain": "example.com",
                "connection_work_target": target,
                "root_certificates_version": 1,
            }}],
            add_to_wallet=False,
        )
        assert_equal(funded_p2c["complete"], True)
        funded_decoded = node.decoderawtransaction(funded_p2c["hex"])
        p2c_outputs = [output for output in funded_decoded["vout"] if output["type"] == 2]
        assert_equal(len(p2c_outputs), 1)
        assert_equal(p2c_outputs[0]["domain"], "example.com")
        assert_equal(node.testmempoolaccept([funded_p2c["hex"]])[0]["allowed"], True)

        self.log.info("Create P2C bounties through the dedicated wallet RPC")

        def get_p2c_output(txid):
            decoded = node.gettransaction(txid=txid, verbose=True)["decoded"]
            outputs = [output for output in decoded["vout"] if output["type"] == 2]
            assert_equal(len(outputs), 1)
            return outputs[0]

        explicit_txid = node.sendtop2c(
            domain="example.com",
            amount=1,
            work={"target": target},
        )
        explicit_output = get_p2c_output(explicit_txid)
        assert_equal(explicit_output["domain"], "example.com")
        assert_equal(explicit_output["connection_work_target"], target)
        assert_equal(explicit_output["root_certificates_version"], 1)

        bits_result = node.cli.sendtop2c(
            domain="example.com",
            amount=1,
            work={"work_bits": 10},
            verbose=True,
        )
        assert "fee_reason" in bits_result
        bits_output = get_p2c_output(bits_result["txid"])
        assert_equal(
            bits_output["connection_work_target"],
            "003f" + "ff" * 30,
        )

        zero_bits_output = get_p2c_output(node.cli.sendtop2c("example.com", 1, {"work_bits": 0}))
        assert_equal(zero_bits_output["connection_work_target"], "ff" * 32)
        full_bits_output = get_p2c_output(node.sendtop2c("example.com", 1, {"work_bits": 256}))
        assert_equal(full_bits_output["connection_work_target"], "00" * 32)

        self.log.info("Reject ambiguous and malformed P2C wallet requests")
        assert_raises_rpc_error(
            -8,
            "exactly one",
            node.sendtop2c,
            "example.com",
            1,
            {"target": target, "work_bits": 10},
        )
        assert_raises_rpc_error(-8, "exactly one", node.sendtop2c, "example.com", 1, {})
        assert_raises_rpc_error(
            -8, "Unknown work field", node.sendtop2c, "example.com", 1, {"difficulty": 10}
        )
        assert_raises_rpc_error(
            -8, "between 0 and 256", node.sendtop2c, "example.com", 1, {"work_bits": -1}
        )
        assert_raises_rpc_error(
            -8, "between 0 and 256", node.sendtop2c, "example.com", 1, {"work_bits": 257}
        )
        assert_raises_rpc_error(
            -8, "32 bytes of hex", node.sendtop2c, "example.com", 1, {"target": "ff"}
        )
        assert_raises_rpc_error(
            -8, "canonical lower-case ASCII", node.sendtop2c, "Example.com", 1, {"work_bits": 10}
        )
        assert_raises_rpc_error(
            -8,
            "unsupported root_certificates_version",
            node.sendtop2c,
            "example.com",
            1,
            {"work_bits": 10},
            2,
        )
        assert_raises_rpc_error(
            -8,
            "unsupported root_certificates_version",
            node.sendtop2c,
            "example.com",
            1,
            {"work_bits": 10},
            -1,
        )
        assert_raises_rpc_error(
            -8,
            "unsupported root_certificates_version",
            node.sendtop2c,
            "example.com",
            1,
            {"work_bits": 10},
            2**32,
        )

        self.log.info("Check wallet signing and typed wire decoding")
        txid = node.sendtoaddress(address=address, amount=1)
        self.assert_typed_transaction(node.getrawtransaction(txid=txid, verbose=True))

        self.log.info("Combine complementary type-1 signatures without the legacy Script merger")
        node.createwallet(wallet_name="combine_a")
        node.createwallet(wallet_name="combine_b")
        default_wallet = node.get_wallet_rpc(self.default_wallet_name)
        wallet_a = node.get_wallet_rpc("combine_a")
        wallet_b = node.get_wallet_rpc("combine_b")
        address_a = wallet_a.getnewaddress(address_type="bech32m")
        address_b = wallet_b.getnewaddress(address_type="bech32m")
        funding_a = default_wallet.sendtoaddress(address_a, 1)
        funding_b = default_wallet.sendtoaddress(address_b, 1)
        self.generate(node, 1)

        def find_output(txid, output_address):
            for output in default_wallet.gettransaction(txid=txid, verbose=True)["decoded"]["vout"]:
                if output["scriptPubKey"].get("address") == output_address:
                    return output["n"]
            raise AssertionError(f"No output for {output_address} in {txid}")

        unsigned = node.createrawtransaction(
            inputs=[
                {"txid": funding_a, "vout": find_output(funding_a, address_a)},
                {"txid": funding_b, "vout": find_output(funding_b, address_b)},
            ],
            outputs={default_wallet.getnewaddress(address_type="bech32m"): 1.99999},
        )
        partial_a = wallet_a.signrawtransactionwithwallet(unsigned)
        partial_b = wallet_b.signrawtransactionwithwallet(unsigned)
        assert_equal(partial_a["complete"], False)
        assert_equal(partial_b["complete"], False)
        combined = node.combinerawtransaction([partial_a["hex"], partial_b["hex"]])
        self.assert_typed_transaction(node.decoderawtransaction(combined))
        assert_equal(node.testmempoolaccept([combined])[0]["allowed"], True)
        node.sendrawtransaction(combined)
        node.unloadwallet("combine_a")
        node.unloadwallet("combine_b")

        self.log.info("Reject type-1 addresses whose witness program is not a valid x-only key")
        invalid_xonly_address = "ccrt1pqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq8ep5f4"
        invalid_info = node.validateaddress(invalid_xonly_address)
        assert_equal(invalid_info["isvalid"], False)
        assert_equal(invalid_info["error"], "Invalid ConnectCoin type-1 x-only public key")
        assert_raises_rpc_error(-5, "Invalid ConnectCoin address", node.createrawtransaction, [], {invalid_xonly_address: 1})
        assert_raises_rpc_error(-5, "Invalid ConnectCoin address", node.sendtoaddress, invalid_xonly_address, 1)
        invalid_tool_output = subprocess.run(
            self.get_binaries().tx_argv() + ["-create", f"outaddr=1:{invalid_xonly_address}"],
            capture_output=True,
            check=False,
            text=True,
        )
        assert_equal(invalid_tool_output.returncode, 1)
        assert "invalid TX output address" in invalid_tool_output.stderr

        self.log.info("Check PSBT finalization uses one 64-byte DEFAULT Schnorr witness")
        funded = node.walletcreatefundedpsbt(inputs=[], outputs=[{address: 1}])
        assert_raises_rpc_error(
            -8,
            "support only SIGHASH_DEFAULT",
            node.walletprocesspsbt,
            psbt=funded["psbt"],
            sign=True,
            sighashtype="ALL",
        )
        processed = node.walletprocesspsbt(psbt=funded["psbt"], sign=True, sighashtype="DEFAULT")
        finalized = node.finalizepsbt(processed["psbt"])
        assert_equal(finalized["complete"], True)
        self.assert_typed_transaction(node.decoderawtransaction(finalized["hex"]))

        self.log.info("Check connectcoin-tx signs a type-1 prevout with its complete digest context")
        prev_txid = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
        xonly_pubkey = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
        private_key = "CqPSjupy1JHzxQ4TF4aSgvZxum5BnUhA2LqRmLiaHHHwXyvr84zk"
        prevtxs = [{
            "txid": prev_txid,
            "vout": 0,
            "amount": "1.0",
            "scriptPubKey": f"5120{xonly_pubkey}",
        }]
        signed = subprocess.run(
            self.get_binaries().tx_argv() + [
                "-create",
                f"in={prev_txid}:0",
                f"set=privatekeys:{json.dumps([private_key], separators=(',', ':'))}",
                f"set=prevtxs:{json.dumps(prevtxs, separators=(',', ':'))}",
                f"outpubkey=0.5:{xonly_pubkey}",
                "sign=DEFAULT",
            ],
            capture_output=True,
            check=False,
            text=True,
        )
        assert_equal(signed.returncode, 0)
        assert_equal(signed.stderr, "")
        self.assert_typed_transaction(node.decoderawtransaction(signed.stdout.strip()))

        self.log.info("Check connectcoin-tx rejects a well-shaped but invalid existing witness")
        invalid_witness_tx = tx_from_hex(signed.stdout.strip())
        invalid_witness_tx.wit.vtxinwit[0].scriptWitness.stack = [b"\x00" * 64]
        rejected = subprocess.run(
            self.get_binaries().tx_argv() + [
                invalid_witness_tx.serialize().hex(),
                "set=privatekeys:[]",
                f"set=prevtxs:{json.dumps(prevtxs, separators=(',', ':'))}",
                "sign=DEFAULT",
            ],
            capture_output=True,
            check=False,
            text=True,
        )
        assert_equal(rejected.returncode, 1)
        assert "sign did not produce a valid type-1 witness" in rejected.stderr

        self.log.info("Check descriptor tooling does not call Script outputs solvable")
        info = node.getdescriptorinfo("raw(51)")
        assert_equal(info["isconnectcoinoutput"], False)
        assert_equal(info["issolvable"], False)

        self.log.info("Check getblocktemplate requires and advertises typed output support")
        assert_raises_rpc_error(
            -8,
            "typedoutputs rule set",
            node.getblocktemplate,
            {"rules": ["segwit"]},
        )
        template = node.getblocktemplate({"rules": ["segwit", "typedoutputs"]})
        assert "!typedoutputs" in template["rules"]
        assert_equal(len(template["coinbaseaux"]["typedoutputs"]), 74)
        assert_equal(template["coinbaseaux"]["typedoutputs"], template["default_witness_commitment"])
        assert_equal(template["witness_commitment_location"], "coinbase_input")


if __name__ == "__main__":
    TypedOutputsTest(__file__).main()
