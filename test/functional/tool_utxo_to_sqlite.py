#!/usr/bin/env python3
# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test typed UTXO snapshots, SQL encodings, and malformed-input handling."""
from contextlib import closing
from itertools import product
import os
from pathlib import Path
import platform
try:
    import sqlite3
except ImportError:
    pass
import subprocess
import sys

from test_framework.key import ECKey
from test_framework.compressor import compress_amount
from test_framework.messages import (
    COIN,
    MAX_MONEY,
    COutPoint,
    CTxOut,
    ser_compact_size,
    ser_varint,
    uint256_from_str,
)
from test_framework.crypto.muhash import MuHash3072
from test_framework.script_util import (
    output_key_to_p2tr_script,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
)
from test_framework.wallet import MiniWallet


def calculate_muhash_from_sqlite_utxos(filename, txid_format, spk_format):
    muhash = MuHash3072()
    con = sqlite3.connect(filename)
    cur = con.cursor()
    for (txid, vout, value, coinbase, height, spk) in cur.execute("SELECT * FROM utxos"):
        match txid_format:
            case "hex":
                assert type(txid) is str
                txid_bytes = bytes.fromhex(txid)[::-1]
            case "raw":
                assert type(txid) is bytes
                txid_bytes = txid
            case "rawle":
                assert type(txid) is bytes
                txid_bytes = txid[::-1]
        match spk_format:
            case "hex":
                assert type(spk) is str
                spk_bytes = bytes.fromhex(spk)
            case "raw":
                assert type(spk) is bytes
                spk_bytes = spk

        # serialize UTXO for MuHash (see function `TxOutSer` in the  coinstats module)
        utxo_ser = COutPoint(uint256_from_str(txid_bytes), vout).serialize()
        utxo_ser += (height * 2 + coinbase).to_bytes(4, 'little')
        utxo_ser += CTxOut(value, spk_bytes).serialize()
        muhash.insert(utxo_ser)
    con.close()
    return muhash.digest()[::-1].hex()


class UtxoToSqliteTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [['-coinstatsindex=1']]

    def skip_test_if_missing_module(self):
        self.skip_if_no_py_sqlite3()

    def run_test(self):
        node = self.nodes[0]
        wallet = MiniWallet(node)
        key = ECKey()
        self.generate(wallet, 101)
        self.utxo_to_sqlite_path = os.path.join(self.config["environment"]["SRCDIR"],
                                              "contrib", "utxo-tools", "utxo_to_sqlite.py")

        self.log.info('Create typed P2PK and P2C UTXOs, including a 253-byte domain')
        output_scripts = []
        for _ in range(3):
            key.generate(compressed=True)
            pubkey = key.get_pubkey().get_bytes()
            output_scripts.append(output_key_to_p2tr_script(pubkey[1:]))
        domains = (b'a', b'example.test', b'a' * 63 + b'.test', b'.'.join([b'a' * 63] * 3 + [b'b' * 61]))
        for i, domain in enumerate(domains):
            target = (bytes(32), b'\xff' * 32, bytes(range(32)), bytes(range(32, 64)))[i]
            output_scripts.append(b'\x52' + bytes([len(domain)]) + domain + target + (1).to_bytes(4, 'little'))

        sent_outputs = []
        for i, output_script in enumerate(output_scripts):
            amount = 1_000_000 * (i + 1) + i
            sent = wallet.send_to(from_node=node, scriptPubKey=output_script, amount=amount, fee=1_000_000)
            sent_outputs.append((sent['txid'], sent['sent_vout'], amount, bytes(output_script)))
        self.generate(wallet, 1)

        self.log.info('Dump UTXO set via `dumptxoutset` RPC')
        input_filename = os.path.join(self.options.tmpdir, "utxos.dat")
        dump = node.dumptxoutset(input_filename, "latest")

        # The index is built asynchronously and can still be catching up on
        # slower systems after the test has finished mining its blocks.
        self.wait_until(lambda: node.getindexinfo()['coinstatsindex']['synced'])

        stats = node.gettxoutsetinfo('muhash')
        for i, (txid_format, spk_format) in enumerate(product(["hex", "raw", "rawle"], ["hex", "raw"])):
            self.log.info(f'Test utxo-to-sqlite script using txid format "{txid_format}" and spk format "{spk_format}" ({i+1})')
            self.log.info('-> Convert UTXO set from compact-serialized format to sqlite format')
            output_filename = os.path.join(self.options.tmpdir, f"utxos_{i+1}.sqlite")
            arguments = [input_filename, output_filename, f'--txid={txid_format}', f'--spk={spk_format}']
            subprocess.run([sys.executable, self.utxo_to_sqlite_path] + arguments, check=True,
                           stderr=subprocess.STDOUT, timeout=60)

            self.log.info('-> Verify that both UTXO sets match by comparing their MuHash')
            muhash_sqlite = calculate_muhash_from_sqlite_utxos(output_filename, txid_format, spk_format)
            assert_equal(muhash_sqlite, stats['muhash'])
            with closing(sqlite3.connect(output_filename)) as con:
                assert_equal(con.execute('SELECT COUNT(*) FROM utxos').fetchone()[0], dump['coins_written'])
                for txid, vout, amount, script in sent_outputs:
                    txid_sql = txid if txid_format == 'hex' else bytes.fromhex(txid)
                    if txid_format == 'raw':
                        txid_sql = txid_sql[::-1]
                    row = con.execute('SELECT value, coinbase, height, scriptpubkey FROM utxos WHERE txid=? AND vout=?',
                                      (txid_sql, vout)).fetchone()
                    txout = node.gettxout(txid, vout)
                    assert_equal(row[0], amount, int(txout['value'] * COIN))
                    assert_equal(row[1], txout['coinbase'], False)
                    assert_equal(row[2], node.getblockcount() - txout['confirmations'] + 1)
                    script_sql = bytes.fromhex(row[3]) if spk_format == 'hex' else row[3]
                    assert_equal(script_sql, script, bytes.fromhex(txout['scriptPubKey']['hex']))

        self.test_malformed_inputs(input_filename, bytes(output_scripts[0])[2:])

        if platform.system() != "Windows":  # FIFOs are not available on Windows
            self.log.info('Convert UTXO set directly (without intermediate dump) via named pipe')
            fifo_filename = os.path.join(self.options.tmpdir, "utxos.fifo")
            os.mkfifo(fifo_filename)
            output_direct_filename = os.path.join(self.options.tmpdir, "utxos_direct.sqlite")
            p = subprocess.Popen([sys.executable, self.utxo_to_sqlite_path, fifo_filename, output_direct_filename],
                                 stderr=subprocess.STDOUT)
            try:
                target_height = node.getblockcount() - 10
                node.dumptxoutset(fifo_filename, "rollback", {"rollback": target_height})
                assert_equal(p.wait(timeout=60), 0)
                muhash_direct_sqlite = calculate_muhash_from_sqlite_utxos(output_direct_filename, "hex", "hex")
                muhash_index = node.gettxoutsetinfo('muhash', target_height)['muhash']
                assert_equal(muhash_index, muhash_direct_sqlite)
            finally:
                if p.poll() is None:
                    p.kill()
                    p.wait(timeout=10)
                os.remove(fifo_filename)

    def test_malformed_inputs(self, input_filename, pubkey):
        self.log.info('Test malformed snapshots, bounded decoding, and atomic output publication')
        directory = Path(self.options.tmpdir)
        original = Path(input_filename).read_bytes()
        # Keep the actual dump's magic/version/network/base hash; independently
        # construct the grouped Coin encoding, not using the converter's parser.
        header = original[:43]
        txid = bytes(range(32))
        p2pk = b'\x01' + pubkey

        def coin(payload=p2pk, amount=1, height_code=200, vout=0):
            return ser_compact_size(vout) + ser_varint(height_code) + ser_varint(compress_amount(amount)) + payload

        def snapshot(body, count=1):
            return header + count.to_bytes(8, 'little') + body

        def grouped(body, count=1):
            return txid + ser_compact_size(count) + body

        valid = snapshot(grouped(coin()))
        p2c = b'\x02\x01a' + bytes(range(32)) + (1).to_bytes(4, 'little')
        valid_p2c = snapshot(grouped(coin(p2c)))
        malformed = {
            'magic': b'wrong' + valid[5:],
            'version': valid[:5] + b'\x03\x00' + valid[7:],
            'zero-group': snapshot(grouped(coin(), 0)),
            'oversized-group': snapshot(grouped(coin(), 2)),
            'count-too-small': snapshot(grouped(coin()), 0),
            'count-too-large': snapshot(grouped(coin()), 2),
            'trailing': valid + b'\x00',
            'invalid-type': snapshot(grouped(coin(b'\x00'))),
            'unknown-type': snapshot(grouped(coin(b'\xff'))),
            'invalid-key': snapshot(grouped(coin(b'\x01' + b'\xff' * 32))),
            'key-not-on-curve': snapshot(grouped(coin(b'\x01' + bytes(32)))),
            'root-zero': snapshot(grouped(coin(p2c[:-4] + bytes(4)))),
            'noncanonical-group': snapshot(txid + b'\xfd\x01\x00' + coin()),
            'noncanonical-vout': snapshot(grouped(b'\xfe\x00\x01\x00\x00' + coin()[1:])),
            'oversized-vout': snapshot(grouped(coin(vout=0x04000001))),
            'height-overflow': snapshot(grouped(coin(height_code=1 << 32))),
            'amount-overflow': snapshot(grouped(b'\x00\x00' + ser_varint(1 << 64) + p2pk)),
            'amount-too-large': snapshot(grouped(coin(amount=MAX_MONEY * 2))),
            'unterminated-varint': snapshot(grouped(b'\x00' + b'\x80' * 20)),
            'real-dump-truncated': original[:-1],
        }
        for i, domain in enumerate((b'', b'Example.test', b'a.', b'a..b', b'-a', b'a-', b'a_b',
                                    b'a\x00b', b'\xff', b'a' * 64, b'a' * 254)):
            malformed[f'domain-{i}'] = snapshot(grouped(coin(b'\x02' + bytes([len(domain)]) + domain + p2c[-36:])))
        for cut in (0, 4, 5, 6, 7, 10, 11, 42, 43, 50, 51, 82, 83, 84, 85, 86, 87, len(valid) - 1):
            malformed[f'truncated-p2pk-{cut}'] = valid[:cut]
        for cut in (len(valid_p2c) - 39, len(valid_p2c) - 38, len(valid_p2c) - 37,
                    len(valid_p2c) - 36, len(valid_p2c) - 5, len(valid_p2c) - 4, len(valid_p2c) - 1):
            malformed[f'truncated-p2c-{cut}'] = valid_p2c[:cut]
        # Fail after a committed SQL batch, proving no partial output is published.
        malformed['after-batch'] = snapshot(grouped(
            b''.join(coin(vout=i) for i in range(16384)) + coin(b'\xff', vout=16384), 16385), 16385)
        for name, data in malformed.items():
            source = directory / f'bad-{name}.dat'
            destination = directory / f'bad-{name}.sqlite'
            source.write_bytes(data)
            result = subprocess.run([sys.executable, self.utxo_to_sqlite_path, str(source), str(destination)],
                                    capture_output=True, text=True, timeout=60)
            assert_equal(result.returncode, 1)
            assert 'Error:' in result.stderr, (name, result.stderr)
            assert 'Traceback' not in result.stderr, (name, result.stderr)
            assert 'TOTAL:' not in result.stdout, (name, result.stdout)
            assert not destination.exists(), name
            assert not list(directory.glob('.utxo-sqlite-*')), name

        for name, data, expected_count in (
                ('empty', snapshot(b'', 0), 0),
                ('bounds', snapshot(grouped(coin(amount=MAX_MONEY, height_code=0xffffffff, vout=0x04000000))), 1),
                ('zero-amount', snapshot(grouped(coin(amount=0))), 1)):
            source = directory / f'valid-{name}.dat'
            destination = directory / f'valid-{name}.sqlite'
            source.write_bytes(data)
            subprocess.run([sys.executable, self.utxo_to_sqlite_path, str(source), str(destination)],
                           check=True, capture_output=True, text=True, timeout=60)
            with closing(sqlite3.connect(destination)) as con:
                assert_equal(con.execute('SELECT COUNT(*) FROM utxos').fetchone()[0], expected_count)
            # Existing output must remain byte-for-byte unchanged, even when the
            # input would otherwise parse successfully.
            before = destination.read_bytes()
            result = subprocess.run([sys.executable, self.utxo_to_sqlite_path, str(source), str(destination)],
                                    capture_output=True, text=True, timeout=60)
            assert_equal(result.returncode, 1)
            assert 'already exists' in result.stderr
            assert_equal(destination.read_bytes(), before)


if __name__ == "__main__":
    UtxoToSqliteTest(__file__).main()
