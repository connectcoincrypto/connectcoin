#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Restore pre-reset wallet identities without reviving old-chain balances.

Use two disconnected regtest datadirs to simulate losing the old chain. The
backup's native block locator is changed to the real pre-v2 genesis; none of
the source's blocks or UTXOs exists on the destination. Only disposable wallets
created by this test are inspected.
"""

from contextlib import closing
from decimal import Decimal
from io import BytesIO
from pathlib import Path
import shutil
try:
    import sqlite3
except ImportError:
    pass

from test_framework.messages import CBlockLocator, COIN, MAGIC_BYTES, ser_string
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_not_equal,
    assert_raises_rpc_error,
    sha256sum_file,
)
from test_framework.wallet import MiniWallet
from test_framework.wallet_util import WalletUnlock


class WalletTestnetResetRestoreTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.wallet_names = []

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_py_sqlite3()

    def setup_network(self):
        self.setup_nodes()
        # Deliberately do not connect the nodes: the destination must not learn
        # source blocks or transactions while testing a fresh-chain restore.

    def set_backup_application_id(self, backup, magic):
        backup = backup.resolve(strict=True)
        fixture_dir = self.backup_dir.resolve(strict=True)
        assert fixture_dir.is_relative_to(Path(self.options.tmpdir).resolve(strict=True))
        assert backup.is_relative_to(fixture_dir) and backup.is_file()
        # SQLite PRAGMA takes a signed int32, even when the wire identifier's
        # high bit is set. mode=rw prevents silently creating a missing file.
        application_id = int.from_bytes(magic, 'big', signed=True)
        with closing(sqlite3.connect(backup.as_uri() + '?mode=rw', uri=True)) as con:
            con.execute(f'PRAGMA application_id={application_id}')
            con.commit()
            assert_equal(con.execute('PRAGMA application_id').fetchone()[0], application_id)

    def backup_locator(self, backup, *, replace_genesis=None):
        backup = backup.resolve(strict=True)
        fixture_dir = self.backup_dir.resolve(strict=True)
        assert fixture_dir.is_relative_to(Path(self.options.tmpdir).resolve(strict=True))
        assert backup.is_relative_to(fixture_dir) and backup.is_file()
        with closing(sqlite3.connect(backup.as_uri() + '?mode=rw', uri=True)) as con:
            # ReadBestBlock prefers a nonempty legacy bestblock record. Require
            # it to be empty so the fixture cannot accidentally bypass the real
            # bestblock_nomerkle record used for chain-identity enforcement.
            bestblock = con.execute('SELECT value FROM main WHERE key=?', (ser_string(b'bestblock'),)).fetchone()
            if bestblock is not None:
                old_locator = CBlockLocator()
                old_locator.deserialize(BytesIO(bestblock[0]))
                assert_equal(old_locator.vHave, [])
            key = ser_string(b'bestblock_nomerkle')
            row = con.execute('SELECT value FROM main WHERE key=?', (key,)).fetchone()
            assert row is not None
            serialized = row[0]
            stream = BytesIO(serialized)
            locator = CBlockLocator()
            locator.deserialize(stream)
            assert locator.vHave
            assert_equal(stream.read(), b'')
            if replace_genesis is not None:
                current, legacy = replace_genesis
                assert_equal(locator.vHave[-1], int(current, 16))
                assert_not_equal(current, legacy)
                assert_equal(serialized[-32:], bytes.fromhex(current)[::-1])
                # Preserve Core's version field, CompactSize and all other
                # locator entries byte-for-byte; replace only its final hash.
                replacement = serialized[:-32] + bytes.fromhex(legacy)[::-1]
                assert_equal(con.execute('UPDATE main SET value=? WHERE key=?', (replacement, key)).rowcount, 1)
                con.commit()
                return int(legacy, 16)
            return locator.vHave[-1]

    def assert_no_old_balance(self, wallet):
        balances = wallet.getbalances()['mine']
        for field in ('trusted', 'untrusted_pending', 'immature'):
            assert_equal(balances[field], 0)
        assert_equal(wallet.getbalance(), 0)
        assert_equal(wallet.listunspent(minconf=0), [])

    def run_test(self):
        source, destination = self.nodes
        for node in self.nodes:
            assert_equal(node.getblockcount(), 0)
            assert_equal(node.getpeerinfo(), [])
        legacy_magic = bytes.fromhex('a54fc7d5')
        legacy_genesis = 'ccfa95619bae24b5045dbd91e4410c5279bc757ddad127a25c31d0258ee99342'
        current_genesis = source.getblockhash(0)
        assert_not_equal(legacy_magic, MAGIC_BYTES['regtest'])

        self.log.info('Create a disposable encrypted wallet with confirmed old-chain history')
        passphrase = 'test-only reset backup passphrase'
        source.createwallet(wallet_name='before_reset', passphrase=passphrase)
        original = source.get_wallet_rpc('before_reset')
        with WalletUnlock(original, passphrase):
            original.keypoolrefill(10)
            receiving_address = original.getnewaddress('preserved label', 'bech32m')
        address_info = original.getaddressinfo(receiving_address)

        old_miner = MiniWallet(source, tag_name='reset source miner')
        self.generate(old_miner, 1, sync_fun=self.no_op)
        source_blocks = self.generatetoaddress(source, 101, receiving_address, sync_fun=self.no_op)
        old_coinbase_txid = source.getblock(source_blocks[0])['tx'][0]
        old_miner.rescan_utxos()
        incoming = old_miner.send_to(
            from_node=source,
            scriptPubKey=bytes.fromhex(address_info['scriptPubKey']),
            amount=2 * COIN,
        )
        self.generatetoaddress(source, 1, receiving_address, sync_fun=self.no_op)
        original.syncwithvalidationinterfacequeue()
        assert_greater_than(original.getbalance(), 0)
        assert_greater_than(original.gettransaction(incoming['txid'])['confirmations'], 0)
        assert_greater_than(original.gettransaction(old_coinbase_txid)['confirmations'], 100)
        old_outpoints = {(coin['txid'], coin['vout']) for coin in original.listunspent()}
        assert old_outpoints

        self.backup_dir = source.datadir_path / 'reset-backup-fixtures'
        self.backup_dir.mkdir()
        backup = self.backup_dir / 'legacy-encrypted.sqlite'
        original.backupwallet(backup)
        # The database identity must already be independent of the new P2P
        # magic for newly created wallets as well as restored old wallets.
        with closing(sqlite3.connect(backup.as_uri() + '?mode=ro', uri=True)) as con:
            assert_equal(con.execute('PRAGMA application_id').fetchone()[0],
                         int.from_bytes(legacy_magic, 'big', signed=True))
        self.set_backup_application_id(backup, legacy_magic)
        self.backup_locator(backup, replace_genesis=(current_genesis, legacy_genesis))
        assert_equal(self.backup_locator(backup), int(legacy_genesis, 16))
        original_backup_hash = sha256sum_file(backup)
        original_descriptors = [(item['desc'], item['active'], item['internal'])
                                for item in original.listdescriptors()['descriptors']]
        # The next addresses are derived AFTER making the backup, so the
        # restored wallet must reproduce them from its saved derivation state.
        expected_next_address = original.getnewaddress('', 'bech32m')
        expected_next_change = original.getrawchangeaddress('bech32m')
        original.unloadwallet()

        self.log.info('Restore keys on a node with none of the old blocks or balances')
        assert_equal(destination.getblockcount(), 0)
        assert_raises_rpc_error(-5, 'Block not found', destination.getblock, source_blocks[0])
        assert_raises_rpc_error(-4, 'Wallet files should not be reused across chains',
                               destination.restorewallet, 'after_reset', backup)
        assert not (destination.wallets_path / 'after_reset').exists()
        assert_equal(sha256sum_file(backup), original_backup_hash)
        self.log.info('Require an explicit one-time cross-chain override and rescan from genesis')
        self.restart_node(1, extra_args=['-walletcrosschain=1'])
        result = destination.restorewallet('after_reset', backup, load_on_startup=True)
        assert_equal(result['name'], 'after_reset')
        restored = destination.get_wallet_rpc('after_reset')
        restored.syncwithvalidationinterfacequeue()
        assert_equal(restored.getwalletinfo()['unlocked_until'], 0)
        assert_equal(restored.getwalletinfo()['private_keys_enabled'], True)
        assert_raises_rpc_error(-14, 'wallet passphrase entered was incorrect',
                               restored.walletpassphrase, passphrase + ' wrong', 1)
        with WalletUnlock(restored, passphrase):
            rescan = restored.rescanblockchain(0)
        assert_equal(rescan['start_height'], 0)
        assert_equal(rescan['stop_height'], 0)
        assert_equal([(item['desc'], item['active'], item['internal'])
                      for item in restored.listdescriptors()['descriptors']], original_descriptors)
        restored_address_info = restored.getaddressinfo(receiving_address)
        assert_equal(restored_address_info['ismine'], True)
        assert_equal(restored_address_info['desc'], address_info['desc'])
        assert_equal(restored_address_info['scriptPubKey'], address_info['scriptPubKey'])
        assert_equal(restored_address_info['labels'], ['preserved label'])
        assert_equal(restored.getnewaddress('', 'bech32m'), expected_next_address)
        assert_equal(restored.getrawchangeaddress('bech32m'), expected_next_change)
        self.assert_no_old_balance(restored)
        assert_equal(destination.getrawmempool(), [])
        assert_equal(sha256sum_file(backup), original_backup_hash)

        self.log.info('Reject another network and the transient P2P-v2 identifier')
        for name, wrong_magic in (('wrong_chain', bytes.fromhex('bb51f5e7')),
                                  ('p2p_id', MAGIC_BYTES['regtest'])):
            wrong_backup = self.backup_dir / f'{name}.sqlite'
            shutil.copyfile(backup, wrong_backup)
            self.set_backup_application_id(wrong_backup, wrong_magic)
            wrong_backup_hash = sha256sum_file(wrong_backup)
            assert_raises_rpc_error(-18, 'Data is not in recognized format',
                                   destination.restorewallet, name, wrong_backup)
            assert not (destination.wallets_path / name).exists()
            assert_equal(sha256sum_file(wrong_backup), wrong_backup_hash)

        self.log.info('Persist the new locator and reopen without the cross-chain override')
        restored.unloadwallet()
        restored_file = (destination.wallets_path / 'after_reset' / 'wallet.dat').resolve(strict=True)
        assert restored_file.is_relative_to(Path(self.options.tmpdir).resolve(strict=True))
        reconciled_backup = self.backup_dir / 'reconciled.sqlite'
        shutil.copyfile(restored_file, reconciled_backup)
        assert_equal(self.backup_locator(reconciled_backup), int(current_genesis, 16))
        self.restart_node(1)
        restored = destination.get_wallet_rpc('after_reset')
        assert_equal(restored.getwalletinfo()['unlocked_until'], 0)
        self.assert_no_old_balance(restored)

        self.log.info('Accept new-chain funds and prove the restored encrypted keys can spend them')
        new_miner = MiniWallet(destination, tag_name='reset destination miner')
        self.generate(new_miner, 101, sync_fun=self.no_op)
        assert_not_equal(destination.getblockhash(1), source.getblockhash(1))
        self.assert_no_old_balance(restored)
        new_funding = new_miner.send_to(
            from_node=destination,
            scriptPubKey=bytes.fromhex(address_info['scriptPubKey']),
            amount=3 * COIN,
        )
        self.generate(new_miner, 1, sync_fun=self.no_op)
        restored.syncwithvalidationinterfacequeue()
        assert_equal(restored.getbalance(), 3)
        available = restored.listunspent()
        assert_equal([(coin['txid'], coin['vout']) for coin in available],
                     [(new_funding['txid'], new_funding['sent_vout'])])
        assert not old_outpoints.intersection((coin['txid'], coin['vout']) for coin in available)
        raw_tx = destination.createrawtransaction(
            [{'txid': new_funding['txid'], 'vout': new_funding['sent_vout']}],
            {new_miner.get_address(): Decimal('2.9999000000')},
        )
        assert_raises_rpc_error(-13, 'Please enter the wallet passphrase',
                               restored.signrawtransactionwithwallet, raw_tx)
        with WalletUnlock(restored, passphrase):
            signed = restored.signrawtransactionwithwallet(raw_tx)
        assert_equal(signed['complete'], True)
        assert_equal(destination.testmempoolaccept([signed['hex']])[0]['allowed'], True)
        sent_txid = destination.sendrawtransaction(signed['hex'])
        self.generate(new_miner, 1, sync_fun=self.no_op)
        assert_equal(restored.gettransaction(sent_txid)['confirmations'], 1)
        self.assert_no_old_balance(restored)

        self.restart_node(1)
        restored = destination.get_wallet_rpc('after_reset')
        assert_equal(restored.getwalletinfo()['unlocked_until'], 0)
        assert_equal(restored.getaddressinfo(receiving_address)['ismine'], True)
        assert_equal(restored.gettransaction(sent_txid)['confirmations'], 1)
        self.assert_no_old_balance(restored)


if __name__ == '__main__':
    WalletTestnetResetRestoreTest(__file__).main()
