#!/usr/bin/env python3
# Copyright (c) 2015-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Utilities for manipulating blocks and transactions."""

import struct
import time
import unittest

from .address import (
    address_to_scriptpubkey,
    key_to_p2sh_p2wpkh,
    key_to_p2wpkh,
    script_to_p2sh_p2wsh,
    script_to_p2wsh,
)
from .messages import (
    CBlock,
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    MAX_BLOCK_WEIGHT,
    SEQUENCE_FINAL,
    hash256,
    ser_uint256,
    tx_from_hex,
    uint256_from_compact,
    WITNESS_SCALE_FACTOR,
    MAX_SEQUENCE_NONFINAL,
)
from .script import (
    CScript,
    CScriptNum,
    CScriptOp,
    OP_0,
    OP_1,
    OP_RETURN,
)
from .script_util import (
    key_to_p2wpkh_script,
    keys_to_multisig_script,
    script_to_p2wsh_script,
)
from .util import assert_equal

MAX_BLOCK_SIGOPS = 20000
MAX_BLOCK_SIGOPS_WEIGHT = MAX_BLOCK_SIGOPS * WITNESS_SCALE_FACTOR
MAX_STANDARD_TX_SIGOPS = 4000
MAX_STANDARD_TX_WEIGHT = 400000

# Genesis block time (regtest)
TIME_GENESIS_BLOCK = 1296688602

MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60

# Coinbase transaction outputs can only be spent after this number of new blocks (network rule)
COINBASE_MATURITY = 100

# Initial non-genesis block subsidy, in whole CC.
INITIAL_BLOCK_REWARD = 15

# A full block withholds one tenth of its scheduled subsidy. Intermediate
# weights use a linear interpolation rounded down, matching consensus.
BLOCK_SUBSIDY_PENALTY_DIVISOR = 10

# From BIP141
WITNESS_COMMITMENT_HEADER = b"\xaa\x21\xa9\xed"
DETERMINISTIC_P2PK_XONLY = bytes.fromhex("79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798")

NULL_OUTPOINT = COutPoint(0, 0xffffffff)

NORMAL_GBT_REQUEST_PARAMS = {"rules": ["segwit", "typedoutputs"]}
VERSIONBITS_LAST_OLD_BLOCK_VERSION = 4
MIN_BLOCKS_TO_KEEP = 288

REGTEST_RETARGET_PERIOD = 150

REGTEST_N_BITS = 0x207fffff  # difficulty retargeting is disabled in REGTEST chainparams"
REGTEST_TARGET = 0x7fffff0000000000000000000000000000000000000000000000000000000000
assert_equal(uint256_from_compact(REGTEST_N_BITS), REGTEST_TARGET)

DIFF_1_N_BITS = 0x1d00ffff
DIFF_1_TARGET = 0x00000000ffff0000000000000000000000000000000000000000000000000000
assert_equal(uint256_from_compact(DIFF_1_N_BITS), DIFF_1_TARGET)

DIFF_4_N_BITS = 0x1c3fffc0
DIFF_4_TARGET = int(DIFF_1_TARGET / 4)
assert_equal(uint256_from_compact(DIFF_4_N_BITS), DIFF_4_TARGET)

# From BIP325
SIGNET_HEADER = b"\xec\xc7\xda\xa2"

# Number of blocks to create in temporary blockchain branch for reorg testing
FORK_LENGTH = 10

def nbits_str(nbits):
    return f"{nbits:08x}"

def target_str(target):
    return f"{target:064x}"

def create_block(hashprev=None, coinbase=None, *, ntime=None, height=None, version=None, tmpl=None, txlist=None):
    """Create a block (with regtest difficulty)."""
    block = CBlock()
    if tmpl is None:
        tmpl = {}
    block.nVersion = version or tmpl.get('version') or VERSIONBITS_LAST_OLD_BLOCK_VERSION
    block.nTime = ntime or tmpl.get('curtime') or int(time.time() + 600)
    block.hashPrevBlock = hashprev or int(tmpl['previousblockhash'], 0x10)
    if tmpl and tmpl.get('bits') is not None:
        block.nBits = struct.unpack('>I', bytes.fromhex(tmpl['bits']))[0]
    else:
        block.nBits = REGTEST_N_BITS
    if coinbase is None:
        coinbase = create_coinbase(height=height or tmpl["height"])
    block.vtx.append(coinbase)
    if txlist:
        for tx in txlist:
            if type(tx) is str:
                tx = tx_from_hex(tx)
            block.vtx.append(tx)
    update_block_subsidy(block, previous_non_coinbase_weight=0)
    block.hashMerkleRoot = block.calc_merkle_root()
    return block

