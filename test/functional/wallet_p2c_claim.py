#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Wallet P2C claim preparation and rejection tests; no live TLS or external roots."""

from decimal import Decimal

from test_framework.messages import COIN, CTxInWitness, tx_from_hex
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


def structural_proof(domain, challenge):
    """Well-framed TLS with dummy DER/signature: it must NEVER pass crypto checks."""
    def u16(value):
        return value.to_bytes(2, "big")

    def u24(value):
        return value.to_bytes(3, "big")

    def extension(kind, body):
        return u16(kind) + u16(len(body)) + body

    def handshake(kind, body):
        return bytes([kind]) + u24(len(body)) + body

    name = domain.encode("ascii")
    extensions = extension(0, u16(len(name) + 3) + b"\x00" + u16(len(name)) + name)
    extensions += extension(43, b"\x02\x03\x04")
    extensions += extension(13, b"\x00\x02\x04\x03")
    extensions += extension(51, u16(36) + u16(29) + u16(32) + b"\x01" * 32)
    client = b"\x03\x03" + bytes.fromhex(challenge) + b"\x00\x00\x02\x13\x01\x01\x00"
    client += u16(len(extensions)) + extensions
    extensions = extension(43, b"\x03\x04") + extension(51, u16(29) + u16(32) + b"\x02" * 32)
    server = b"\x03\x03" + b"\x03" * 32 + b"\x00\x13\x01\x00" + u16(len(extensions)) + extensions
    certificate = b"\x00" + u24(6) + u24(1) + b"\x30\x00\x00"
    return (b"\x02" + handshake(1, client) + handshake(2, server) + handshake(8, b"\x00\x00")
            + handshake(11, certificate) + handshake(15, b"\x04\x03\x00\x01\x30")).hex()


