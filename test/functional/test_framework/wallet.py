#!/usr/bin/env python3
# Copyright (c) 2020-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A limited-functionality wallet, which may replace a real wallet in tests"""

from copy import deepcopy
from decimal import Decimal
from enum import Enum
from typing import (
    Any,
    Optional,
)
from test_framework.address import (
    key_to_p2pkh,
    key_to_p2sh_p2wpkh,
    key_to_p2wpkh,
    output_key_to_p2tr,
)
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.descriptors import descsum_create
from test_framework.key import (
    ECKey,
    compute_xonly_pubkey,
    sign_schnorr,
)
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    hash256,
)
from test_framework.script import (
    CScript,
    OP_1,
    SIGHASH_DEFAULT,
    TaprootSignatureHash,
    taproot_construct,
)
from test_framework.script_util import (
    key_to_p2pkh_script,
    key_to_p2sh_p2wpkh_script,
    key_to_p2wpkh_script,
)
from test_framework.util import (
    assert_equal,
    assert_greater_than_or_equal,
    get_fee,
)
from test_framework.wallet_util import (
    bytes_to_wif,
    generate_keypair,
)

DEFAULT_FEE = Decimal("0.000001")

class MiniWalletMode(Enum):
    """Compatibility names for the single native type-1 MiniWallet mode.

    ConnectCoin no longer has OP_TRUE or legacy raw-script outputs. All three
    inherited enum values therefore create and spend a type-1 x-only P2PK lock.
    Tests whose purpose is legacy Script execution must be retired explicitly.
    """
    ADDRESS_OP_TRUE = 1
    RAW_OP_TRUE = 2
    RAW_P2PK = 3


