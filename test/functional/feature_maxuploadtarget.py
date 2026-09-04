#!/usr/bin/env python3
# Copyright (c) 2015-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test behavior of -maxuploadtarget.

* Verify that getdata requests for old blocks (>1week) are dropped
if uploadtarget has been reached.
* Verify that getdata requests for recent blocks are respected even
if uploadtarget has been reached.
* Verify that mempool requests lead to a disconnect if uploadtarget has been reached.
* Verify that the upload counters are reset after 24 hours.
"""
from collections import defaultdict
import time

from test_framework.messages import (
    CInv,
    MAX_BLOCK_SERIALIZED_SIZE,
    MSG_BLOCK,
    msg_getdata,
    msg_mempool,
)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    mine_large_block,
)
from test_framework.wallet import MiniWallet


# This functional test runs on regtest, whose target spacing remains ten
# minutes even though the public ConnectCoin networks target ten seconds.
TARGET_SPACING_SECONDS = 10 * 60
MIB = 1024 * 1024
DAILY_RELAY_BUFFER = (24 * 60 * 60 // TARGET_SPACING_SECONDS) * MAX_BLOCK_SERIALIZED_SIZE
# A modest historical-block headroom is enough to exercise both serving and
# disconnect behavior without transferring hundreds of MiB through the Python
# P2P implementation on every CI run.
HISTORICAL_BLOCK_HEADROOM_MB = 32
UPLOAD_TARGET_MB = (DAILY_RELAY_BUFFER + MIB - 1) // MIB + HISTORICAL_BLOCK_HEADROOM_MB


class TestP2PConn(P2PInterface):
    def __init__(self):
        super().__init__()
        self.block_receive_map = defaultdict(int)

    def on_inv(self, message):
        pass

    def on_block(self, message):
        self.block_receive_map[message.block.hash_int] += 1

class MaxUploadTest(BitcoinTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            f"-maxuploadtarget={UPLOAD_TARGET_MB}M",
        ]]

    def assert_uploadtarget_state(self, *, target_reached, serve_historical_blocks):
        """Verify the node's current upload target state via the `getnettotals` RPC call."""
        uploadtarget = self.nodes[0].getnettotals()["uploadtarget"]
        assert_equal(uploadtarget["target_reached"], target_reached)
        assert_equal(uploadtarget["serve_historical_blocks"], serve_historical_blocks)

    def run_test(self):
        # Initially, neither historical blocks serving limit nor total limit are reached
        self.assert_uploadtarget_state(target_reached=False, serve_historical_blocks=True)

        # Before we connect anything, we first set the time on the node
        # to be in the past, otherwise things break because the CNode
        # time counters can't be reset backward after initialization
        old_time = int(time.time() - 2*60*60*24*7)
        self.nodes[0].setmocktime(old_time)

        # Generate some old blocks
        self.wallet = MiniWallet(self.nodes[0])
        self.generate(self.wallet, 130)

        # p2p_conns[0] will only request old blocks
        # p2p_conns[1] will only request new blocks
        # p2p_conns[2] will test resetting the counters
        p2p_conns = []

        for _ in range(3):
            # Don't use v2transport in this test (too slow with the unoptimized python ChaCha20 implementation)
            p2p_conns.append(self.nodes[0].add_p2p_connection(TestP2PConn(), supports_v2_p2p=False))

        # Now mine a big block
        mine_large_block(self, self.wallet, self.nodes[0])

        # Store the hash; we'll request this later
        big_old_block = self.nodes[0].getbestblockhash()
        old_block_size = self.nodes[0].getblock(big_old_block, True)['size']
        # A nearly empty block would make success_count enormous and turn a
        # useful regression test into a very long sequence of P2P requests.
        assert_greater_than(old_block_size, 900_000)
        big_old_block = int(big_old_block, 16)

        # Advance to two days ago
        self.nodes[0].setmocktime(int(time.time()) - 2*60*60*24)

        # Mine one more block, so that the prior block looks old
        mine_large_block(self, self.wallet, self.nodes[0])

        # We'll be requesting this new block too
        big_new_block = self.nodes[0].getbestblockhash()
        big_new_block = int(big_new_block, 16)

        # p2p_conns[0] will test what happens if we just keep requesting the
        # the same big old block too many times (expect: disconnect)

        getdata_request = msg_getdata()
        getdata_request.inv.append(CInv(MSG_BLOCK, big_old_block))

        max_bytes_per_day = UPLOAD_TARGET_MB * MIB
        daily_buffer = DAILY_RELAY_BUFFER
        max_bytes_available = max_bytes_per_day - daily_buffer
        success_count = max_bytes_available // old_block_size

        # The remaining headroom is used for historical blocks.
        for i in range(success_count):
            p2p_conns[0].send_and_ping(getdata_request)
            assert_equal(p2p_conns[0].block_receive_map[big_old_block], i+1)

        assert_equal(len(self.nodes[0].getpeerinfo()), 3)
        # Serialize the final requests so the send accounting is visible
        # before the next request is evaluated. Queuing several requests at
        # once can let all of them pass the pre-send limit check.
        with self.nodes[0].assert_debug_log(expected_msgs=["historical block serving limit reached, disconnecting peer=0"]):
            for _ in range(3):
                blocks_received = p2p_conns[0].block_receive_map[big_old_block]
                p2p_conns[0].send_without_ping(getdata_request)
                p2p_conns[0].wait_until(
                    lambda: not p2p_conns[0].is_connected or p2p_conns[0].block_receive_map[big_old_block] > blocks_received,
                    check_connected=False,
                )
                if not p2p_conns[0].is_connected:
                    break
            p2p_conns[0].wait_for_disconnect()
        assert_equal(len(self.nodes[0].getpeerinfo()), 2)
        self.log.info("Peer 0 disconnected after downloading old block too many times")

        # Historical blocks serving limit is reached by now, but total limit still isn't
        self.assert_uploadtarget_state(target_reached=False, serve_historical_blocks=False)

        # Recent blocks remain available after the historical-block reserve is
        # reached. The separate small-target phase below exercises behavior
        # after the total limit is reached without transferring hundreds of GB.
        getdata_request.inv = [CInv(MSG_BLOCK, big_new_block)]
        for i in range(3):
            p2p_conns[1].send_and_ping(getdata_request)
            assert_equal(p2p_conns[1].block_receive_map[big_new_block], i+1)

        self.assert_uploadtarget_state(target_reached=False, serve_historical_blocks=False)

        self.log.info("Peer 1 able to repeatedly download new block")

        # But if p2p_conns[1] tries for an old block, it gets disconnected too.
        getdata_request.inv = [CInv(MSG_BLOCK, big_old_block)]
        with self.nodes[0].assert_debug_log(expected_msgs=["historical block serving limit reached, disconnecting peer=1"]):
            p2p_conns[1].send_without_ping(getdata_request)
            p2p_conns[1].wait_for_disconnect()
        assert_equal(len(self.nodes[0].getpeerinfo()), 1)

        self.log.info("Peer 1 disconnected after trying to download old block")

        self.log.info("Advancing system time on node to clear counters...")

        # If we advance the time by 24 hours, then the counters should reset,
        # and p2p_conns[2] should be able to retrieve the old block.
        self.nodes[0].setmocktime(int(time.time()))
        p2p_conns[2].sync_with_ping()
        p2p_conns[2].send_and_ping(getdata_request)
        assert_equal(p2p_conns[2].block_receive_map[big_old_block], 1)
        self.assert_uploadtarget_state(target_reached=False, serve_historical_blocks=True)

        self.log.info("Peer 2 able to download old block")

        self.nodes[0].disconnect_p2ps()

        self.log.info("Restarting node 0 with download permission, bloom filter support and 1MB maxuploadtarget")
        self.restart_node(0, ["-whitelist=download@127.0.0.1", "-peerbloomfilters", "-maxuploadtarget=1"])
        # Total limit isn't reached after restart, but 1 MB is too small to serve historical blocks
        self.assert_uploadtarget_state(target_reached=False, serve_historical_blocks=False)

        # Reconnect to self.nodes[0]
        peer = self.nodes[0].add_p2p_connection(TestP2PConn(), supports_v2_p2p=False)

        # Sending mempool message shouldn't disconnect peer, as total limit isn't reached yet
        peer.send_and_ping(msg_mempool())

        #retrieve 20 blocks which should be enough to break the 1MB limit
        getdata_request.inv = [CInv(MSG_BLOCK, big_new_block)]
        for i in range(20):
            peer.send_and_ping(getdata_request)
            assert_equal(peer.block_receive_map[big_new_block], i+1)

        # Total limit is exceeded
        self.assert_uploadtarget_state(target_reached=True, serve_historical_blocks=False)

        getdata_request.inv = [CInv(MSG_BLOCK, big_old_block)]
        peer.send_and_ping(getdata_request)

        self.log.info("Peer still connected after trying to download old block (download permission)")
        peer_info = self.nodes[0].getpeerinfo()
        assert_equal(len(peer_info), 1)  # node is still connected
        assert_equal(peer_info[0]['permissions'], ['download'])

        self.log.info("Peer gets disconnected for a mempool request after limit is reached")
        with self.nodes[0].assert_debug_log(expected_msgs=["mempool request with bandwidth limit reached, disconnecting peer=0"]):
            peer.send_without_ping(msg_mempool())
            peer.wait_for_disconnect()

        self.log.info("Test passing an unparsable value to -maxuploadtarget throws an error")
        self.stop_node(0)
        self.nodes[0].assert_start_raises_init_error(extra_args=["-maxuploadtarget=abc"], expected_msg="Error: Unable to parse -maxuploadtarget: 'abc'")

if __name__ == '__main__':
    MaxUploadTest(__file__).main()
