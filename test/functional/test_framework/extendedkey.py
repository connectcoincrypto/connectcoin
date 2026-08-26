#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Base classes for creating dynamic xprvs and xpubs. Only basic
functionality of BIP32 is provided here. These classes work over
the ECKey and ECPubKey classes in the key.py."""

import hashlib
import hmac
import unittest

from test_framework.address import byte_to_base58
from test_framework.crypto import secp256k1
from test_framework.key import (
    ECKey,
    ECPubKey,
    generate_privkey,
    ORDER,
)
from test_framework.script import hash160

BIP32_HARDENED = 0x80000000

def get_version(private=False, mainnet=False):
    if mainnet:
        if private:
            return bytes.fromhex("A7C73B9F")
        return bytes.fromhex("A7C73FD9")

    if private:
        return bytes.fromhex("04313316")
    return bytes.fromhex("04313A97")

def hardened(index):
    assert 0 <= index < BIP32_HARDENED
    return index | BIP32_HARDENED

def derive_path(key, path, path_idx_parser):
    if path in ("", "m"):
        return key
    parts = path.split("/")
    if parts[0] == "m":
        parts = parts[1:]
    for part in parts:
        index = path_idx_parser(part)
        key = key._derive(index)

    return key

class ExtendedPrivateKey:
    def __init__(self, key, chaincode, depth=0, parent_fingerprint_bytes=b"\x00\x00\x00\x00", child_num=0):
        self.key = key
        self.chaincode = chaincode
        self.depth = depth
        self.parent_fingerprint_bytes = parent_fingerprint_bytes
        self.child_num = child_num

    @classmethod
    def from_seed(cls, seed):
        I = hmac.new(b"Bitcoin seed", seed, hashlib.sha512).digest()

        secret = I[:32]
        chaincode = I[32:]

        secret_int = int.from_bytes(secret, "big")
        if secret_int == 0 or secret_int >= ORDER:
            raise ValueError("Invalid master key")

        key = ECKey()
        key.set(secret, compressed=True)

        return cls(key, chaincode)

    @classmethod
    def generate(cls):
        return cls.from_seed(generate_privkey())

    def _fingerprint(self):
        return hash160(self.key.get_pubkey().get_bytes())[:4]

    def _derive(self, index):
        if index >= BIP32_HARDENED:
            data = (b"\x00" + self.key.get_bytes() + index.to_bytes(4, "big"))
        else:
            data = (self.key.get_pubkey().get_bytes() + index.to_bytes(4, "big"))

        I = hmac.new(self.chaincode, data, hashlib.sha512).digest()
        IL = I[:32]
        IR = I[32:]

        IL_int = int.from_bytes(IL, "big")
        child_secret = (IL_int + int.from_bytes(self.key.get_bytes(), "big")) % ORDER
        # Per BIP32, if IL >= n or the child key is 0, the key is invalid. This is
        # astronomically unlikely (~1 in 2^127), so reject rather than retrying the next index.
        if IL_int >= ORDER or child_secret == 0:
            raise ValueError("Invalid BIP32 child key")

        child = ECKey()
        child.set(child_secret.to_bytes(32, 'big'), compressed=True)

        return ExtendedPrivateKey(child, IR, self.depth + 1, self._fingerprint(), index)

    def _serialize(self):
        return (bytes([self.depth]) + self.parent_fingerprint_bytes + self.child_num.to_bytes(4, "big") + self.chaincode + b"\x00" + self.key.get_bytes())

    def pubkey(self):
        return ExtendedPublicKey(self.key.get_pubkey(), self.chaincode, self.depth, self.parent_fingerprint_bytes, self.child_num)

    def derive_path(self, path):
        def path_idx_parser(part):
            if part.endswith(("h", "'")):
                return hardened(int(part[:-1]))
            return int(part)

        return derive_path(self, path, path_idx_parser)

    def to_string(self, mainnet=False):
        return byte_to_base58(self._serialize(), get_version(private=True, mainnet=mainnet))

class ExtendedPublicKey:
    def __init__(self, pubkey, chaincode, depth, parent_fingerprint_bytes, child_num):
        self.pubkey = pubkey
        self.chaincode = chaincode
        self.depth = depth
        self.parent_fingerprint_bytes = parent_fingerprint_bytes
        self.child_num = child_num

    def _fingerprint(self):
        return hash160(self.pubkey.get_bytes())[:4]

    def _derive(self, index):
        if index >= BIP32_HARDENED:
            raise ValueError("Cannot derive hardened child from xpub")

        data = (self.pubkey.get_bytes() + index.to_bytes(4, "big"))
        I = hmac.new(self.chaincode, data, hashlib.sha512).digest()
        IL = I[:32]
        IR = I[32:]

        IL_int = int.from_bytes(IL, "big")
        child_point = IL_int * secp256k1.G + self.pubkey.p
        # Per BIP32, if IL >= n or the resulting point is infinity, the key is invalid.
        if IL_int >= ORDER or child_point.infinity:
            raise ValueError("Invalid BIP32 child key")

        child_pubkey = ECPubKey()
        child_pubkey.set(child_point.to_bytes_compressed())

        return ExtendedPublicKey(child_pubkey, IR, self.depth + 1, self._fingerprint(), index)

    def _serialize(self):
        return (bytes([self.depth]) + self.parent_fingerprint_bytes + self.child_num.to_bytes(4, "big") + self.chaincode + self.pubkey.get_bytes())

    def derive_path(self, path):
        return derive_path(self, path, lambda x: int(x))

    def to_string(self, mainnet=False):
        return byte_to_base58(self._serialize(), get_version(private=False, mainnet=mainnet))

class TestFrameworkExtendedKey(unittest.TestCase):
    def test_bip32_vectors(self):
        vectors = [
            [
                "000102030405060708090a0b0c0d0e0f",
                [
                    ["m", "ccprvNzbdrJ6DyFCgfaJ6dTP9pj1qQPgsabB2cZn6bi9iDvPrUb9CacpqtzKXTuKAj3mh9izy4nP7B72JjLHsbBgKxmdpKHvbvwVcRWAuENGPUNf", "ccpubKDazFod7ockyt4NZjUvABrxZxRXMz3tsynhhQ6ZKnFvqMPUM8A96Sne1KAxUEwBm5FtuZjQFpQNcXrV1tRWyaodu6zeq6U1DBcmeGiRaKd6"],
                    ["m/0h", "ccprvP2s3r1Ph4WKfpBRoqDFnhm7wjDFDxff3JBCzZoXqC7b8qsSNzrMw28fwFAfW8m9ermtfS5hyZTsBT6KbEmxWkqiLPAbkAuCW8WokfbMdsPT", "ccpubKFrQFWvatssy2fWGwEno4u4gHF5iN8NtfQ8bNBwSkT87ifmXYPgBZvzR6SXAzF6J5UBURxwgZ3ShWFT9qTFMeh6dJdT61muRhYuCaT6D5vW"],
                    ["m/0h/1", "ccprvP53B3nwaTDCjedUFZU9fJW1bnbEKxhPLEVsqA5Je8kPM5QwWienEyhaAfDrZjgvhJreMEuJQeApbb2qpN7kVY1CmtwZUEeyMoK88CTTDSpM", "ccpubKJ2XTJUUHam2s7YifVgffdxLLd4pNA7BbioRxTiFh5vKxDGfGC6VXVteWWykQBoe3BoXDPPnfdwB6ZByJnRFLLvZbqHwVQuhLHRysHoinzj"],
                    ["m/0h/1/2h", "ccprvP7eT6KmSA649WoGLp4Z2fW3k1k6rEEyKa37WQQboJcknVG8dXQcft986DG5BhvSBpKfjqNFL4aeVpEaZx8fgf6eSFZ82PbDJQasLq5VBbgu", "ccpubKLdoVqJKzTcSjHLov6632dzUZmwLdhhAwG37Co1QrxHmN4Tn4wvvRwSa4YAar4ExPhuFpmNQ3aW9YtLG1sxnPo5QBjG5aMyP3yM7xXT9kbU"],
                    ["m/0h/1/2h/2", "ccprvP9sqvktPLZj8bGk4WKm7Z1D46ZParqHX1WzofN8Gd4KPrMkzbBK35ib1taUeAaqct7oSTHUtyQ6fHRV29ZD2rVEUeoD7wg31ZZLWLbLadwB", "ccpubKNsCLGRHAwHRokpXcMJ7v99nebE5GJ1NNjvQTkXtBPrNjA698idHdWuVjs9yFrPgs22RxmrM6KQG7JgG7BPz7PYf6WpGQYsD4fFy79k6Z8i"],
                    ["m/0h/1/2h/2/1000000000", "ccprvPBbcQSVdTh7L7cXWcsKqriD3tnxtgL6YomPMueWQq8htZVAP7bZhLpk5XSFr5VxhVXChEeTqpSdnHbVPgdDkySt1zHyM4aniqtts5jHnxiy", "ccpubKQaxox2XJ4fdL6byitrrDr9nSpoP5npQAzJxi2v2PUEsSHVXf8swtd4ZNh4v77UQLiJyU7n9dX6rhj5HD2tpxehmy4EMD2SqscstMPzk7r3"],
                ]
            ],
            [
                "fffcf9f6f3f0edeae7e4e1dedbd8d5d2cfccc9c6c3c0bdbab7b4b1aeaba8a5a29f9c999693908d8a8784817e7b7875726f6c696663605d5a5754514e4b484542",
                [
                    ["m", "ccprvNzbdrJ6DyFCgfBoRjcTSGvjbeaqJGDRrJ9ZptetYCJWimaAPkmbnKtV7oy6DuZKnPmkPp4qZTD4Vps1ydc52a8xamhzr5HQLzko8bQDpJfC", "ccpubKDazFod7ockysfstqdzSe4gLCcfnfg9hfNVRh3J9ke3heNVYJJv2sgobfH3SxBYiyp8hUQ4RDUssFeDF1cS8iidqxvrHvxVsVzSkYiqTq2C"],
                    ["m/0", "ccprvP3sP81nBTG2mBTPisPqKLj981YaYyBv8rKKC4TuzGiNgTYcNL2wrUj4NPUN2YdAcoRzDi5FHtNUdBNeoBta5WN7ymtQuTma68AhgxfkfqEQ", "ccpubKGrjXXK5Hdb4PwUByRNKhs5rZaR3NedzDYEnrrKbq3ufLLwWsaG72XNrEk1LkGZAC8csLd4AcZiqQodvtm8Z4KJ8EyaxqJeFdUZQyvQqvB7"],
                    ["m/0/2147483647h", "ccprvP52SNcohqACsMKbuHqpxp4pgQHoTWg5pwUPiyrpkvT94uqqpwvNAgnqxzpr1Gsu6hfK7V6SvwNyjccPZvGBD5fZYCceFEHJLux3Dj47AFT2", "ccpubKJ1nn8LbfXmAZogNPsMyBCmQxKdwv8ogJhKKnFENUng3neAyVTgREbASr8FTW1AFyeYQzTumun9S5UWGffcfnTkkn4D2C2uGVrq1pg2YCvu"],
                    ["m/0/2147483647h/1", "ccprvP7qQndndzoW4SwH3MVyXn1kujpfxcs16FutapvV6ELVWafhzFRtsahELqd3dJb6o1cfpEPXKLvewwKAiy9hb69NqLPB45PGjPkTxCnAKa5z", "ccpubKLpmC9KXqB4MfRMWTXWY99heHrWT2Kiwd8pBdJthng2VTU38nyD88VYpgvSbHsrYZpzxq711YuTGAGmHaa3d3oDfNLQsQaLURdVy55AKKaY"],
                    ["m/0/2147483647h/1/2147483646h", "ccprvP91ShbizcCTmjtWe5yo59XT6eZ9KfqdbHccUMeBW56km322vM6pt7DbSUKn1w1V5ZBxWYDJJjJEXkSG6V6c5mz41AKiNDbJMjLUUGZax3ys", "ccpubKMzo77FtSa24xNb7C1L5WfPqCayp5JMSeqY5A2b7dSHjupN4te98f1uvKaa6VFNzXGebdc4ibWn7ouvgGTA8m9v7eEjMxsRtHraYc5dxvAr"],
                    ["m/0/2147483647h/1/2147483646h/2", "ccprvPANUf2wW8Nn1V9hXp3vm8Gq53cgRvAYfwnEMmRXzhtcM7DNVWjkeHnYSYjT262a9bVpEpxw8BQ5Dnz9cDQyJpxniyBDsH1S5Rhp2M64vrWW", "ccpubKPMq4YUPxkLJhdmzv5TmVQmobeWvKdGXK19xZowcGE9Kz1he4H4tqarvPyeNcpjh3KrpuXa8VDo7JLzB6Yr35QmhHQZSuEicy9mjGfrpSUa"]
                ]
            ]
        ]

        for vector in vectors:
            seed = bytes.fromhex(vector[0])
            xprv = ExtendedPrivateKey.from_seed(seed)
            for seed_vector in vector[1]:
                path = seed_vector[0]
                derivedxprv = xprv.derive_path(path)
                self.assertEqual(derivedxprv.to_string(mainnet=True), seed_vector[1])
                self.assertEqual(derivedxprv.pubkey().to_string(mainnet=True), seed_vector[2])