class P2CClaimTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Manual claims must work without an estimator or configured fallback.
        self.extra_args = [["-fallbackfee=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        funder = node.get_wallet_rpc(self.default_wallet_name)
        node.createwallet("claimant")
        claimant = node.get_wallet_rpc("claimant")
        node.createwallet("watch", disable_private_keys=True)
        watch = node.get_wallet_rpc("watch")

        def bounty(bits=0):
            txid = funder.sendtop2c("example.com", 1, {"work_bits": bits})["txids"][0]
            outputs = funder.gettransaction(txid, verbose=True)["decoded"]["vout"]
            return txid, next(out["n"] for out in outputs if out["type"] == 2)

        outpoint = bounty()
        strict_outpoint = bounty(256)
        assert_raises_rpc_error(-4, "must be confirmed", claimant.preparep2cclaim, *outpoint)
        block = self.generate(node, 1)[0]
        assert_equal(claimant.getbalance(), 0)
        assert_raises_rpc_error(-5, "Invalid or non-wallet", claimant.gettransaction, outpoint[0])
        history_before = claimant.listtransactions()
        mempool_before = node.getrawmempool()

        self.log.info("Prepare external bounties without any claimant funds or private-key signature")
        prepared = claimant.preparep2cclaim(*outpoint)
        decoded = node.decoderawtransaction(prepared["hex"])
        assert_equal(len(decoded["vin"]), 1)
        assert_equal(len(decoded["vout"]), 1)
        assert_equal(decoded["vin"][0]["txid"], outpoint[0])
        assert_equal(decoded["vin"][0]["vout"], outpoint[1])
        assert_equal(decoded["vin"][0]["sequence"], 0xFFFFFFFF)
        assert_equal(decoded["vout"][0]["type"], 1)
        assert_equal(decoded["vout"][0]["scriptPubKey"]["address"], prepared["address"])
        assert claimant.getaddressinfo(prepared["address"])["ismine"]
        assert_equal(prepared["domain"], "example.com")
        assert_equal(prepared["connection_work_target"], "ff" * 32)
        assert_equal(prepared["root_certificates_version"], 1)
        assert_equal(prepared["proof_size"], 65536)
        assert_equal(prepared["fee"] + prepared["receive_amount"], 1)
        assert_equal(prepared["bounty_amount"], 1)
        assert_equal(prepared["validation_time"], node.getblockchaininfo()["mediantime"])
        challenge = node.getp2cchallenge(prepared["hex"], 0)
        assert_equal(challenge["clienthello_random"], prepared["clienthello_random"])
        assert_equal(challenge["txid"], prepared["txid"])

        self.log.info("Budget full wire overhead at CompactSize boundaries, with CLI conversion")
        for size in (1, 252, 253, 65535, 65536):
            proposal = node.cli("-rpcwallet=claimant").preparep2cclaim(*outpoint, fee_rate=10000, proof_size=size)
            tx = tx_from_hex(proposal["hex"])
            tx.wit.vtxinwit = [CTxInWitness()]
            tx.wit.vtxinwit[0].scriptWitness.stack = [b"\x00" * size]
            signed_size = node.decoderawtransaction(tx.serialize_with_witness().hex())["vsize"]
            assert_equal(proposal["fee"], Decimal(signed_size * 10000) / COIN)
            assert_equal(node.getp2cchallenge(tx.serialize_with_witness().hex(), 0)["txid"], proposal["txid"])

        self.log.info("Reject malformed parameters, excessive fees, and unsupported wallets")
        for size in (0, -1, 65537, 2**63 - 1):
            assert_raises_rpc_error(-8, "proof_size", claimant.preparep2cclaim, *outpoint, proof_size=size)
        for index in (-1, 2**32):
            assert_raises_rpc_error(-8, "vout", claimant.preparep2cclaim, outpoint[0], index)
        assert_raises_rpc_error(-4, "unavailable or already spent", claimant.preparep2cclaim, "00" * 32, 1)
        assert_raises_rpc_error(-4, "local private keys", watch.preparep2cclaim, *outpoint)
        assert_raises_rpc_error(-4, "Fee rate", claimant.preparep2cclaim, *outpoint, fee_rate=10**15)
        assert_raises_rpc_error(-4, "exceeds the wallet maximum", claimant.preparep2cclaim, *outpoint, fee_rate=10**8)
        normal = funder.listunspent()[0]
        assert_raises_rpc_error(-4, "not a supported P2C bounty", claimant.preparep2cclaim, normal["txid"], normal["vout"])

        self.log.info("Structural validity is not enough: consensus must reject the dummy certificate")
        external = funder.getnewaddress()
        for rpc in (claimant, watch):
            keypool_before = rpc.getwalletinfo()["keypoolsize"]
            payout = rpc.preparep2cclaim(*outpoint, address=external)
            assert_equal(rpc.getwalletinfo()["keypoolsize"], keypool_before)
            assert_equal(payout["address"], external)
            assert_equal(node.decoderawtransaction(payout["hex"])["vout"][0]["scriptPubKey"]["address"], external)
            external_proof = structural_proof(payout["domain"], payout["clienthello_random"])
            assert_raises_rpc_error(-4, "explicitly specified reward address", claimant.submitp2cclaim, payout["hex"], external_proof)
            assert_raises_rpc_error(-4, "explicitly specified reward address", rpc.submitp2cclaim, payout["hex"], external_proof, prepared["address"])
            assert_raises_rpc_error(-4, "invalid DER certificate", rpc.submitp2cclaim, payout["hex"], external_proof, external)
        assert_equal(claimant.getaddressinfo(external)["ismine"], False)
        assert_raises_rpc_error(-5, "P2PK reward address", claimant.preparep2cclaim, *outpoint, address="invalid")
        assert_raises_rpc_error(-5, "P2PK reward address", claimant.submitp2cclaim, payout["hex"], external_proof, "invalid")
        assert_equal(claimant.getaddressinfo(claimant.preparep2cclaim(*outpoint, address="")["address"])["ismine"], True)
        proof = structural_proof(prepared["domain"], prepared["clienthello_random"])
        assert_raises_rpc_error(-4, "P2C proof contains an invalid DER certificate", claimant.submitp2cclaim, prepared["hex"], proof)
        self.log.info("The reset network rejects complete legacy version-1 proofs")
        legacy_proof = "01" + proof[2:]
        assert_raises_rpc_error(-4, "unsupported P2C proof version", claimant.submitp2cclaim, prepared["hex"], legacy_proof)
        assert_raises_rpc_error(-4, "Invalid P2C proof:", claimant.submitp2cclaim, prepared["hex"], "01")
        for invalid in ("", "xyz", "0", "00" * 65537):
            assert_raises_rpc_error(-8, "proof must encode", claimant.submitp2cclaim, prepared["hex"], invalid)
        assert_raises_rpc_error(-22, "TX decode failed", claimant.submitp2cclaim, "00", "01")
        assert_raises_rpc_error(-8, "proposal is too large", claimant.submitp2cclaim, "00" * 65537, "01")
        strict = claimant.preparep2cclaim(*strict_outpoint)
        assert_raises_rpc_error(-4, "work hash exceeds target", claimant.submitp2cclaim, strict["hex"],
                                structural_proof("example.com", strict["clienthello_random"]))

        self.log.info("Reject redirected payouts, changed challenges, extra inputs, and overspending")
        redirected = node.createrawtransaction(
            [{"txid": outpoint[0], "vout": outpoint[1], "sequence": 0xFFFFFFFF}],
            [{funder.getnewaddress(): prepared["receive_amount"]}],
        )
        assert_raises_rpc_error(-4, "belonging to this wallet", claimant.submitp2cclaim, redirected, proof)
        tx = tx_from_hex(prepared["hex"])
        tx.vout[0].nValue -= 1
        assert_raises_rpc_error(-4, "Invalid P2C proof:", claimant.submitp2cclaim, tx.serialize().hex(), proof)
        tx.vout[0].nValue = 2 * COIN
        assert_raises_rpc_error(-4, "Invalid P2C payout", claimant.submitp2cclaim, tx.serialize().hex(), proof)
        tx = tx_from_hex(prepared["hex"])
        tx.vin.append(tx.vin[0])
        assert_raises_rpc_error(-4, "one input and one output", claimant.submitp2cclaim, tx.serialize().hex(), proof)
        witnessed = node.setp2cproof(prepared["hex"], 0, proof)
        assert_raises_rpc_error(-4, "unwitnessed P2C claim", claimant.submitp2cclaim, witnessed, proof)
        normal_input = node.createrawtransaction(
            [{"txid": normal["txid"], "vout": normal["vout"], "sequence": 0xFFFFFFFF}],
            [{prepared["address"]: prepared["receive_amount"]}],
        )
        assert_raises_rpc_error(-4, "not a supported P2C bounty", claimant.submitp2cclaim, normal_input, proof)

        self.log.info("Reorgs removing bounty confirmation prevent both preparation and submission")
        node.invalidateblock(block)
        assert_raises_rpc_error(-4, "must be confirmed", claimant.preparep2cclaim, *outpoint)
        assert_raises_rpc_error(-4, "must be confirmed", claimant.submitp2cclaim, prepared["hex"], proof)
        node.reconsiderblock(block)
        claimant.preparep2cclaim(*outpoint)

        self.log.info("Locked wallets can prepare payouts; rejected proofs never enter history")
        claimant.encryptwallet("test passphrase")
        assert_equal(claimant.getwalletinfo()["unlocked_until"], 0)
        claimant.preparep2cclaim(*outpoint)
        assert_equal(claimant.getwalletinfo()["unlocked_until"], 0)

        self.log.info("Reserved claim destinations survive wallet reload and are not recycled")
        node.unloadwallet("claimant")
        node.loadwallet("claimant")
        assert claimant.getaddressinfo(prepared["address"])["ismine"]
        assert_equal(claimant.getwalletinfo()["unlocked_until"], 0)
        reloaded = claimant.preparep2cclaim(*outpoint)
        assert reloaded["address"] != prepared["address"]
        assert_raises_rpc_error(-4, "P2C proof contains an invalid DER certificate", claimant.submitp2cclaim, prepared["hex"], proof)
        assert_equal(claimant.getbalance(), 0)
        assert_equal(claimant.listtransactions(), history_before)
        assert_equal(claimant.listlockunspent(), [])
        assert_equal(node.getrawmempool(), mempool_before)


if __name__ == "__main__":
    P2CClaimTest(__file__).main()
