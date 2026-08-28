#!/usr/bin/env python3
# Copyright (c) 2018-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import argparse
import json
import os
import struct
import sys
import traceback
from io import BytesIO

TEST_FRAMEWORK_PATH = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
sys.path.insert(0, TEST_FRAMEWORK_PATH)

from test_framework.address import b58chars  # noqa: E402
from test_framework.extendedkey import ExtendedPrivateKey  # noqa: E402
from test_framework.key import (  # noqa: E402
    ECKey,
    compute_xonly_pubkey,
    sign_schnorr,
    tweak_add_privkey,
)
from test_framework.messages import (  # noqa: E402
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    deser_compact_size,
    from_binary,
    hash256,
)
from test_framework.psbt import (  # noqa: E402
    PSBT,
    PSBT_GLOBAL_FALLBACK_LOCKTIME,
    PSBT_GLOBAL_TX_VERSION,
    PSBT_GLOBAL_UNSIGNED_TX,
    PSBT_IN_OUTPUT_INDEX,
    PSBT_IN_PREVIOUS_TXID,
    PSBT_IN_REQUIRED_HEIGHT_LOCKTIME,
    PSBT_IN_REQUIRED_TIME_LOCKTIME,
    PSBT_IN_SEQUENCE,
    PSBT_IN_TAP_BIP32_DERIVATION,
    PSBT_IN_TAP_KEY_SIG,
    PSBT_IN_WITNESS_UTXO,
    PSBT_OUT_AMOUNT,
    PSBT_OUT_SCRIPT,
)
from test_framework.script import (  # noqa: E402
    SIGHASH_DEFAULT,
    TaprootSignatureHash,
    taproot_construct,
)


MOCK_XPRV = "tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR"