class MiniWallet:
    def __init__(self, test_node, *, mode=MiniWalletMode.ADDRESS_OP_TRUE, tag_name=None):
        self._test_node = test_node
        self._utxos = []
        self._mode = mode

        assert isinstance(mode, MiniWalletMode)
        if mode != MiniWalletMode.ADDRESS_OP_TRUE:
            assert tag_name is None

        # Keep the inherited modes on distinct deterministic keys even though
        # they now share the same native type-1 output format. Several tests
        # rely on wallets for different modes not spending each other's UTXOs.
        secret = mode.value.to_bytes(32, 'big') if tag_name is None else hash256(tag_name.encode())
        self._priv_key = ECKey()
        self._priv_key.set(secret, True)
        if not self._priv_key.is_valid:
            raise ValueError("invalid deterministic MiniWallet private key")
        xonly, _ = compute_xonly_pubkey(secret)
        self._scriptPubKey = bytes(CScript([OP_1, xonly]))
        if mode == MiniWalletMode.ADDRESS_OP_TRUE:
            self._address = output_key_to_p2tr(xonly)

        # When the pre-mined test framework chain is used, it contains coinbase
        # outputs to the MiniWallet's default address in blocks 76-100
        # (see method BitcoinTestFramework._initialize_chain())
        # The MiniWallet needs to rescan_utxos() in order to account
        # for those mature UTXOs, so that all txs spend confirmed coins
        self.rescan_utxos()

    def _create_utxo(self, *, txid, vout, value, height, coinbase, confirmations):
        return {"txid": txid, "vout": vout, "value": value, "height": height, "coinbase": coinbase, "confirmations": confirmations}

    def _bulk_tx(self, tx, target_vsize):
        """Pad using dust-safe type-1 outputs when the size is representable."""
        padding_value = 1000
        current_count = len(tx.vout)

        def compact_size_length(count):
            if count < 253:
                return 1
            if count <= 0xffff:
                return 3
            if count <= 0xffffffff:
                return 5
            return 9

        # Type-1 outputs are exactly 41 non-witness bytes. Compute the final
        # count arithmetically instead of repeatedly serializing a growing
        # transaction (which is quadratic for near-block-sized tests).
        base_vsize = tx.get_vsize() - compact_size_length(current_count) - 41 * current_count
        low = current_count
        high = current_count + max(0, (target_vsize - tx.get_vsize()) // 41) + 2
        while low <= high:
            candidate = (low + high) // 2
            candidate_vsize = base_vsize + compact_size_length(candidate) + 41 * candidate
            if candidate_vsize <= target_vsize:
                low = candidate + 1
            else:
                high = candidate - 1

        final_count = high
        if base_vsize + compact_size_length(final_count) + 41 * final_count != target_vsize:
            raise RuntimeError(
                f"target_vsize {target_vsize} is not representable with fixed-size typed outputs"
            )
        additional_count = final_count - current_count
        padding_total = padding_value * additional_count
        if tx.vout[0].nValue <= padding_total:
            raise RuntimeError("transaction value is too small for typed-output padding")
        tx.vout[0].nValue -= padding_total
        tx.vout.extend(
            CTxOut(nValue=padding_value, scriptPubKey=self._scriptPubKey)
            for _ in range(additional_count)
        )
        assert_equal(tx.get_vsize(), target_vsize)


    def get_balance(self):
        return sum(u['value'] for u in self._utxos)

    def rescan_utxos(self, *, include_mempool=True):
        """Drop all utxos and rescan the utxo set"""
        self._utxos = []
        res = self._test_node.scantxoutset(action="start", scanobjects=[self.get_descriptor()])
        assert_equal(True, res['success'])
        for utxo in res['unspents']:
            self._utxos.append(
                self._create_utxo(txid=utxo["txid"],
                                  vout=utxo["vout"],
                                  value=utxo["amount"],
                                  height=utxo["height"],
                                  coinbase=utxo["coinbase"],
                                  confirmations=res["height"] - utxo["height"] + 1))
        if include_mempool:
            mempool = self._test_node.getrawmempool(verbose=True)
            # Sort tx by ancestor count. See BlockAssembler::SortForBlock in src/node/miner.cpp
            sorted_mempool = sorted(mempool.items(), key=lambda item: (item[1]["ancestorcount"], int(item[0], 16)))
            for txid, _ in sorted_mempool:
                self.scan_tx(self._test_node.getrawtransaction(txid=txid, verbose=True))

    def scan_tx(self, tx):
        """Scan the tx and adjust the internal list of owned utxos"""
        for spent in tx["vin"]:
            # Mark spent. This may happen when the caller has ownership of a
            # utxo that remained in this wallet. For example, by passing
            # mark_as_spent=False to get_utxo or by using an utxo returned by a
            # create_self_transfer* call.
            try:
                self.get_utxo(txid=spent["txid"], vout=spent["vout"])
            except StopIteration:
                pass
        for out in tx['vout']:
            if out['scriptPubKey']['hex'] == self._scriptPubKey.hex():
                self._utxos.append(self._create_utxo(txid=tx["txid"], vout=out["n"], value=out["value"], height=0, coinbase=False, confirmations=0))

    def scan_txs(self, txs):
        for tx in txs:
            self.scan_tx(tx)

    def sign_tx(self, tx, fixed_length=True, utxos_to_spend=None):
        """Authorize every input with one 64-byte SIGHASH_DEFAULT signature."""
        del fixed_length  # Schnorr signatures are always exactly 64 bytes here.
        if utxos_to_spend is None:
            utxos_to_spend = []
            for txin in tx.vin:
                coin = self._test_node.gettxout(f"{txin.prevout.hash:064x}", txin.prevout.n, True)
                if coin is None:
                    raise RuntimeError(f"cannot find prevout {txin.prevout.hash:064x}:{txin.prevout.n}")
                utxos_to_spend.append({"value": coin["value"]})

        assert_equal(len(tx.vin), len(utxos_to_spend))
        spent_outputs = [
            CTxOut(int(COIN * utxo["value"]), self._scriptPubKey)
            for utxo in utxos_to_spend
        ]
        tx.wit.vtxinwit = [CTxInWitness() for _ in tx.vin]
        for index, txin in enumerate(tx.vin):
            txin.scriptSig = b''
            sighash = TaprootSignatureHash(
                tx,
                spent_outputs,
                SIGHASH_DEFAULT,
                input_index=index,
            )
            tx.wit.vtxinwit[index].scriptWitness.stack = [
                sign_schnorr(self._priv_key.get_bytes(), sighash)
            ]

    def generate(self, num_blocks, **kwargs):
        """Generate blocks with coinbase outputs to the internal address, and call rescan_utxos"""
        blocks = self._test_node.generatetodescriptor(num_blocks, self.get_descriptor(), **kwargs)
        # Calling rescan_utxos here makes sure that after a generate the utxo
        # set is in a clean state. For example, the wallet will update
        # - if the caller consumed utxos, but never used them
        # - if the caller sent a transaction that is not mined or got rbf'd
        # - after block re-orgs
        # - the utxo height for mined mempool txs
        # - However, the wallet will not consider remaining mempool txs
        self.rescan_utxos()
        return blocks

    def get_output_script(self):
        return self._scriptPubKey

    def get_descriptor(self):
        return descsum_create(f'rawtr({self._scriptPubKey[2:].hex()})')

    def get_address(self):
        assert_equal(self._mode, MiniWalletMode.ADDRESS_OP_TRUE)
        return self._address

    def get_utxo(self, *, txid: str = '', vout: Optional[int] = None, mark_as_spent=True, confirmed_only=False) -> dict:
        """
        Returns a utxo and marks it as spent (pops it from the internal list)

        Args:
        txid: get the first utxo we find from a specific transaction
        """
        self._utxos = sorted(self._utxos, key=lambda k: (k['value'], -k['height']))  # Put the largest utxo last
        blocks_height = self._test_node.getblockchaininfo()['blocks']
        mature_coins = list(filter(lambda utxo: not utxo['coinbase'] or COINBASE_MATURITY - 1 <= blocks_height - utxo['height'], self._utxos))
        if txid:
            utxo_filter: Any = filter(lambda utxo: txid == utxo['txid'], self._utxos)
        else:
            utxo_filter = reversed(mature_coins)  # By default the largest utxo
        if vout is not None:
            utxo_filter = filter(lambda utxo: vout == utxo['vout'], utxo_filter)
        if confirmed_only:
            utxo_filter = filter(lambda utxo: utxo['confirmations'] > 0, utxo_filter)
        index = self._utxos.index(next(utxo_filter))
        if mark_as_spent:
            return self._utxos.pop(index)
        else:
            return self._utxos[index]

    def get_utxos(self, *, include_immature_coinbase=False, mark_as_spent=True, confirmed_only=False):
        """Returns the list of all utxos and optionally mark them as spent"""
        if not include_immature_coinbase:
            blocks_height = self._test_node.getblockchaininfo()['blocks']
            utxo_filter = filter(lambda utxo: not utxo['coinbase'] or COINBASE_MATURITY - 1 <= blocks_height - utxo['height'], self._utxos)
        else:
            utxo_filter = self._utxos
        if confirmed_only:
            utxo_filter = filter(lambda utxo: utxo['confirmations'] > 0, utxo_filter)
        utxos = deepcopy(list(utxo_filter))
        if mark_as_spent:
            self._utxos = []
        return utxos

    def send_self_transfer(self, *, from_node, **kwargs):
        """Call create_self_transfer and send the transaction."""
        tx = self.create_self_transfer(**kwargs)
        self.sendrawtransaction(from_node=from_node, tx_hex=tx['hex'])
        return tx

    def send_to(self, *, from_node, scriptPubKey, amount, fee=1000):
        """
        Create and send a tx with an output to a given scriptPubKey/amount,
        plus a change output to our internal address. To keep things simple, a
        fixed fee given in Satoshi is used.

        Note that this method fails if there is no single internal utxo
        available that can cover the cost for the amount and the fixed fee
        (the utxo with the largest value is taken).
        """
        tx = self.create_self_transfer(fee_rate=0)["tx"]
        assert_greater_than_or_equal(tx.vout[0].nValue, amount + fee)
        tx.vout[0].nValue -= (amount + fee)           # change output -> MiniWallet
        tx.vout.append(CTxOut(amount, scriptPubKey))  # arbitrary output -> to be returned
        # SIGHASH_DEFAULT commits to every output, so output changes require a
        # fresh signature (the historical OP_TRUE MiniWallet did not).
        self.sign_tx(tx)
        txid = self.sendrawtransaction(from_node=from_node, tx_hex=tx.serialize().hex())
        return {
            "sent_vout": 1,
            "txid": txid,
            "wtxid": tx.wtxid_hex,
            "hex": tx.serialize().hex(),
            "tx": tx,
        }

    def send_self_transfer_multi(self, *, from_node, **kwargs):
        """Call create_self_transfer_multi and send the transaction."""
        tx = self.create_self_transfer_multi(**kwargs)
        self.sendrawtransaction(from_node=from_node, tx_hex=tx["hex"])
        return tx

    def create_self_transfer_multi(
        self,
        *,
        utxos_to_spend: Optional[list[dict]] = None,
        num_outputs=1,
        amount_per_output=0,
        version=2,
        locktime=0,
        sequence=0,
        fee_per_output=1000,
        target_vsize=0,
        confirmed_only=False,
    ):
        """
        Create and return a transaction that spends the given UTXOs and creates a
        certain number of outputs with equal amounts. The output amounts can be
        set by amount_per_output or automatically calculated with a fee_per_output.
        """
        utxos_to_spend = utxos_to_spend or [self.get_utxo(confirmed_only=confirmed_only)]
        sequence = [sequence] * len(utxos_to_spend) if type(sequence) is int else sequence
        assert_equal(len(utxos_to_spend), len(sequence))

        # calculate output amount
        inputs_value_total = sum([int(COIN * utxo['value']) for utxo in utxos_to_spend])
        outputs_value_total = inputs_value_total - fee_per_output * num_outputs
        amount_per_output = amount_per_output or (outputs_value_total // num_outputs)
        assert amount_per_output > 0
        outputs_value_total = amount_per_output * num_outputs
        fee = Decimal(inputs_value_total - outputs_value_total) / COIN

        # create tx
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(utxo_to_spend['txid'], 16), utxo_to_spend['vout']), nSequence=seq) for utxo_to_spend, seq in zip(utxos_to_spend, sequence)]
        tx.vout = [CTxOut(amount_per_output, bytearray(self._scriptPubKey)) for _ in range(num_outputs)]
        tx.version = version
        tx.nLockTime = locktime

        self.sign_tx(tx, utxos_to_spend=utxos_to_spend)

        if target_vsize:
            self._bulk_tx(tx, target_vsize)
            # Added outputs change SIGHASH_DEFAULT while keeping signature size
            # fixed, so rebuild every signature after padding.
            self.sign_tx(tx, utxos_to_spend=utxos_to_spend)

        txid = tx.txid_hex
        return {
            "new_utxos": [self._create_utxo(
                txid=txid,
                vout=i,
                value=Decimal(tx.vout[i].nValue) / COIN,
                height=0,
                coinbase=False,
                confirmations=0,
            ) for i in range(len(tx.vout))],
            "fee": fee,
            "txid": txid,
            "wtxid": tx.wtxid_hex,
            "hex": tx.serialize().hex(),
            "tx": tx,
        }

    def create_self_transfer(
            self,
            *,
            fee_rate=Decimal("0.00003"),
            fee=Decimal("0"),
            utxo_to_spend=None,
            target_vsize=0,
            confirmed_only=False,
            **kwargs,
    ):
        """Create and return a tx with the specified fee. If fee is 0, use fee_rate, where the resulting fee may be exact or at most one satoshi higher than needed."""
        utxo_to_spend = utxo_to_spend or self.get_utxo(confirmed_only=confirmed_only)
        assert fee_rate >= 0
        assert fee >= 0
        # calculate fee
        vsize = Decimal(109)  # one type-1 input, one type-1 output, one Schnorr signature
        if target_vsize and not fee:  # respect fee_rate if target vsize is passed
            fee = get_fee(target_vsize, fee_rate)
        send_value = utxo_to_spend["value"] - (fee or (fee_rate * vsize / 1000))
        if send_value <= 0:
            raise RuntimeError(f"UTXO value {utxo_to_spend['value']} is too small to cover fees {(fee or (fee_rate * vsize / 1000))}")
        # create tx
        tx = self.create_self_transfer_multi(
            utxos_to_spend=[utxo_to_spend],
            amount_per_output=int(COIN * send_value),
            target_vsize=target_vsize,
            **kwargs,
        )
        if not target_vsize:
            assert_equal(tx["tx"].get_vsize(), vsize)
        tx["new_utxo"] = tx.pop("new_utxos")[0]

        return tx

    def sendrawtransaction(self, *, from_node, tx_hex, maxfeerate=0, **kwargs):
        txid = from_node.sendrawtransaction(hexstring=tx_hex, maxfeerate=maxfeerate, **kwargs)
        self.scan_tx(from_node.decoderawtransaction(tx_hex))
        return txid

    def create_self_transfer_chain(self, *, chain_length, utxo_to_spend=None):
        """
        Create a "chain" of chain_length transactions. The nth transaction in
        the chain is a child of the n-1th transaction and parent of the n+1th transaction.
        """
        chaintip_utxo = utxo_to_spend or self.get_utxo()
        chain = []

        for _ in range(chain_length):
            tx = self.create_self_transfer(utxo_to_spend=chaintip_utxo)
            chaintip_utxo = tx["new_utxo"]
            chain.append(tx)

        return chain

    def send_self_transfer_chain(self, *, from_node, **kwargs):
        """Create and send a "chain" of chain_length transactions. The nth transaction in
        the chain is a child of the n-1th transaction and parent of the n+1th transaction.

        Returns a list of objects for each tx (see create_self_transfer_multi).
        """
        chain = self.create_self_transfer_chain(**kwargs)
        for t in chain:
            self.sendrawtransaction(from_node=from_node, tx_hex=t["hex"])
        return chain


