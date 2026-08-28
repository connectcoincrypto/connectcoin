#!/usr/bin/env python3
# Copyright (c) 2018-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""External-signer mock that deliberately exposes no type-1 descriptors."""

import argparse
import json
import sys


def enumerate_signers(args):
    signer = {
        "fingerprint": "00000001",
        "type": "legacy-only",
        "model": "legacy-only",
    }
    if args.connectcoin_compatible:
        signer["protocol"] = "connectcoin-typed-v1"
    sys.stdout.write(json.dumps([signer]))


def get_descriptors(args):
    xpub = "tcubNC1uBHhBxmWgfPoCJ8krUwtLbnqX2uhWwmgfSt2GLbCHhPuEq4LhtLBZKLwC6YkHK2hXrPWvVMgReYFtgWxenNZAuC69MaERFZ7AygMKgQd"
    origin = f"[00000001/44h/1h/{args.account}']"
    sys.stdout.write(json.dumps({
        "receive": [f"pkh({origin}{xpub}/0/*)#r3hvwn9g"],
        "internal": [f"pkh({origin}{xpub}/1/*)#j9jdnx4s"],
    }))


parser = argparse.ArgumentParser(prog="legacy_signer.py")
parser.add_argument("--fingerprint")
parser.add_argument("--chain", default="main")
parser.add_argument("--stdin", action="store_true")
parser.add_argument("--connectcoin-compatible", action="store_true")
subparsers = parser.add_subparsers(dest="command", required=True)

enumerate_parser = subparsers.add_parser("enumerate")
enumerate_parser.set_defaults(func=enumerate_signers)

descriptors_parser = subparsers.add_parser("getdescriptors")
descriptors_parser.add_argument("--account", metavar="account")
descriptors_parser.set_defaults(func=get_descriptors)

if not sys.stdin.isatty():
    buffer = sys.stdin.read()
    if buffer.rstrip():
        sys.argv.extend(buffer.rstrip().split(" "))

args = parser.parse_args()
args.func(args)