def decode_mock_xprv():
    value = 0
    for char in MOCK_XPRV:
        value = value * 58 + b58chars.index(char)
    encoded = value.to_bytes((value.bit_length() + 7) // 8, "big")
    encoded = b"\x00" * (len(MOCK_XPRV) - len(MOCK_XPRV.lstrip("1"))) + encoded
    if len(encoded) != 82 or hash256(encoded[:-4])[:4] != encoded[-4:]:
        raise ValueError("invalid mock extended private key")

    payload = encoded[:-4]
    key = ECKey()
    key.set(payload[46:78], compressed=True)
    return ExtendedPrivateKey(
        key,
        payload[13:45],
        depth=payload[4],
        parent_fingerprint_bytes=payload[5:9],
        child_num=int.from_bytes(payload[9:13], "big"),
    )


def unsigned_tx_from_psbt(psbt):
    if psbt.version == 0:
        return from_binary(CTransaction, psbt.g.map[PSBT_GLOBAL_UNSIGNED_TX])

    tx = CTransaction()
    tx.version = int.from_bytes(psbt.g.map[PSBT_GLOBAL_TX_VERSION], "little", signed=True)

    height_locks = []
    time_locks = []
    for psbt_input in psbt.i:
        if PSBT_IN_REQUIRED_HEIGHT_LOCKTIME in psbt_input.map:
            height_locks.append(int.from_bytes(psbt_input.map[PSBT_IN_REQUIRED_HEIGHT_LOCKTIME], "little"))
        if PSBT_IN_REQUIRED_TIME_LOCKTIME in psbt_input.map:
            time_locks.append(int.from_bytes(psbt_input.map[PSBT_IN_REQUIRED_TIME_LOCKTIME], "little"))
    if height_locks and time_locks:
        raise ValueError("PSBTv2 contains incompatible height and time lock requirements")
    if height_locks:
        tx.nLockTime = max(height_locks)
    elif time_locks:
        tx.nLockTime = max(time_locks)
    else:
        tx.nLockTime = int.from_bytes(psbt.g.map.get(PSBT_GLOBAL_FALLBACK_LOCKTIME, b"\x00" * 4), "little")

    for psbt_input in psbt.i:
        prev_txid = psbt_input.map[PSBT_IN_PREVIOUS_TXID]
        if len(prev_txid) != 32:
            raise ValueError("PSBTv2 previous transaction id must be 32 bytes")
        prev_index = int.from_bytes(psbt_input.map[PSBT_IN_OUTPUT_INDEX], "little")
        sequence = int.from_bytes(psbt_input.map.get(PSBT_IN_SEQUENCE, b"\xff" * 4), "little")
        tx.vin.append(CTxIn(COutPoint(int.from_bytes(prev_txid, "little"), prev_index), nSequence=sequence))

    for psbt_output in psbt.o:
        amount = int.from_bytes(psbt_output.map[PSBT_OUT_AMOUNT], "little", signed=True)
        tx.vout.append(CTxOut(amount, psbt_output.map[PSBT_OUT_SCRIPT]))
    return tx


def sign_typed_psbt(encoded_psbt):
    psbt = PSBT.from_base64(encoded_psbt)
    tx = unsigned_tx_from_psbt(psbt)
    spent_outputs = []
    for psbt_input in psbt.i:
        if PSBT_IN_WITNESS_UTXO not in psbt_input.map:
            raise ValueError("typed signer requires witness_utxo for every input")
        spent_outputs.append(from_binary(CTxOut, psbt_input.map[PSBT_IN_WITNESS_UTXO]))

    descriptor_root = decode_mock_xprv()
    for input_index, psbt_input in enumerate(psbt.i):
        for key, value in list(psbt_input.map.items()):
            if not isinstance(key, bytes) or len(key) != 33 or key[0] != PSBT_IN_TAP_BIP32_DERIVATION:
                continue

            reader = BytesIO(value)
            leaf_hash_count = deser_compact_size(reader)
            reader.read(32 * leaf_hash_count)
            if reader.read(4) != bytes.fromhex("00000001"):
                continue

            path = []
            while component := reader.read(4):
                if len(component) != 4:
                    raise ValueError("truncated BIP32 path")
                path.append(struct.unpack("<I", component)[0])
            if len(path) < 3:
                raise ValueError("unexpected BIP32 path")

            child = descriptor_root
            for component in path[3:]:
                child = child._derive(component)
            secret = child.key.get_bytes()
            internal_key, _ = compute_xonly_pubkey(secret)
            if internal_key != key[1:]:
                continue

            tweak = taproot_construct(internal_key).tweak
            tweaked_secret = tweak_add_privkey(secret, tweak)
            sighash = TaprootSignatureHash(
                tx,
                spent_outputs,
                SIGHASH_DEFAULT,
                input_index=input_index,
            )
            psbt_input.map[PSBT_IN_TAP_KEY_SIG] = sign_schnorr(tweaked_secret, sighash)
            break

    return psbt.to_base64()

def perform_pre_checks():
    mock_result_path = os.path.join(os.getcwd(), "mock_result")
    if os.path.isfile(mock_result_path):
        with open(mock_result_path, "r") as f:
            mock_result = f.read()
        if mock_result[0]:
            sys.stdout.write(mock_result[2:])
            sys.exit(int(mock_result[0]))

def enumerate_signers(args):
    sys.stdout.write(json.dumps([{
        "fingerprint": "00000001",
        "type": "connectcoin-test-signer",
        "model": "typed-v1-mock",
        "protocol": "connectcoin-typed-v1",
    }]))

def getdescriptors(args):
    xpub = "tcubNC1uBHhBxmWgfPoCJ8krUwtLbnqX2uhWwmgfSt2GLbCHhPuEq4LhtLBZKLwC6YkHK2hXrPWvVMgReYFtgWxenNZAuC69MaERFZ7AygMKgQd"

    sys.stdout.write(json.dumps({
        "receive": [
            "pkh([00000001/44h/1h/" + args.account + "']" + xpub + "/0/*)#r3hvwn9g",
            "sh(wpkh([00000001/49h/1h/" + args.account + "']" + xpub + "/0/*))#7zskq2dp",
            "wpkh([00000001/84h/1h/" + args.account + "']" + xpub + "/0/*)#k9f6qm0f",
            "tr([00000001/86h/1h/" + args.account + "']" + xpub + "/0/*)#z2y76n6x"
        ],
        "internal": [
            "pkh([00000001/44h/1h/" + args.account + "']" + xpub + "/1/*)#j9jdnx4s",
            "sh(wpkh([00000001/49h/1h/" + args.account + "']" + xpub + "/1/*))#tr7qc4c7",
            "wpkh([00000001/84h/1h/" + args.account + "']" + xpub + "/1/*)#83vmawl3",
            "tr([00000001/86h/1h/" + args.account + "']" + xpub + "/1/*)#n7pl8x27"

        ]
    }))


def displayaddress(args):
    if args.fingerprint != "00000001":
        return sys.stdout.write(json.dumps({"error": "Unexpected fingerprint", "fingerprint": args.fingerprint}))

    expected_desc = {
        "wpkh([00000001/84h/1h/0h/0/0]02c97dc3f4420402e01a113984311bf4a1b8de376cac0bdcfaf1b3ac81f13433c7)#3te6hhy7": "ccrt1qm90ugl4d48jv8n6e5t9ln6t9zlpm5th6jye4cd",
        "sh(wpkh([00000001/49h/1h/0h/0/0]02c97dc3f4420402e01a113984311bf4a1b8de376cac0bdcfaf1b3ac81f13433c7))#kz9y5w82": "tGMnPWAbec9DvEC2ccyai6gLNUcSUDzgvw",
        "pkh([00000001/44h/1h/0h/0/0]02c97dc3f4420402e01a113984311bf4a1b8de376cac0bdcfaf1b3ac81f13433c7)#q3pqd8wh": "TVnaMjTKy4nQ45qs4EUohZLkZs3vGva9ro",
        "tr([00000001/86h/1h/0h/0/0]c97dc3f4420402e01a113984311bf4a1b8de376cac0bdcfaf1b3ac81f13433c7)#puqqa90m": "tcc1phw4cgpt6cd30kz9k4wkpwm872cdvhss29jga2xpmftelhqll62ms0exlg6",
        "wpkh([00000001/84h/1h/0h/0/1]03a20a46308be0b8ded6dff0a22b10b4245c587ccf23f3b4a303885be3a524f172)#aqpjv5xr": "wrong_address",
    }
    if args.desc not in expected_desc:
        return sys.stdout.write(json.dumps({"error": "Unexpected descriptor", "desc": args.desc}))

    return sys.stdout.write(json.dumps({"address": expected_desc[args.desc]}))

def signtx(args):
    if args.fingerprint != "00000001":
        return sys.stdout.write(json.dumps({"error": "Unexpected fingerprint", "fingerprint": args.fingerprint}))

    mock_psbt_path = os.path.join(os.getcwd(), "mock_psbt")
    if os.path.isfile(mock_psbt_path):
        with open(mock_psbt_path, "r") as f:
            signed_psbt = f.read()
    else:
        try:
            signed_psbt = sign_typed_psbt(args.psbt)
        except Exception as error:
            return sys.stdout.write(json.dumps({"error": f"{type(error).__name__}: {error}\n{traceback.format_exc()}"}))

    sys.stdout.write(json.dumps({
        "psbt": signed_psbt,
        "complete": True,
    }))

parser = argparse.ArgumentParser(prog='./signer.py', description='External signer mock')
parser.add_argument('--fingerprint')
parser.add_argument('--chain', default='main')
parser.add_argument('--stdin', action='store_true')

subparsers = parser.add_subparsers(description='Commands', dest='command')
subparsers.required = True

parser_enumerate = subparsers.add_parser('enumerate', help='list available signers')
parser_enumerate.set_defaults(func=enumerate_signers)

parser_getdescriptors = subparsers.add_parser('getdescriptors')
parser_getdescriptors.set_defaults(func=getdescriptors)
parser_getdescriptors.add_argument('--account', metavar='account')

parser_displayaddress = subparsers.add_parser('displayaddress', help='display address on signer')
parser_displayaddress.add_argument('--desc', metavar='desc')
parser_displayaddress.set_defaults(func=displayaddress)

parser_signtx = subparsers.add_parser('signtx')
parser_signtx.add_argument('psbt', metavar='psbt')

parser_signtx.set_defaults(func=signtx)

if not sys.stdin.isatty():
    buffer = sys.stdin.read()
    if buffer and buffer.rstrip() != "":
        sys.argv.extend(buffer.rstrip().split(" "))

args = parser.parse_args()

perform_pre_checks()

args.func(args)
