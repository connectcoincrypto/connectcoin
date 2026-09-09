#!/usr/bin/env python3
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Exercise descriptor imports for ConnectCoin's native type-1 outputs.

The Bitcoin Script/P2SH/P2WSH/multisig spending and older() cases do not
describe a ConnectCoin output. They are explicit rejection tests here, not
silently skipped. Their applicable ownership, keypool, persistence, rescan,
and private-key safety coverage uses key-path-only tr() and rawtr().
"""

import concurrent.futures
from decimal import Decimal
import threading
import time

from test_framework.address import output_key_to_p2tr
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.descriptors import descsum_create
from test_framework.extendedkey import ExtendedPrivateKey
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, JSONRPCException
from test_framework.wallet import MiniWallet
from test_framework.wallet_util import get_generate_key, test_address


NATIVE_ONLY = "ConnectCoin wallets accept only key-path-only tr() or rawtr() type-1 descriptors"
MISSING_KEYS_WARNING = "Not all private keys provided. Some wallet functionality may return unexpected errors"


class ImportDescriptorsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.noban_tx_relay = True
        self.extra_args = [[], ["-keypool=5"]]
        self.setup_clean_chain = True
        self.wallet_names = []

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def wallet(self, name, *, private=True, node=1, **kwargs):
        self.nodes[node].createwallet(name, blank=True, disable_private_keys=not private, **kwargs)
        return self.nodes[node].get_wallet_rpc(name)

    def check_import(self, wallet, request, *, error=None, warnings=()):
        result = wallet.importdescriptors([request])
        assert_equal(len(result), 1)
        assert_equal(result[0]["success"], error is None)
        assert_equal(sorted(result[0].get("warnings", [])), sorted(warnings))
        if error is not None:
            assert_equal(result[0]["error"], {"code": error[0], "message": error[1]})
        return result[0]

    def test_validation_and_labels(self):
        self.log.info("Descriptor validation, ownership, labels, and ordered per-item errors")
        watch = self.wallet("validation_watch", private=False)
        private = self.wallet("validation_private")
        key = get_generate_key()
        pubdesc = descsum_create(f"rawtr({key.pubkey})")
        privdesc = descsum_create(f"rawtr({key.privkey})")
        address = output_key_to_p2tr(bytes.fromhex(key.pubkey)[1:])
        assert_equal(self.nodes[0].deriveaddresses(pubdesc), [address])

        self.check_import(watch, {"timestamp": "now"}, error=(-8, "Descriptor not found."))
        assert_raises_rpc_error(-3, "Missing required timestamp field for key",
                               watch.importdescriptors, [{"desc": pubdesc}])
        assert_raises_rpc_error(-3, 'Expected number or "now" timestamp value for key. got type string',
                               watch.importdescriptors, [{"desc": pubdesc, "timestamp": "invalid"}])
        self.check_import(watch, {"desc": f"rawtr({key.pubkey})", "timestamp": "now"},
                          error=(-5, "Missing checksum"))
        bad_checksum = pubdesc[:-1] + ("q" if pubdesc[-1] != "q" else "p")
        self.check_import(watch, {"desc": bad_checksum, "timestamp": "now"},
                          error=(-5, f"Provided checksum '{bad_checksum.split('#')[1]}' does not match computed checksum '{pubdesc.split('#')[1]}'"))
        for whitespace_key in (f" {key.pubkey}", f"{key.pubkey} "):
            self.check_import(watch, {"desc": descsum_create(f"rawtr({whitespace_key})"), "timestamp": "now"},
                              error=(-5, f"rawtr(): Key '{whitespace_key}' is invalid due to whitespace"))

        request = {"desc": pubdesc, "timestamp": "now", "label": "Imported native key"}
        self.check_import(watch, request)
        test_address(watch, address, ismine=True, solvable=True, labels=["Imported native key"])
        self.check_import(watch, request)
        self.check_import(watch, {**request, "label": "Updated label"})
        test_address(watch, address, labels=["Updated label"])
        self.check_import(watch, {**request, "internal": True},
                          error=(-8, "Internal addresses should not have a label"))
        self.check_import(watch, {"desc": pubdesc, "timestamp": "now", "internal": True})
        # Reimporting as internal does not erase an existing address-book label.
        test_address(watch, address, ischange=False, labels=["Updated label"])
        self.check_import(watch, {**request, "internal": False})
        test_address(watch, address, ischange=False, labels=["Imported native key"])

        self.check_import(watch, {"desc": pubdesc, "timestamp": "now", "range": [0, 1]},
                          error=(-8, "Range should not be specified for an un-ranged descriptor"))
        self.check_import(watch, {"desc": pubdesc, "timestamp": "now", "active": True},
                          error=(-8, "Active descriptors must be ranged"))
        self.check_import(watch, {"desc": privdesc, "timestamp": "now"},
                          error=(-4, "Cannot import private keys to a wallet with private keys disabled"))
        self.check_import(private, {"desc": pubdesc, "timestamp": "now"},
                          error=(-4, "Cannot import descriptor without private keys to a wallet with private keys enabled"))
        self.check_import(private, {"desc": privdesc, "timestamp": "now"})
        self.check_import(private, {"desc": privdesc, "timestamp": "now"})
        test_address(private, address, ismine=True, solvable=True)

        second = get_generate_key()
        requests = [
            {"timestamp": "now"},
            {"desc": privdesc, "timestamp": 1, "label": "first"},
            {"desc": descsum_create(f"tr({second.privkey})"), "timestamp": "now", "internal": True},
            {"desc": pubdesc, "timestamp": "now", "internal": True, "label": "invalid"},
            {"desc": descsum_create(f"rawtr( {key.pubkey})"), "timestamp": "now"},
        ]
        results = private.importdescriptors(requests)
        assert_equal([entry["success"] for entry in results], [False, True, True, False, False])
        assert_equal(results[0]["error"]["message"], "Descriptor not found.")
        assert_equal(results[3]["error"]["message"], "Internal addresses should not have a label")
        assert_equal(results[4]["error"]["message"], f"rawtr(): Key ' {key.pubkey}' is invalid due to whitespace")
        assert_equal(len(private.listdescriptors()["descriptors"]), 2)

    def test_unsupported_descriptors(self):
        self.log.info("Script, multisig, script-path Taproot, and unused descriptors are explicitly rejected")
        watch = self.wallet("unsupported_watch", private=False)
        private = self.wallet("unsupported_private")
        key1, key2 = get_generate_key(), get_generate_key()
        extended = ExtendedPrivateKey.generate()
        expressions = [
            f"pk({key1.pubkey})",
            f"pkh({key1.pubkey})",
            f"wpkh({key1.pubkey})",
            f"sh(wpkh({key1.pubkey}))",
            f"multi(1,{key1.pubkey},{key2.pubkey})",
            f"sh(multi(2,{key1.pubkey},{key2.pubkey}))",
            f"wsh(multi(2,{key1.pubkey},{key2.pubkey}))",
            f"sh(wsh(multi(2,{key1.pubkey},{key2.pubkey})))",
            f"combo({key1.pubkey})",
            f"tr({key1.pubkey},pk({key2.pubkey}))",
            f"unused({extended.pubkey().to_string()})",
        ]
        # Script locktime warnings cannot make an unsupported output admissible.
        for value in (65535, 65536, (1 << 22) | 65535, (1 << 22) | 65536):
            expressions.append(f"wsh(and_v(v:pk({key1.pubkey}),older({value})))")
        for expression in expressions:
            self.check_import(watch, {"desc": descsum_create(expression), "timestamp": "now"},
                              error=(-5, NATIVE_ONLY))
        self.check_import(private, {"desc": descsum_create(f"unused({extended.to_string()})"), "timestamp": "now"},
                          error=(-5, NATIVE_ONLY))
        assert_equal(watch.listdescriptors()["descriptors"], [])
        assert_equal(private.listdescriptors()["descriptors"], [])
        assert_equal(watch.getwalletinfo()["keypoolsize"], 0)
        assert_equal(private.gethdkeys(), [])

    def test_ranges_and_persistence(self):
        self.log.info("Ranges, hardened derivation, next_index, keypool ordering and persistence")
        extended = ExtendedPrivateKey.generate()
        xpub, xpriv = extended.pubkey().to_string(), extended.to_string()
        watch = self.wallet("range_watch", private=False)
        private = self.wallet("range_private")
        desc = descsum_create(f"rawtr({xpub}/0/*)")
        request = {"desc": desc, "timestamp": "now"}
        self.check_import(watch, {**request, "label": "invalid", "range": [0, 2]},
                          error=(-8, "Ranged descriptors should not have a label"))
        self.check_import(watch, {**request, "label": "invalid"},
                          error=(-8, "Ranged descriptors should not have a label"),
                          warnings=["Range not given, using default keypool range"])
        self.check_import(watch, request, warnings=["Range not given, using default keypool range"])
        for invalid_range, message in [
            (-1, "End of range is too high"),
            ([-1, 10], "Range should be greater or equal than 0"),
            ([0, 1 << 31], "End of range is too high"),
            ([2, 1], "Range specified as [begin,end] must not have begin after end"),
            ([0, 1000001], "Range is too large"),
        ]:
            self.check_import(watch, {**request, "range": invalid_range}, error=(-8, message))
        for next_index in (-1, 6):
            self.check_import(watch, {**request, "range": [0, 5], "next_index": next_index},
                              error=(-8, "next_index is out of range"))
        self.check_import(watch, {"desc": descsum_create(f"rawtr({xpub}/1h/*)"), "timestamp": "now", "range": [0, 1]},
                          error=(-4, "Cannot expand descriptor. Probably because of hardened derivations without private keys provided"))

        private_desc = descsum_create(f"tr({xpriv}/1h/*h)")
        req = {"desc": private_desc, "timestamp": "now", "active": True}
        for interval, pool_size in [([5, 10], 6), ([0, 10], 11), ([0, 20], 21), ([0, 20], 21)]:
            self.check_import(private, {**req, "range": interval})
            assert_equal(private.getwalletinfo()["keypoolsize"], pool_size)
        for interval in ([5, 10], [0, 10], [5, 20]):
            self.check_import(private, {**req, "range": interval},
                              error=(-4, f"Could not add descriptor '{private_desc}': new range must include current range = [0,20]"))
        self.check_import(private, {**req, "range": [0, 20], "internal": True})
        assert_equal(private.getwalletinfo()["keypoolsize"], 0)
        assert_equal(private.getwalletinfo()["keypoolsize_hd_internal"], 21)
        assert_raises_rpc_error(-4, "This wallet has no available keys", private.getnewaddress)
        private.getrawchangeaddress()
        self.check_import(private, {**req, "range": [0, 20], "internal": False})
        assert_equal(private.getwalletinfo()["keypoolsize"], 21)
        assert_equal(private.getwalletinfo()["keypoolsize_hd_internal"], 0)
        assert_raises_rpc_error(-4, "This wallet has no available keys", private.getrawchangeaddress)

        # Compare derived addresses against independent Python BIP32 public keys,
        # rather than copying legacy addresses or using the RPC as its own oracle.
        origin_desc = descsum_create(f"rawtr([80002067/0h/0h]{xpub}/2/*)")
        expected = [
            output_key_to_p2tr(extended.derive_path(f"2/{i}").key.get_pubkey().get_bytes()[1:])
            for i in range(5)
        ]
        origin = {"desc": origin_desc, "timestamp": "now", "range": [0, 9], "active": True}
        self.check_import(watch, origin)
        for i, address in enumerate(expected):
            assert_equal(watch.getnewaddress(), address)
            info = watch.getaddressinfo(address)
            assert info["desc"].startswith(f"rawtr([80002067/0h/0h/2/{i}]")
        watch.keypoolrefill()
        assert_equal(watch.getwalletinfo()["keypoolsize"], 5)
        for index in (4, 0, 2, 1, 3):
            self.check_import(watch, {**origin, "range": [0, 20], "next_index": index})
            assert_equal(watch.getnewaddress(), expected[index])

        internal = {"desc": descsum_create(f"rawtr({xpub}/3/*)"), "timestamp": "now", "range": [0, 5], "internal": True}
        self.check_import(watch, internal)
        assert_raises_rpc_error(-4, "This wallet has no available keys", watch.getrawchangeaddress)
        self.check_import(watch, {**internal, "active": True})
        watch.getrawchangeaddress()
        self.check_import(watch, {**internal, "active": False})
        assert_raises_rpc_error(-4, "This wallet has no available keys", watch.getrawchangeaddress)
        before = watch.listdescriptors()["descriptors"]
        watch.unloadwallet()
        self.nodes[1].loadwallet("range_watch")
        assert_equal(watch.listdescriptors()["descriptors"], before)
        assert_raises_rpc_error(-4, "This wallet has no available keys", watch.getrawchangeaddress)
        self.check_import(watch, {**internal, "active": True})
        watch.unloadwallet()
        self.nodes[1].loadwallet("range_watch")
        watch.getrawchangeaddress()

    def test_rescan_and_spending(self):
        self.log.info("Historical native-output discovery and private/watch-only signing")
        for descriptor_type in ("rawtr", "tr"):
            key = get_generate_key()
            pubdesc = descsum_create(f"{descriptor_type}({key.pubkey})")
            privdesc = descsum_create(f"{descriptor_type}({key.privkey})")
            address = self.nodes[0].deriveaddresses(pubdesc)[0]
            txid = self.funder.sendtoaddress(address, 2)
            self.generatetoaddress(self.nodes[0], 1, self.funder.getnewaddress())
            watch = self.wallet(f"rescan_watch_{descriptor_type}", private=False)
            private = self.wallet(f"rescan_private_{descriptor_type}")
            self.check_import(watch, {"desc": pubdesc, "timestamp": 0, "label": "historical"})
            self.check_import(private, {"desc": privdesc, "timestamp": 0})
            assert_equal(watch.getbalance(), 2)
            assert_equal(private.getbalance(), 2)
            utxo = private.listunspent()[0]
            assert_equal(utxo["txid"], txid)
            unsigned = private.createrawtransaction([{"txid": txid, "vout": utxo["vout"]}],
                                                    {self.funder.getnewaddress(): Decimal("1.99")})
            assert_equal(watch.signrawtransactionwithwallet(unsigned)["complete"], False)
            signed = private.signrawtransactionwithwallet(unsigned)
            assert_equal(signed["complete"], True)
            spend = self.nodes[1].sendrawtransaction(signed["hex"], 0)
            self.sync_mempools()
            self.generatetoaddress(self.nodes[0], 1, self.funder.getnewaddress())
            assert_equal(private.gettransaction(spend)["confirmations"], 1)
            assert_equal(private.getbalance(), 0)
            assert_equal(watch.getbalance(), 0)

    def test_multipath_and_musig(self):
        self.log.info("Native multipath imports preserve the receive/change split")
        xpriv = ExtendedPrivateKey.generate().to_string()
        multipath = self.wallet("multipath")
        split = self.wallet("multipath_split")
        request = {"desc": descsum_create(f"rawtr({xpriv}/<10;20>/0/*)"), "active": True, "range": 10,
                   "timestamp": self.nodes[1].getblockheader(self.nodes[1].getbestblockhash())["time"]}
        self.check_import(multipath, {**request, "label": "invalid"},
                          error=(-8, "Multipath descriptors should not have a label"))
        self.check_import(multipath, {**request, "internal": True},
                          error=(-5, "Cannot have multipath descriptor while also specifying 'internal'"))
        self.check_import(multipath, request)
        for path, internal in ((10, False), (20, True)):
            self.check_import(split, {**request, "desc": descsum_create(f"rawtr({xpriv}/{path}/0/*)"), "internal": internal})
        for _ in range(10):
            assert_equal(multipath.getnewaddress(), split.getnewaddress())
            assert_equal(multipath.getrawchangeaddress(), split.getrawchangeaddress())
        assert_equal(sorted(multipath.listdescriptors()["descriptors"], key=lambda d: d["desc"]),
                     sorted(split.listdescriptors()["descriptors"], key=lambda d: d["desc"]))

        self.log.info("MuSig key-path imports distinguish complete from missing private key sets")
        wallet = self.wallet("musig_import_warnings")
        other = ExtendedPrivateKey.generate()
        xprv2, xpub2 = other.to_string(), other.pubkey().to_string()
        for ranged in (False, True):
            suffix = "/0/*" if ranged else ""
            for second_key, warnings in ((xprv2, ()), (xpub2, (MISSING_KEYS_WARNING,))):
                branch = 0 if second_key == xprv2 else 1
                expression = f"rawtr(musig({xpriv}/{branch},{second_key}/{branch}){suffix})"
                request = {"desc": descsum_create(expression), "timestamp": "now"}
                if ranged:
                    request["range"] = [0, 1]
                self.check_import(wallet, request, warnings=warnings)

    def test_rescan_reservation_and_encryption(self):
        self.log.info("Concurrent imports, abort, encryption and relock safety during a rescan")
        extended = ExtendedPrivateKey.generate()
        xpriv = extended.to_string()
        slow = [{"desc": descsum_create(f"rawtr({xpriv}/0h/*h)"), "timestamp": 0, "range": [0, 10000]}]
        conflicting = [{"desc": descsum_create(f"rawtr({xpriv}/1h/*h)"), "timestamp": 0, "range": [0, 10000]}]
        # Reuse 1,000 relevant transactions for exclusion, abort, and encrypted
        # rescans. Mining 2,000 almost-empty blocks would do equivalent wallet
        # database work but spend minutes hashing with RandomX.
        node = self.nodes[0]
        mini = MiniWallet(node)
        self.funder.sendtoaddress(mini.get_address(), 2)
        self.generatetoaddress(node, 1, self.funder.getnewaddress())
        mini.rescan_utxos()
        split = mini.send_self_transfer_multi(from_node=node, num_outputs=1000, fee_per_output=300_000)
        self.generatetoaddress(node, 1, self.funder.getnewaddress())
        # The second funded child is outside the imported range. Finding the
        # first must extend the hardened keypool during the rescan, so losing
        # access to private keys would miss the second half of the payments.
        public_keys = [[extended.derive_path(f"{branch}h/{child}h").key.get_pubkey().get_bytes()[1:]
                        for branch in range(2)] for child in (10000, 10001)]
        expected_balance = 0
        for index, utxo in enumerate(split["new_utxos"], start=1):
            tx = CTransaction()
            tx.vin = [CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"]), nSequence=0xfffffffd)]
            amount = (int(utxo["value"] * COIN) - 600_000) // 2
            tx.vout = [CTxOut(amount, b"\x51\x20" + key) for key in public_keys[(index - 1) // 500]]
            mini.sign_tx(tx, utxos_to_spend=[utxo])
            node.sendrawtransaction(tx.serialize().hex())
            expected_balance += amount
            if index % 500 == 0:
                assert_equal(len(node.getrawmempool()), 500)
                block = self.generatetoaddress(node, 1, self.funder.getnewaddress())[0]
                assert_equal(len(node.getblock(block)["tx"]), 501)

        self.wallet("rescan_wallet", node=0)

        def rpc(name):
            return self.nodes[0].create_new_rpc_connection(mode="AUTHPROXY") / f"wallet/{name}"

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as threads:
            start = threading.Barrier(3)

            def import_after_barrier(wallet, requests):
                start.wait(timeout=10 * self.options.timeout_factor)
                return wallet.importdescriptors(requests)

            futures = [threads.submit(import_after_barrier, rpc("rescan_wallet"), request)
                       for request in (slow, conflicting)]
            start.wait(timeout=10 * self.options.timeout_factor)
            successes, errors = 0, 0
            for future in concurrent.futures.as_completed(futures, timeout=120 * self.options.timeout_factor):
                try:
                    assert_equal(future.result(), [{"success": True}])
                    successes += 1
                except JSONRPCException as error:
                    assert_equal(error.error, {"code": -4, "message": "Wallet is currently rescanning. Abort existing rescan or wait."})
                    errors += 1
            assert_equal((successes, errors), (1, 1))
        self.check_import(rpc("rescan_wallet"),
                          {"desc": descsum_create(f"rawtr({get_generate_key().privkey})"), "timestamp": "now"})

        self.wallet("abort_import_wallet", node=0)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as threads:
            importing = threads.submit(rpc("abort_import_wallet").importdescriptors, slow)
            abort_rpc = rpc("abort_import_wallet")
            aborted = False
            deadline = time.monotonic() + 120 * self.options.timeout_factor
            # An abort before ScanForWalletTransactions starts is reset by the
            # scan loop, so continue until the importing RPC has actually ended.
            while not importing.done() and time.monotonic() < deadline:
                aborted = abort_rpc.abortrescan() or aborted
                time.sleep(0.005)
            assert_equal(aborted, True)
            assert_raises_rpc_error(-1, "Rescan aborted by user.", importing.result,
                                   timeout=120 * self.options.timeout_factor)
        # An aborted scan must release its reservation.
        self.check_import(abort_rpc, {"desc": slow[0]["desc"], "timestamp": "now", "range": [0, 10000]})

        encrypted = self.wallet("encrypted", node=0, passphrase="test passphrase")
        unlocked = self.wallet("unencrypted", node=0)
        assert_equal(unlocked.importdescriptors([]), [])
        assert_raises_rpc_error(-13, "Please enter the wallet passphrase", encrypted.importdescriptors, slow)
        assert_raises_rpc_error(-13, "Please enter the wallet passphrase", encrypted.importdescriptors, [])
        encrypted.walletpassphrase("test passphrase", 99999)
        assert_equal(encrypted.importdescriptors([]), [])
        # Leave enough real time to dispatch the import even on loaded CI;
        # the scheduler is advanced only after its reservation is established.
        encrypted.walletpassphrase("test passphrase", 60)
        genesis = self.nodes[0].getblockhash(0)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as threads:
            with self.nodes[0].busy_wait_for_debug_log(
                    expected_msgs=[f"[encrypted] Rescan started from block {genesis}...".encode()], timeout=120):
                importing = threads.submit(rpc("encrypted").importdescriptors, slow)
            control = rpc("encrypted")
            assert_raises_rpc_error(-4, "Please call `abortrescan` before locking",
                                   control.walletlock)
            assert_raises_rpc_error(-4, "before changing the passphrase", control.walletpassphrasechange,
                                   "test passphrase", "changed")
            # Make the queued relock callback due now, rather than hoping a
            # timer expires before a fast machine finishes scanning.
            # The later block requires hardened children outside the initially
            # imported range; its discovery proves keys remain available.
            node.mockscheduler(61)
            assert_equal(importing.result(timeout=120 * self.options.timeout_factor), [{"success": True}])
        # Compare exact discoveries with an independently rescanned watch-only
        # wallet and the constructed funding amounts.
        reference = self.wallet("encrypted_reference", private=False, node=0)
        assert_equal(reference.importdescriptors([
            {"desc": descsum_create(f"rawtr({keys[0].hex()})"), "timestamp": 0} for keys in public_keys
        ]), [{"success": True}, {"success": True}])
        assert_equal(encrypted.getbalances()["mine"], reference.getbalances()["mine"])
        assert_equal(encrypted.getbalance(), Decimal(expected_balance) / COIN)
        assert_equal(len(encrypted.listunspent()), 1000)
        assert encrypted.listdescriptors()["descriptors"][0]["range"][1] >= 10001
        self.wait_until(lambda: encrypted.getwalletinfo()["unlocked_until"] == 0)
        assert_raises_rpc_error(-13, "Please enter the wallet passphrase", encrypted.importdescriptors, [])
        encrypted.walletpassphrase("test passphrase", 60)
        encrypted.walletpassphrasechange("test passphrase", "changed")
        encrypted.walletlock()
        assert_raises_rpc_error(-14, "wallet passphrase entered was incorrect", encrypted.walletpassphrase,
                               "test passphrase", 60)
        encrypted.walletpassphrase("changed", 60)
        encrypted.walletlock()

    def run_test(self):
        self.nodes[0].createwallet("funder")
        self.funder = self.nodes[0].get_wallet_rpc("funder")
        self.generatetoaddress(self.nodes[0], COINBASE_MATURITY + 1, self.funder.getnewaddress())
        self.test_validation_and_labels()
        self.test_unsupported_descriptors()
        self.test_ranges_and_persistence()
        self.test_rescan_and_spending()
        self.test_multipath_and_musig()
        self.test_rescan_reservation_and_encryption()


if __name__ == "__main__":
    ImportDescriptorsTest(__file__).main()