def get_block_subsidy(height, halving_period=REGTEST_RETARGET_PERIOD):
    return INITIAL_BLOCK_REWARD * COIN >> (height // halving_period)

def get_block_subsidy_penalty(height, non_coinbase_weight, halving_period=REGTEST_RETARGET_PERIOD):
    bounded_weight = min(non_coinbase_weight, MAX_BLOCK_WEIGHT)
    return get_block_subsidy(height, halving_period) * bounded_weight // (MAX_BLOCK_WEIGHT * BLOCK_SUBSIDY_PENALTY_DIVISOR)

def update_block_subsidy(block, previous_non_coinbase_weight=None, *, height=None, halving_period=REGTEST_RETARGET_PERIOD):
    """Adjust coinbase by the penalty delta after changing a block's tx list."""
    if previous_non_coinbase_weight is None:
        previous_non_coinbase_weight = getattr(block, "_subsidy_penalty_weight", 0)
    if height is None:
        # ConnectCoin coinbases commit to height - 1 in nLockTime.
        height = block.vtx[0].nLockTime + 1
    non_coinbase_weight = sum(tx.get_weight() for tx in block.vtx[1:])
    old_penalty = get_block_subsidy_penalty(height, previous_non_coinbase_weight, halving_period)
    new_penalty = get_block_subsidy_penalty(height, non_coinbase_weight, halving_period)
    block.vtx[0].vout[0].nValue -= new_penalty - old_penalty
    block._subsidy_penalty_weight = non_coinbase_weight

def create_empty_fork(node, fork_length=FORK_LENGTH):
    '''
        Creates a fork using node's chaintip as the starting point.
        Returns a list of blocks to submit in order.
    '''
    tip = int(node.getbestblockhash(), 16)
    height = node.getblockcount()
    block_time = node.getblock(node.getbestblockhash())['time'] + 1

    blocks = []
    for _ in range(fork_length):
        block = create_block(tip, height=height + 1, ntime=block_time)
        block.solve()
        blocks.append(block)
        tip = block.hash_int
        block_time += 1
        height += 1

    return blocks

def get_witness_script(witness_root, witness_nonce):
    witness_commitment = hash256(ser_uint256(witness_root) + ser_uint256(witness_nonce))
    output_data = WITNESS_COMMITMENT_HEADER + witness_commitment
    return CScript([OP_RETURN, output_data])

def add_witness_commitment(block, nonce=0):
    """Add a witness commitment to the block's coinbase transaction.

    ConnectCoin stores the commitment as a 36-byte push in the coinbase input
    metadata instead of creating a Script output."""
    # First calculate the merkle root of the block's
    # transactions, with witnesses.
    witness_nonce = nonce
    witness_root = block.calc_witness_merkle_root()
    # witness_nonce should go to coinbase witness.
    block.vtx[0].wit.vtxinwit = [CTxInWitness()]
    block.vtx[0].wit.vtxinwit[0].scriptWitness.stack = [ser_uint256(witness_nonce)]

    # Drop the leading OP_RETURN from the historical script. The remaining
    # canonical push (0x24 || header || hash) is coinbase input metadata.
    commitment = get_witness_script(witness_root, witness_nonce)[1:]
    block.vtx[0].vin[0].scriptSig = CScript(bytes(block.vtx[0].vin[0].scriptSig) + commitment)
    block.hashMerkleRoot = block.calc_merkle_root()


def script_BIP34_coinbase_height(height, *, padding=True):
    if height <= 16:
        res = CScriptOp.encode_op_n(height)
        if padding:
            # Append dummy extraNonce to increase scriptSig size to 2 (see bad-cb-length consensus rule)
            return CScript([res, OP_0])
        return CScript([res])
    return CScript([CScriptNum(height)])


def create_coinbase(height, pubkey=None, *, script_pubkey=None, extra_output_script=None, fees=0, nValue=INITIAL_BLOCK_REWARD, halving_period=REGTEST_RETARGET_PERIOD):
    """Create a coinbase transaction.

    If pubkey is passed in, its x coordinate becomes the type-1 P2PK key.
    Otherwise a deterministic valid x-only key is used.

    If extra_output_script is given, make a 0-value output to that
    script. This is useful to pad block weight/sigops as needed. """
    coinbase = CTransaction()
    coinbase.nLockTime = height - 1
    coinbase.vin.append(CTxIn(NULL_OUTPOINT, script_BIP34_coinbase_height(height), MAX_SEQUENCE_NONFINAL))
    coinbaseoutput = CTxOut()
    coinbaseoutput.nValue = nValue * COIN
    if nValue == INITIAL_BLOCK_REWARD:
        halvings = int(height / halving_period)
        coinbaseoutput.nValue >>= halvings
        coinbaseoutput.nValue += fees
    if pubkey is not None:
        coinbaseoutput.scriptPubKey = CScript([OP_1, pubkey[1:33]])
    elif script_pubkey is not None:
        coinbaseoutput.scriptPubKey = script_pubkey
    else:
        coinbaseoutput.scriptPubKey = CScript([OP_1, DETERMINISTIC_P2PK_XONLY])
    coinbase.vout = [coinbaseoutput]
    if extra_output_script is not None:
        coinbaseoutput2 = CTxOut()
        coinbaseoutput2.nValue = 0
        coinbaseoutput2.scriptPubKey = extra_output_script
        coinbase.vout.append(coinbaseoutput2)
    return coinbase

def create_tx_with_script(prevtx, n, script_sig=b"", *, amount, output_script=None):
    """Return one-input, one-output transaction object
       spending the prevtx's n-th output with the given amount.

       Can optionally pass scriptPubKey and scriptSig, default is anyone-can-spend output.
    """
    if output_script is None:
        output_script = CScript()
    tx = CTransaction()
    assert n < len(prevtx.vout)
    tx.vin.append(CTxIn(COutPoint(prevtx.txid_int, n), script_sig, SEQUENCE_FINAL))
    tx.vout.append(CTxOut(amount, output_script))
    return tx

def get_legacy_sigopcount_block(block, accurate=True):
    count = 0
    for tx in block.vtx:
        count += get_legacy_sigopcount_tx(tx, accurate)
    return count

def get_legacy_sigopcount_tx(tx, accurate=True):
    count = 0
    for i in tx.vout:
        count += i.scriptPubKey.GetSigOpCount(accurate)
    for j in tx.vin:
        # scriptSig might be of type bytes, so convert to CScript for the moment
        count += CScript(j.scriptSig).GetSigOpCount(accurate)
    return count

def witness_script(use_p2wsh, pubkey):
    """Create a scriptPubKey for a pay-to-witness TxOut.

    This is either a P2WPKH output for the given pubkey, or a P2WSH output of a
    1-of-1 multisig for the given pubkey. Returns the hex encoding of the
    scriptPubKey."""
    if not use_p2wsh:
        # P2WPKH instead
        pkscript = key_to_p2wpkh_script(pubkey)
    else:
        # 1-of-1 multisig
        witness_script = keys_to_multisig_script([pubkey])
        pkscript = script_to_p2wsh_script(witness_script)
    return pkscript.hex()

def create_witness_tx(node, use_p2wsh, utxo, pubkey, encode_p2sh, amount):
    """Return a transaction (in hex) that spends the given utxo to a segwit output.

    Optionally wrap the segwit output using P2SH."""
    if use_p2wsh:
        program = keys_to_multisig_script([pubkey])
        addr = script_to_p2sh_p2wsh(program) if encode_p2sh else script_to_p2wsh(program)
    else:
        addr = key_to_p2sh_p2wpkh(pubkey) if encode_p2sh else key_to_p2wpkh(pubkey)
    if not encode_p2sh:
        assert_equal(address_to_scriptpubkey(addr).hex(), witness_script(use_p2wsh, pubkey))
    return node.createrawtransaction([utxo], {addr: amount})

def send_to_witness(use_p2wsh, node, utxo, pubkey, encode_p2sh, amount, sign=True, insert_redeem_script=""):
    """Create a transaction spending a given utxo to a segwit output.

    The output corresponds to the given pubkey: use_p2wsh determines whether to
    use P2WPKH or P2WSH; encode_p2sh determines whether to wrap in P2SH.
    sign=True will have the given node sign the transaction.
    insert_redeem_script will be added to the scriptSig, if given."""
    tx_to_witness = create_witness_tx(node, use_p2wsh, utxo, pubkey, encode_p2sh, amount)
    if (sign):
        signed = node.signrawtransactionwithwallet(tx_to_witness)
        assert "errors" not in signed
        return node.sendrawtransaction(signed["hex"])
    else:
        if (insert_redeem_script):
            tx = tx_from_hex(tx_to_witness)
            tx.vin[0].scriptSig += CScript([bytes.fromhex(insert_redeem_script)])
            tx_to_witness = tx.serialize().hex()

    return node.sendrawtransaction(tx_to_witness)

class TestFrameworkBlockTools(unittest.TestCase):
    def test_create_block_prefers_explicit_height(self):
        block = create_block(
            hashprev=1,
            tmpl={"height": 100},
            height=200,
        )
        assert_equal(CScriptNum.decode(block.vtx[0].vin[0].scriptSig), 200)

    def test_create_coinbase(self):
        height = 20
        coinbase_tx = create_coinbase(height=height)
        assert_equal(CScriptNum.decode(coinbase_tx.vin[0].scriptSig), height)
