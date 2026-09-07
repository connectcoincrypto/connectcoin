#!/usr/bin/env python3
# Copyright (c) 2014-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the rawtransaction RPCs.

Test the following RPCs:
   - getrawtransaction
   - createrawtransaction
   - signrawtransactionwithwallet
   - sendrawtransaction
   - decoderawtransaction
"""

from collections import OrderedDict
from decimal import Decimal
from itertools import product

from test_framework.messages import (
    MAX_BIP125_RBF_SEQUENCE,
    TX_MAX_STANDARD_VERSION,
    TX_MIN_STANDARD_VERSION,
    CTxOut,
    tx_from_hex,
)
from test_framework.script import (
    CScript,
    OP_FALSE,
    OP_INVALIDOPCODE,
    OP_RETURN,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
    sync_txindex,
)
from test_framework.wallet import (
    getnewdestination,
    MiniWallet,
)


TXID = "1d1d4e24ed99057e84c3f80fd8fbec79ed9e1acee37da269356ecea000000000"


class multidict(dict):
    """Dictionary that allows duplicate keys.

    Constructed with a list of (key, value) tuples. When dumped by the json module,
    will output invalid json with repeated keys, eg:
    >>> json.dumps(multidict([(1,2),(1,2)])
    '{"1": 2, "1": 2}'

    Used to test calls to rpc methods with repeated keys in the json object."""

    def __init__(self, x):
        dict.__init__(self, x)
        self.x = x

    def items(self):
        return self.x


class RawTransactionsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.extra_args = [
            ["-txindex"],
            [],
            ["-fastprune", "-prune=1"],
        ]
        # whitelist peers to speed up tx relay / mempool sync
        self.noban_tx_relay = True
        self.supports_cli = False

    def setup_network(self):
        super().setup_network()
        self.connect_nodes(0, 2)

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0])

        self.getrawtransaction_tests()
        self.createrawtransaction_tests()
        self.sendrawtransaction_tests()
        self.sendrawtransaction_testmempoolaccept_tests()
        self.decoderawtransaction_tests()
        self.transaction_version_number_tests()
        self.getrawtransaction_verbosity_tests()

    def getrawtransaction_tests(self):
        tx = self.wallet.send_self_transfer(from_node=self.nodes[0])
        self.generate(self.nodes[0], 1)
        txId = tx['txid']
        err_msg = (
            "No such mempool transaction. Use -txindex or provide a block hash to enable"
            " blockchain transaction queries. Use gettransaction for wallet transactions."
        )

        for n in [0, 2]:
            self.log.info(f"Test getrawtransaction {'with' if n == 0 else 'without'} -txindex")

            if n == 0:
                sync_txindex(self, self.nodes[n])
                # With -txindex.
                # 1. valid parameters - only supply txid
                assert_equal(self.nodes[n].getrawtransaction(txId), tx['hex'])

                # 2. valid parameters - supply txid and 0 for non-verbose
                assert_equal(self.nodes[n].getrawtransaction(txId, 0), tx['hex'])

                # 3. valid parameters - supply txid and False for non-verbose
                assert_equal(self.nodes[n].getrawtransaction(txId, False), tx['hex'])

                # 4. valid parameters - supply txid and 1 for verbose.
                # We only check the "hex" field of the output so we don't need to update this test every time the output format changes.
                assert_equal(self.nodes[n].getrawtransaction(txId, 1)["hex"], tx['hex'])
                assert_equal(self.nodes[n].getrawtransaction(txId, 2)["hex"], tx['hex'])

                # 5. valid parameters - supply txid and True for non-verbose
                assert_equal(self.nodes[n].getrawtransaction(txId, True)["hex"], tx['hex'])
            else:
                # Without -txindex, expect to raise.
                for verbose in [None, 0, False, 1, True]:
                    assert_raises_rpc_error(-5, err_msg, self.nodes[n].getrawtransaction, txId, verbose)

            # 6. invalid parameters - supply txid and invalid boolean values (strings) for verbose
            for value in ["True", "False"]:
                assert_raises_rpc_error(-3, "not of expected type number", self.nodes[n].getrawtransaction, txid=txId, verbose=value)
                assert_raises_rpc_error(-3, "not of expected type number", self.nodes[n].getrawtransaction, txid=txId, verbosity=value)

            # 7. invalid parameters - supply txid and empty array
            assert_raises_rpc_error(-3, "not of expected type number", self.nodes[n].getrawtransaction, txId, [])

            # 8. invalid parameters - supply txid and empty dict
            assert_raises_rpc_error(-3, "not of expected type number", self.nodes[n].getrawtransaction, txId, {})

        # Make a tx by sending, then generate 2 blocks; block1 has the tx in it
        tx = self.wallet.send_self_transfer(from_node=self.nodes[2])['txid']
        block1, block2 = self.generate(self.nodes[2], 2)
        for n in [0, 2]:
            self.log.info(f"Test getrawtransaction {'with' if n == 0 else 'without'} -txindex, with blockhash")
            # We should be able to get the raw transaction by providing the correct block
            gottx = self.nodes[n].getrawtransaction(txid=tx, verbose=True, blockhash=block1)
            assert_equal(gottx['txid'], tx)
            assert_equal(gottx['in_active_chain'], True)
            if n == 0:
                self.log.info("Test getrawtransaction with -txindex, without blockhash: 'in_active_chain' should be absent")
                for v in [1,2]:
                    gottx = self.nodes[n].getrawtransaction(txid=tx, verbosity=v)
                    assert_equal(gottx['txid'], tx)
                    assert 'in_active_chain' not in gottx
            else:
                self.log.info("Test getrawtransaction without -txindex, without blockhash: expect the call to raise")
                assert_raises_rpc_error(-5, err_msg, self.nodes[n].getrawtransaction, txid=tx, verbose=True)
            # We should not get the tx if we provide an unrelated block
            assert_raises_rpc_error(-5, "No such transaction found", self.nodes[n].getrawtransaction, txid=tx, blockhash=block2)
            # An invalid block hash should raise the correct errors
            assert_raises_rpc_error(-3, "JSON value of type bool is not of expected type string", self.nodes[n].getrawtransaction, txid=tx, blockhash=True)
            assert_raises_rpc_error(-8, "parameter 3 must be of length 64 (not 6, for 'foobar')", self.nodes[n].getrawtransaction, txid=tx, blockhash="foobar")
            assert_raises_rpc_error(-8, "parameter 3 must be of length 64 (not 8, for 'abcd1234')", self.nodes[n].getrawtransaction, txid=tx, blockhash="abcd1234")
            foo = "ZZZ0000000000000000000000000000000000000000000000000000000000000"
            assert_raises_rpc_error(-8, f"parameter 3 must be hexadecimal string (not '{foo}')", self.nodes[n].getrawtransaction, txid=tx, blockhash=foo)
            bar = "0000000000000000000000000000000000000000000000000000000000000000"
            assert_raises_rpc_error(-5, "Block hash not found", self.nodes[n].getrawtransaction, txid=tx, blockhash=bar)
            # Undo the blocks and verify that "in_active_chain" is false.
            self.nodes[n].invalidateblock(block1)
            gottx = self.nodes[n].getrawtransaction(txid=tx, verbose=True, blockhash=block1)
            assert_equal(gottx['in_active_chain'], False)
            self.nodes[n].reconsiderblock(block1)
            assert_equal(self.nodes[n].getbestblockhash(), block2)

        self.log.info("Test getrawtransaction returns the spendable genesis coinbase")
        block = self.nodes[0].getblock(self.nodes[0].getblockhash(0))
        genesis_tx = self.nodes[0].getrawtransaction(block['merkleroot'], True)
        assert_equal(genesis_tx['txid'], block['merkleroot'])

    def getrawtransaction_verbosity_tests(self):
        tx = self.wallet.send_self_transfer(from_node=self.nodes[1])['txid']
        [block1] = self.generate(self.nodes[1], 1)
        fields = [
            'blockhash',
            'blocktime',
            'confirmations',
            'hash',
            'hex',
            'in_active_chain',
            'locktime',
            'size',
            'time',
            'txid',
            'vin',
            'vout',
            'vsize',
            'weight',
        ]
        prevout_fields = [
            'generated',
            'height',
            'value',
            'scriptPubKey',
        ]
        script_pub_key_fields = [
            'address',
            'asm',
            'hex',
            'type',
        ]
        # node 0 & 2 with verbosity 1 & 2
        for n, v in product([0, 2], [1, 2]):
            self.log.info(f"Test getrawtransaction_verbosity {v} {'with' if n == 0 else 'without'} -txindex, with blockhash")
            gottx = self.nodes[n].getrawtransaction(txid=tx, verbosity=v, blockhash=block1)
            missing_fields = set(fields).difference(gottx.keys())
            if missing_fields:
                raise AssertionError(f"fields {', '.join(missing_fields)} are not in transaction")

            assert len(gottx['vin']) > 0
            if v == 1:
                assert 'fee' not in gottx
                assert 'prevout' not in gottx['vin'][0]
            if v == 2:
                assert isinstance(gottx['fee'], Decimal)
                assert 'prevout' in gottx['vin'][0]
                prevout = gottx['vin'][0]['prevout']
                script_pub_key = prevout['scriptPubKey']

                missing_fields = set(prevout_fields).difference(prevout.keys())
                if missing_fields:
                    raise AssertionError(f"fields {', '.join(missing_fields)} are not in transaction")

                missing_fields = set(script_pub_key_fields).difference(script_pub_key.keys())
                if missing_fields:
                    raise AssertionError(f"fields {', '.join(missing_fields)} are not in transaction")

        # check verbosity 2 without blockhash but with txindex
        assert 'fee' in self.nodes[0].getrawtransaction(txid=tx, verbosity=2)
        # check that coinbase has no fee or does not throw any errors for verbosity 2
        coin_base = self.nodes[1].getblock(block1)['tx'][0]
        gottx = self.nodes[1].getrawtransaction(txid=coin_base, verbosity=2, blockhash=block1)
        assert 'fee' not in gottx
        # check that verbosity 2 for a mempool tx will fallback to verbosity 1
        # Do this with a pruned chain, as a regression test for https://github.com/bitcoin/bitcoin/pull/29003
        self.generate(self.nodes[2], 400)
        assert_greater_than(self.nodes[2].pruneblockchain(250), 0)
        mempool_tx = self.wallet.send_self_transfer(from_node=self.nodes[2])['txid']
        gottx = self.nodes[2].getrawtransaction(txid=mempool_tx, verbosity=2)
        assert 'fee' not in gottx

    def createrawtransaction_tests(self):
        self.log.info("Test createrawtransaction")
        # Test `createrawtransaction` required parameters
        assert_raises_rpc_error(-1, "createrawtransaction", self.nodes[0].createrawtransaction)
        assert_raises_rpc_error(-1, "createrawtransaction", self.nodes[0].createrawtransaction, [])

        # Test `createrawtransaction` invalid extra parameters
        assert_raises_rpc_error(-1, "createrawtransaction", self.nodes[0].createrawtransaction, [], {}, 0, False, 2, 3, 'foo')

        # Test `createrawtransaction` invalid version parameters
        assert_raises_rpc_error(-8, f"Invalid parameter, version out of range({TX_MIN_STANDARD_VERSION}~{TX_MAX_STANDARD_VERSION})", self.nodes[0].createrawtransaction, [], {}, 0, False, TX_MIN_STANDARD_VERSION - 1)
        assert_raises_rpc_error(-8, f"Invalid parameter, version out of range({TX_MIN_STANDARD_VERSION}~{TX_MAX_STANDARD_VERSION})", self.nodes[0].createrawtransaction, [], {}, 0, False, TX_MAX_STANDARD_VERSION + 1)

        # Test `createrawtransaction` invalid `inputs`
        assert_raises_rpc_error(-3, "JSON value of type string is not of expected type array", self.nodes[0].createrawtransaction, 'foo', {})
        assert_raises_rpc_error(-3, "JSON value of type string is not of expected type object", self.nodes[0].createrawtransaction, ['foo'], {})
        assert_raises_rpc_error(-3, "JSON value of type null is not of expected type string", self.nodes[0].createrawtransaction, [{}], {})
        assert_raises_rpc_error(-8, "txid must be of length 64 (not 3, for 'foo')", self.nodes[0].createrawtransaction, [{'txid': 'foo'}], {})
        txid = "ZZZ7bb8b1697ea987f3b223ba7819250cae33efacb068d23dc24859824a77844"
        assert_raises_rpc_error(-8, f"txid must be hexadecimal string (not '{txid}')", self.nodes[0].createrawtransaction, [{'txid': txid}], {})
        assert_raises_rpc_error(-8, "Invalid parameter, missing vout key", self.nodes[0].createrawtransaction, [{'txid': TXID}], {})
        assert_raises_rpc_error(-8, "Invalid parameter, missing vout key", self.nodes[0].createrawtransaction, [{'txid': TXID, 'vout': 'foo'}], {})
        assert_raises_rpc_error(-8, "Invalid parameter, vout cannot be negative", self.nodes[0].createrawtransaction, [{'txid': TXID, 'vout': -1}], {})
        # sequence number out of range
        for invalid_seq in [-1, 4294967296]:
            inputs = [{'txid': TXID, 'vout': 1, 'sequence': invalid_seq}]
            address = getnewdestination()[2]
            outputs = {address: 1}
            assert_raises_rpc_error(-8, 'Invalid parameter, sequence number is out of range',
                                    self.nodes[0].createrawtransaction, inputs, outputs)
        # with valid sequence number
        for valid_seq in [1000, 4294967294]:
            inputs = [{'txid': TXID, 'vout': 1, 'sequence': valid_seq}]
            address = getnewdestination()[2]
            outputs = {address: 1}
            rawtx = self.nodes[0].createrawtransaction(inputs, outputs)
            decrawtx = self.nodes[0].decoderawtransaction(rawtx)
            assert_equal(decrawtx['vin'][0]['sequence'], valid_seq)

        # Test `createrawtransaction` invalid `outputs`
        address = getnewdestination()[2]
        assert_raises_rpc_error(-8, "outputs must be an array or object", self.nodes[0].createrawtransaction, [], 'foo')
        self.nodes[0].createrawtransaction(inputs=[], outputs={})  # Should not throw for backwards compatibility
        self.nodes[0].createrawtransaction(inputs=[], outputs=[])
        assert_raises_rpc_error(-8, "ConnectCoin typed outputs do not support data/OP_RETURN outputs", self.nodes[0].createrawtransaction, [], {'data': 'foo'})
        assert_raises_rpc_error(-5, "Invalid ConnectCoin address", self.nodes[0].createrawtransaction, [], {'foo': 0})
        assert_raises_rpc_error(-3, "Invalid amount", self.nodes[0].createrawtransaction, [], {address: 'foo'})
        assert_raises_rpc_error(-3, "Amount out of range", self.nodes[0].createrawtransaction, [], {address: -1})
        assert_raises_rpc_error(-8, "Invalid parameter, duplicated address: %s" % address, self.nodes[0].createrawtransaction, [], multidict([(address, 1), (address, 1)]))
        assert_raises_rpc_error(-8, "Invalid parameter, duplicated address: %s" % address, self.nodes[0].createrawtransaction, [], [{address: 1}, {address: 1}])
        assert_raises_rpc_error(-8, "ConnectCoin typed outputs do not support data/OP_RETURN outputs", self.nodes[0].createrawtransaction, [], [{"data": 'aa'}, {"data": "bb"}])
        assert_raises_rpc_error(-8, "ConnectCoin typed outputs do not support data/OP_RETURN outputs", self.nodes[0].createrawtransaction, [], multidict([("data", 'aa'), ("data", "bb")]))
        assert_raises_rpc_error(-8, "each output must be an object containing exactly one address or p2c key", self.nodes[0].createrawtransaction, [], [{'a': 1, 'b': 2}])
        assert_raises_rpc_error(-8, "each output must be an object containing exactly one address or p2c key", self.nodes[0].createrawtransaction, [], [['key-value pair1'], ['2']])

        # Test `createrawtransaction` mismatch between sequence number(s) and `replaceable` option
        assert_raises_rpc_error(-8, "Invalid parameter combination: Sequence number(s) contradict replaceable option",
                                self.nodes[0].createrawtransaction, [{'txid': TXID, 'vout': 0, 'sequence': MAX_BIP125_RBF_SEQUENCE+1}], {}, 0, True)

        # Test `createrawtransaction` invalid `locktime`
        assert_raises_rpc_error(-3, "JSON value of type string is not of expected type number", self.nodes[0].createrawtransaction, [], {}, 'foo')
        assert_raises_rpc_error(-8, "Invalid parameter, locktime out of range", self.nodes[0].createrawtransaction, [], {}, -1)
        assert_raises_rpc_error(-8, "Invalid parameter, locktime out of range", self.nodes[0].createrawtransaction, [], {}, 4294967296)

        # Test `createrawtransaction` invalid `replaceable`
        assert_raises_rpc_error(-3, "JSON value of type string is not of expected type bool", self.nodes[0].createrawtransaction, [], {}, 0, 'foo')

        # Test that createrawtransaction accepts an array and object as outputs
        # One output
        tx = tx_from_hex(self.nodes[2].createrawtransaction(inputs=[{'txid': TXID, 'vout': 9}], outputs={address: 99}))
        assert_equal(len(tx.vout), 1)
        assert_equal(
            tx.serialize().hex(),
            self.nodes[2].createrawtransaction(inputs=[{'txid': TXID, 'vout': 9}], outputs=[{address: 99}]),
        )
        # Two outputs
        address2 = getnewdestination()[2]
        tx = tx_from_hex(self.nodes[2].createrawtransaction(inputs=[{'txid': TXID, 'vout': 9}], outputs=OrderedDict([(address, 99), (address2, 99)])))
        assert_equal(len(tx.vout), 2)
        assert_equal(
            tx.serialize().hex(),
            self.nodes[2].createrawtransaction(inputs=[{'txid': TXID, 'vout': 9}], outputs=[{address: 99}, {address2: 99}]),
        )
        # Multiple mixed native outputs, preserving their order and payload.
        p2c = {"amount": 1, "domain": "example.com", "connection_work_target": "ff" * 32, "root_certificates_version": 1}
        tx = tx_from_hex(self.nodes[2].createrawtransaction(inputs=[{'txid': TXID, 'vout': 9}], outputs=OrderedDict([(address, 99), (address2, 99), ('p2c', p2c)])))
        assert_equal(len(tx.vout), 3)
        assert_equal([output.type for output in tx.vout], [1, 1, 2])
        assert_equal(tx.vout[2].domain, b"example.com")
        assert_equal(
            tx.serialize().hex(),
            self.nodes[2].createrawtransaction(inputs=[{'txid': TXID, 'vout': 9}], outputs=[{address: 99}, {address2: 99}, {'p2c': p2c}]),
        )

        for version in range(TX_MIN_STANDARD_VERSION, TX_MAX_STANDARD_VERSION + 1):
            rawtx = self.nodes[2].createrawtransaction(inputs=[{'txid': TXID, 'vout': 9}], outputs=OrderedDict([(address, 99), (address2, 99)]), version=version)
            tx = tx_from_hex(rawtx)
            assert_equal(tx.version, version)

    def sendrawtransaction_tests(self):
        self.log.info("Test sendrawtransaction with missing input")
        inputs = [{'txid': TXID, 'vout': 1}]  # won't exist
        address = getnewdestination()[2]
        outputs = {address: 4.998}
        rawtx = self.nodes[2].createrawtransaction(inputs, outputs)
        assert_raises_rpc_error(-25, "bad-txns-inputs-missingorspent", self.nodes[2].sendrawtransaction, rawtx)

        self.log.info("Test native outputs and compatibility maxburnamount validation")
        tx = self.wallet.create_self_transfer()['tx']
        tx_hex = tx.serialize().hex()
        self.nodes[2].sendrawtransaction(hexstring=tx_hex)

        # A P2C target is data, not Script opcodes. ff and a long domain both
        # make its compatibility view fail the obsolete Script heuristic.
        for domain, target in product(["example.com", "a" * 63 + "." + "b" * 63 + ".com"], ["ff" * 32, "00" * 31 + "01"]):
            for package in [False, True]:
                transfer = self.wallet.create_self_transfer(fee_rate=Decimal("0.01"))
                tx = transfer['tx']
                recipient = self.nodes[0].createrawtransaction(
                    [{"txid": TXID, "vout": 0}],
                    [{"p2c": {"amount": 1, "domain": domain, "connection_work_target": target, "root_certificates_version": 1}}],
                )
                tx.vout[0].nValue -= tx_from_hex(recipient).vout[0].nValue
                tx.vout.append(tx_from_hex(recipient).vout[0])
                self.wallet.sign_tx(tx)
                tx_hex = tx.serialize().hex()
                assert_equal(self.nodes[2].testmempoolaccept([tx_hex])[0]['allowed'], True)
                for invalid, message in [(-1, "Amount out of range"), ("foo", "Invalid amount")]:
                    rpc = self.nodes[2].submitpackage if package else self.nodes[2].sendrawtransaction
                    assert_raises_rpc_error(-3, message, rpc, [tx_hex] if package else tx_hex, maxburnamount=invalid)
                if package:
                    assert_equal(self.nodes[2].submitpackage([tx_hex])['package_msg'], 'success')
                else:
                    assert_equal(self.nodes[2].sendrawtransaction(tx_hex), tx.txid_hex)
                assert tx.txid_hex in self.nodes[2].getrawmempool()

        # Script outputs are invalid type 0 on the wire. A high maxburnamount
        # must not make them valid, in either submission RPC.
        for script in [CScript([OP_RETURN, OP_FALSE]), CScript([OP_FALSE] * 10001), CScript([OP_INVALIDOPCODE])]:
            tx = self.wallet.create_self_transfer()['tx']
            tx.vout = [CTxOut(tx.vout[0].nValue, script)]
            tx_hex = tx.serialize().hex()
            for maxburn in [0, 100]:
                assert_raises_rpc_error(-26, 'bad-txns-vout-type', self.nodes[2].sendrawtransaction, tx_hex, maxburnamount=maxburn)
                result = self.nodes[2].submitpackage([tx_hex], maxburnamount=maxburn)
                assert_equal(result['package_msg'], 'transaction failed')
                assert_equal(result['tx-results'][tx.wtxid_hex]['error'], 'bad-txns-vout-type, output is not canonical type 1 (P2PK) or type 2 (PAY_TO_CONNECT)')

    def sendrawtransaction_testmempoolaccept_tests(self):
        self.log.info("Test sendrawtransaction/testmempoolaccept with maxfeerate")
        fee_exceeds_max = "Fee exceeds maximum configured by user (e.g. -maxtxfee, maxfeerate)"

        # Test a transaction with a small fee.
        # Keep the small fee above the current economic relay floor.
        tx = self.wallet.create_self_transfer(fee_rate=self.nodes[2].getmempoolinfo()['minrelaytxfee'] * 2)
        # Thus, testmempoolaccept should reject
        testres = self.nodes[2].testmempoolaccept([tx['hex']], 0.00000010)[0]
        assert_equal(testres['allowed'], False)
        assert_equal(testres['reject-reason'], 'max-fee-exceeded')
        # and sendrawtransaction should throw
        assert_raises_rpc_error(-25, fee_exceeds_max, self.nodes[2].sendrawtransaction, tx['hex'], 0.00000010)
        # and the following calls should both succeed
        testres = self.nodes[2].testmempoolaccept(rawtxs=[tx['hex']])[0]
        assert_equal(testres['allowed'], True)
        self.nodes[2].sendrawtransaction(hexstring=tx['hex'])

        # Test a transaction with a large fee.
        # Fee rate is 0.20000000 CC/kvB, above the default 0.1 CC/kvB limit.
        tx = self.wallet.create_self_transfer(fee_rate=Decimal("0.20000000"))
        # Thus, testmempoolaccept should reject
        testres = self.nodes[2].testmempoolaccept([tx['hex']])[0]
        assert_equal(testres['allowed'], False)
        assert_equal(testres['reject-reason'], 'max-fee-exceeded')
        # and sendrawtransaction should throw
        assert_raises_rpc_error(-25, fee_exceeds_max, self.nodes[2].sendrawtransaction, tx['hex'])
        # and the following calls should both succeed
        testres = self.nodes[2].testmempoolaccept(rawtxs=[tx['hex']], maxfeerate='0.20000000')[0]
        assert_equal(testres['allowed'], True)
        self.nodes[2].sendrawtransaction(hexstring=tx['hex'], maxfeerate='0.20000000')

        self.log.info("Test sendrawtransaction/testmempoolaccept with tx outputs already in the utxo set")
        self.generate(self.nodes[2], 1)
        for node in self.nodes:
            testres = node.testmempoolaccept([tx['hex']])[0]
            assert_equal(testres['allowed'], False)
            assert_equal(testres['reject-reason'], 'txn-already-known')
            assert_raises_rpc_error(-27, 'Transaction outputs already in utxo set', node.sendrawtransaction, tx['hex'])

    def decoderawtransaction_tests(self):
        self.log.info("Test native transaction decoding with and without witness")
        tx = self.wallet.create_self_transfer()['tx']
        encrawtx = tx.serialize_with_witness().hex()
        decoded = self.nodes[0].decoderawtransaction(encrawtx)
        assert_equal(decoded, self.nodes[0].decoderawtransaction(encrawtx, True))
        assert_equal(decoded['txid'], tx.txid_hex)
        assert_equal(decoded['hash'], tx.wtxid_hex)
        assert_equal(decoded['vout'][0]['type'], 1)
        assert_equal(decoded['vout'][0]['pubkey'], tx.vout[0].pubkey.hex())
        assert_raises_rpc_error(-22, 'TX decode failed', self.nodes[0].decoderawtransaction, encrawtx, False)
        nonwitness = self.nodes[0].decoderawtransaction(tx.serialize_without_witness().hex(), False)
        assert_equal(nonwitness['txid'], decoded['txid'])
        assert_equal(nonwitness['vout'], decoded['vout'])
        assert 'txinwitness' not in nonwitness['vin'][0]
        assert_equal(nonwitness['hash'], nonwitness['txid'])

        blockhash = self.nodes[0].getbestblockhash()
        coinbase = self.nodes[0].getblock(blockhash)['tx'][0]
        rawcoinbase = self.nodes[0].getrawtransaction(coinbase, False, blockhash)
        decoded = self.nodes[0].decoderawtransaction(rawcoinbase)
        assert_equal(decoded['txid'], coinbase)
        assert 'coinbase' in decoded['vin'][0]
        assert_equal(decoded, self.nodes[0].decoderawtransaction(rawcoinbase, True))

        # An empty Script-era output can be decoded as type 0, but cannot be
        # accepted as a valid ConnectCoin output. Decoding is not validation.
        legacy = "010000000001010000000000000072c1a6a246ae63f74f931e8365e15a089c68d61900000000000000000000ffffffff0100e1f50500000000000102616100000000"
        for mode in [None, True]:
            assert_equal(self.nodes[0].decoderawtransaction(legacy, mode)['vout'][0]['type'], 0)
        assert_raises_rpc_error(-22, 'TX decode failed', self.nodes[0].decoderawtransaction, legacy, False)
        assert_equal(self.nodes[0].testmempoolaccept([legacy])[0]['reject-reason'], 'bad-txns-vout-type')

    def transaction_version_number_tests(self):
        self.log.info("Test unsigned transaction version boundaries")
        tx = self.wallet.create_self_transfer()['tx']
        for version in [0, 0x7fffffff, 0x80000000, 0xffffffff]:
            tx.version = version
            decoded = self.nodes[0].decoderawtransaction(tx.serialize().hex())
            assert_equal(decoded['version'], version)

if __name__ == '__main__':
    RawTransactionsTest(__file__).main()
