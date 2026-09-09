#!/usr/bin/env python3
# Copyright 2014 BitPay Inc.
# Copyright 2016-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.
"""Exercise the utils via json-defined tests."""

import difflib
import io
import json
import os
import subprocess
from decimal import Decimal
from pathlib import Path

from test_framework.address import byte_to_base58
from test_framework.key import compute_xonly_pubkey, verify_schnorr
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut
from test_framework.script import SIGHASH_DEFAULT, TaprootSignatureHash
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class ToolUtils(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 0  # No node/datadir needed

    def setup_network(self):
        pass

    def skip_test_if_missing_module(self):
        self.skip_if_no_connectcoin_tx()
        self.skip_if_no_connectcoin_util()

    def run_test(self):
        # Use the compiler target, not the architecture of the Python runner.
        self.target_pointer_size = self.config.getint("environment", "TARGET_POINTER_SIZE")
        assert self.target_pointer_size in (4, 8)
        self.testcase_dir = Path(self.config["environment"]["SRCDIR"]) / "test" / "functional" / "data" / "util"
        self.bins = self.get_binaries()
        with open(self.testcase_dir / "connectcoin-util-test.json") as f:
            input_data = json.loads(f.read())

        for i, test_obj in enumerate(input_data):
            self.log.debug(f"Running [{i}]: " + test_obj["description"])
            self.test_one(test_obj)
        self.log.info(f"Passed {len(input_data)} utility fixtures; checking typed-output semantics")
        self.test_typed_outputs()

    def assert_model(self, args, tx):
        """Compare CLI output to Python serialization, not another CLI snapshot."""
        expected = tx.serialize_with_witness().hex()
        result = subprocess.run(self.bins.tx_argv() + args, capture_output=True, text=True, timeout=60)
        assert_equal(result.returncode, 0)
        assert_equal(result.stderr, "")
        assert_equal(result.stdout, expected + "\n")
        decoded = subprocess.run(self.bins.tx_argv() + ["-json"] + args, capture_output=True, text=True, timeout=60)
        assert_equal(decoded.returncode, 0)
        assert_equal(decoded.stderr, "")
        data = json.loads(decoded.stdout, parse_float=Decimal)
        assert_equal(data["hex"], expected)
        assert_equal(data["txid"], tx.txid_hex)
        assert_equal(data["hash"], tx.wtxid_hex)
        assert_equal(data["weight"], tx.get_weight())
        assert_equal([out["type"] for out in data["vout"]], [out.type for out in tx.vout])
        assert_equal([out["value"] for out in data["vout"]], [Decimal(out.nValue) / COIN for out in tx.vout])

    def test_typed_outputs(self):
        pubkey = compute_xonly_pubkey((1).to_bytes(32, "big"))[0]
        target = "00" * 31 + "01"
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(1, 0), b"", 0xffffffff)]
        p2pk = CTxOut(1, b"\x51\x20" + pubkey)
        domain = b"example.com"
        p2c = CTxOut(2 * COIN, b"\x52" + bytes([len(domain)]) + domain + bytes.fromhex(target)[::-1] + b"\x01\x00\x00\x00\x07")
        tx.vout = [p2pk, p2c]
        args = ["-create", f"in={1:064x}:0", f"outpubkey=0.0000000001:{pubkey.hex()}", f"outp2c=2:example.com:{target}:1"]
        self.assert_model(args, tx)
        self.assert_model([tx.serialize_without_witness().hex()], tx)
        for mask in range(1, 8):
            tx.vout[1] = CTxOut(2 * COIN, p2c.scriptPubKey[:-1] + bytes([mask]))
            self.assert_model(args[:-1] + [args[-1] + f":{mask}"], tx)
        tx.vout[1] = p2c

        # Attaching a witness is intentionally not full TLS validation. It must
        # preserve the txid/challenge and replace (not append to) its one item.
        unwitnessed = tx.serialize_without_witness().hex()
        original_txid = tx.txid_hex
        tx.wit.vtxinwit = [CTxInWitness()]
        tx.wit.vtxinwit[0].scriptWitness.stack = [b"\x02\xaa\x55"]
        self.assert_model([unwitnessed, "p2cproof=0:02aa55"], tx)
        tx.wit.vtxinwit[0].scriptWitness.stack = [b"\x02\x11"]
        self.assert_model([unwitnessed, "p2cproof=0:02aa55", "p2cproof=0:0211"], tx)
        assert_equal(tx.txid_hex, original_txid)
        assert tx.wtxid_hex != original_txid

        errors = [
            (f"outp2c=2:example.com:{target}", "P2C output must be VALUE:DOMAIN:TARGET:ROOTS_VERSION"),
            (f"outp2c=2:Example.com:{target}:1", "P2C domain must be canonical"),
            (f"outp2c=2:example.com.:{target}:1", "P2C domain must be canonical"),
            ("outp2c=2:example.com:01:1", "P2C target must be exactly 32 bytes of hex"),
            (f"outp2c=2:example.com:{target}:0", "unsupported P2C root certificate version"),
            (f"outp2c=2:example.com:{target}:2", "unsupported P2C root certificate version"),
            *((f"outp2c=2:example.com:{target}:1:{mask}", "P2C signature algorithms mask must be between 1 and 7")
              for mask in (-1, 0, 8, 128, 255, 256)),
            ("p2cproof=0", "P2C proof must be INPUT_INDEX:PROOF"),
            ("p2cproof=1:02", "invalid P2C proof input index"),
            ("p2cproof=0:", "P2C proof must be non-empty hexadecimal data"),
            ("p2cproof=0:xyz", "P2C proof must be non-empty hexadecimal data"),
            ("p2cproof=0:0", "P2C proof must be non-empty hexadecimal data"),
            ("outpubkey=0:" + "ff" * 32, "invalid TX output x-only pubkey"),
            (f"outpubkey=0.00000000001:{pubkey.hex()}", "invalid TX output value"),
            ("sign=ALL", "unknown sighash flag/sign option"),
        ]
        for command, error in errors:
            self.test_one({"exec": "./connectcoin-tx", "args": [unwitnessed, command], "return_code": 1, "error_txt": error})

        # Signing must have all input amounts/keys, not legacy P2PKH prevouts.
        secret = (1).to_bytes(32, "big")  # Public, deterministic test key only.
        key_arg = "set=privatekeys:" + json.dumps([byte_to_base58(secret + b"\x01", 178)])
        prevout = {"txid": f"{1:064x}", "vout": 0, "scriptPubKey": p2pk.scriptPubKey.hex(), "amount": "3"}
        signing = [unwitnessed, key_arg, "set=prevtxs:" + json.dumps([prevout]), "sign=DEFAULT"]
        signed = subprocess.run(self.bins.tx_argv() + signing, capture_output=True, text=True, timeout=60)
        assert_equal(signed.returncode, 0)
        assert_equal(signed.stderr, "")
        self.assert_signatures(signing, signed.stdout)
        incomplete = [unwitnessed, f"in={2:064x}:1", key_arg, "set=prevtxs:" + json.dumps([prevout]), "sign=DEFAULT"]
        self.test_one({"exec": "./connectcoin-tx", "args": incomplete, "return_code": 1, "error_txt": "prevtxs must contain every transaction input"})
        second_prevout = dict(prevout, txid=f"{2:064x}", vout=1, amount="4")
        complete = [unwitnessed, f"in={2:064x}:1", key_arg, "set=prevtxs:" + json.dumps([prevout, second_prevout]), "sign=DEFAULT"]
        signed_pair = subprocess.run(self.bins.tx_argv() + complete, capture_output=True, text=True, timeout=60)
        assert_equal(signed_pair.returncode, 0)
        assert_equal(signed_pair.stderr, "")
        self.assert_signatures(complete, signed_pair.stdout)

    @staticmethod
    def assert_signatures(args, output):
        """Independently verify every successful CLI Schnorr signature."""
        raw = json.loads(output)["hex"] if "-json" in args else output.strip()
        tx = CTransaction()
        tx.deserialize(io.BytesIO(bytes.fromhex(raw)))
        prevouts = json.loads(next(arg[len("set=prevtxs:"):] for arg in args if arg.startswith("set=prevtxs:")))
        by_outpoint = {(int(prev["txid"], 16), prev["vout"]): prev for prev in prevouts}
        spent = []
        for txin in tx.vin:
            prev = by_outpoint[txin.prevout.hash, txin.prevout.n]
            spent.append(CTxOut(int(Decimal(str(prev["amount"])) * COIN), bytes.fromhex(prev["scriptPubKey"])))
        assert_equal(len(tx.wit.vtxinwit), len(tx.vin))
        for index, (txin, witness, prevout) in enumerate(zip(tx.vin, tx.wit.vtxinwit, spent)):
            assert_equal(txin.scriptSig, b"")
            assert_equal(len(witness.scriptWitness.stack), 1)
            signature = witness.scriptWitness.stack[0]
            assert_equal(len(signature), 64)
            assert_equal(prevout.type, CTxOut.TYPE_P2PK)
            assert verify_schnorr(prevout.pubkey, signature, TaprootSignatureHash(tx, spent, SIGHASH_DEFAULT, index))

    def test_one(self, testObj):
        """Runs a single test, comparing output and RC to expected output and RC.

        Raises an error if input can't be read, executable fails, or output/RC
        are not as expected. Error is caught by bctester() and reported.
        """
        # Get the exec names and arguments
        if testObj["exec"] == "./connectcoin-util":
            execrun = self.bins.util_argv() + testObj["args"]
        elif testObj["exec"] == "./connectcoin-tx":
            execrun = self.bins.tx_argv() + testObj["args"]
        else:
            raise ValueError(f"Unknown utility executable: {testObj['exec']}")

        # Read the input data (if there is any)
        inputData = None
        if "input" in testObj:
            with open(self.testcase_dir / testObj["input"]) as f:
                inputData = f.read()

        # Read the expected output data (if there is any)
        outputFn = None
        outputData = None
        outputType = None
        if "output_cmp" in testObj:
            outputFn = testObj['output_cmp']
            outputType = os.path.splitext(outputFn)[1][1:]  # output type from file extension (determines how to compare)
            with open(self.testcase_dir / outputFn) as f:
                outputData = f.read()
            if not outputData:
                raise Exception(f"Output data missing for {outputFn}")
            if not outputType:
                raise Exception(f"Output file {outputFn} does not have a file extension")
            if self.target_pointer_size == 4 and outputFn.startswith("getchainparams-"):
                # Golden files retain the 64-bit default. Adjust only this
                # target-dependent value, preserving every formatting byte and
                # the complete identity comparison for the 32-bit utility.
                assert_equal(outputData.count('"randomx_mode": "fast"'), 1)
                outputData = outputData.replace('"randomx_mode": "fast"', '"randomx_mode": "light"')

        # Run the test
        res = subprocess.run(execrun, capture_output=True, text=True, input=inputData, timeout=60)
        # Report the real process failure before trying to parse empty stdout.
        if res.returncode != testObj.get("return_code", 0):
            raise Exception(f"Return code mismatch for {outputFn}; res: {str(res)}")

        if outputData:
            data_mismatch, formatting_mismatch = False, False
            # Parse command output and expected output
            try:
                a_parsed = parse_output(res.stdout, outputType)
            except Exception as e:
                self.log.error(f"Error parsing command output as {outputType}: '{str(e)}'; res: {str(res)}")
                raise
            try:
                b_parsed = parse_output(outputData, outputType)
            except Exception as e:
                self.log.error('Error parsing expected output %s as %s: %s' % (outputFn, outputType, e))
                raise
            # Compare data
            if a_parsed != b_parsed:
                self.log.error(f"Output data mismatch for {outputFn} (format {outputType}); res: {str(res)}")
                data_mismatch = True
            # Compare formatting
            if res.stdout != outputData:
                error_message = f"Output formatting mismatch for {outputFn}:\nres: {str(res)}\n"
                error_message += "".join(difflib.context_diff(outputData.splitlines(True),
                                                              res.stdout.splitlines(True),
                                                              fromfile=outputFn,
                                                              tofile="returned"))
                self.log.error(error_message)
                formatting_mismatch = True

            assert not data_mismatch and not formatting_mismatch

        if "error_txt" in testObj:
            want_error = testObj["error_txt"]
            # A partial match instead of an exact match makes writing tests easier
            # and should be sufficient.
            if want_error not in res.stderr:
                raise Exception(f"Error mismatch:\nExpected: {want_error}\nReceived: {res.stderr.rstrip()}\nres: {str(res)}")
        else:
            if res.stderr:
                raise Exception(f"Unexpected error received: {res.stderr.rstrip()}\nres: {str(res)}")
        if res.returncode == 0 and any(arg.startswith("sign=") for arg in testObj["args"]):
            self.assert_signatures(testObj["args"], res.stdout)


def parse_output(a, fmt):
    """Parse the output according to specified format.

    Raise an error if the output can't be parsed."""
    if fmt == 'json':  # json: compare parsed data
        return json.loads(a)
    elif fmt == 'hex':  # hex: parse and compare binary data
        return bytes.fromhex(a.strip())
    else:
        raise NotImplementedError("Don't know how to compare %s" % fmt)


if __name__ == "__main__":
    ToolUtils(__file__).main()