class NodeSigner:
    """Simple wallet replacement that delegates signing of existing raw transactions to a node by
       using the `signrawtransactionwithkey` RPC. This can be used for spending from widespread
       output types (P2PKH, P2WPKH, P2SH-P2WPKH, P2TR) without having the wallet compiled in."""
    def __init__(self, node):
        self._node = node
        self._key_entries = []

    def getnewaddress(self, address_type='legacy'):
        (seckey, pubkey), spk, address = getnewdestination(address_type)
        redeem_script = key_to_p2wpkh_script(pubkey) if address_type == 'p2sh-segwit' else None
        self._key_entries.append({"seckey_wif": bytes_to_wif(seckey.get_bytes()), "output_script": spk, "redeem_script": redeem_script})
        return pubkey, spk, address

    def listunspent(self):
        needles = [descsum_create(f'raw({key_entry["output_script"].hex()})') for key_entry in self._key_entries]
        scan_res = self._node.scantxoutset(action="start", scanobjects=needles)
        spend_height = scan_res['height'] + 1  # coins would be spent in the next block
        unspents = []
        for u in scan_res['unspents']:
            if u["coinbase"] and (spend_height - u["height"]) < COINBASE_MATURITY:  # skip immature coins
                continue
            unspent = { "txid": u["txid"], "vout": u["vout"], "scriptPubKey": u["scriptPubKey"], "amount": u["amount"] }
            key_entry = [ke for ke in self._key_entries if ke["output_script"] == bytes.fromhex(u["scriptPubKey"])][0]
            if key_entry["redeem_script"] is not None:
                unspent["redeemScript"] = key_entry["redeem_script"].hex()
            unspents.append(unspent)
        return unspents

    def signrawtransaction(self, tx_hex, inputs):
        output_scripts_to_sign = {i["scriptPubKey"] for i in inputs}
        seckeys_wif = [ke["seckey_wif"] for ke in self._key_entries if ke["output_script"].hex() in output_scripts_to_sign]
        return self._node.signrawtransactionwithkey(tx_hex, seckeys_wif, inputs)


def getnewdestination(address_type='bech32m'):
    """Generate a random destination of the specified type and return the
       corresponding key pair, scriptPubKey and address. Supported types are
       'legacy', 'p2sh-segwit', 'bech32' and 'bech32m'. Can be used when a random
       destination is needed, but no compiled wallet is available (e.g. as
       replacement to the getnewaddress/getaddressinfo RPCs)."""
    key, pubkey = generate_keypair()
    if address_type == 'legacy':
        scriptpubkey = key_to_p2pkh_script(pubkey)
        address = key_to_p2pkh(pubkey)
    elif address_type == 'p2sh-segwit':
        scriptpubkey = key_to_p2sh_p2wpkh_script(pubkey)
        address = key_to_p2sh_p2wpkh(pubkey)
    elif address_type == 'bech32':
        scriptpubkey = key_to_p2wpkh_script(pubkey)
        address = key_to_p2wpkh(pubkey)
    elif address_type == 'bech32m':
        tap = taproot_construct(compute_xonly_pubkey(key.get_bytes())[0])
        pubkey = tap.output_pubkey
        scriptpubkey = tap.scriptPubKey
        address = output_key_to_p2tr(pubkey)
    else:
        assert False
    return (key, pubkey), scriptpubkey, address
