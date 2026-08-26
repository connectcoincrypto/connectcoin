#!/usr/bin/env python3
# Copyright (c) 2018-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the deriveaddresses rpc call."""
from test_framework.test_framework import BitcoinTestFramework
from test_framework.descriptors import descsum_create
from test_framework.util import assert_equal, assert_raises_rpc_error

class DeriveaddressesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        assert_raises_rpc_error(-5, "Missing checksum", self.nodes[0].deriveaddresses, "a")

        descriptor = "wpkh(tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR/1/1/0)#eepju0f9"
        address = "ccrt1qjqmxmkpmxt80xz4y3746zgt0q3u3ferryh3yj3"
        assert_equal(self.nodes[0].deriveaddresses(descriptor), [address])

        descriptor = descriptor[:-9]
        assert_raises_rpc_error(-5, "Missing checksum", self.nodes[0].deriveaddresses, descriptor)

        descriptor_pubkey = "wpkh(tcubNC1uBHhBxmWgfPoCJ8krUwtLbnqX2uhWwmgfSt2GLbCHhPuEq4LhtLBZKLwC6YkHK2hXrPWvVMgReYFtgWxenNZAuC69MaERFZ7AygMKgQd/1/1/0)#ayyynzvq"
        address = "ccrt1qjqmxmkpmxt80xz4y3746zgt0q3u3ferryh3yj3"
        assert_equal(self.nodes[0].deriveaddresses(descriptor_pubkey), [address])

        ranged_descriptor = "wpkh(tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR/1/1/*)#y2yppv05"
        assert_equal(self.nodes[0].deriveaddresses(ranged_descriptor, [1, 2]), ["ccrt1qhku5rq7jz8ulufe2y6fkcpnlvpsta7rqqhekjp", "ccrt1qpgptk2gvshyl0s9lqshsmx932l9ccsv20k8sz9"])
        assert_equal(self.nodes[0].deriveaddresses(ranged_descriptor, 2), [address, "ccrt1qhku5rq7jz8ulufe2y6fkcpnlvpsta7rqqhekjp", "ccrt1qpgptk2gvshyl0s9lqshsmx932l9ccsv20k8sz9"])

        ranged_descriptor = descsum_create("wpkh(tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR/1/<0;1>/*)")
        assert_equal(self.nodes[0].deriveaddresses(ranged_descriptor, [1, 2]), [["ccrt1q7c8mdmdktrzs8xgpjmqw90tjn65j5a3y8de8wk", "ccrt1qs6n37uzu0v0qfzf0r0csm0dwa7prc0v5flq5m2"], ["ccrt1qhku5rq7jz8ulufe2y6fkcpnlvpsta7rqqhekjp", "ccrt1qpgptk2gvshyl0s9lqshsmx932l9ccsv20k8sz9"]])

        assert_raises_rpc_error(-8, "Range should not be specified for an un-ranged descriptor", self.nodes[0].deriveaddresses, descsum_create("wpkh(tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR/1/1/0)"), [0, 2])

        assert_raises_rpc_error(-8, "Range must be specified for a ranged descriptor", self.nodes[0].deriveaddresses, descsum_create("wpkh(tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR/1/1/*)"))
        assert_raises_rpc_error(-8, "Range must be specified for a ranged descriptor", self.nodes[0].deriveaddresses, descsum_create("wpkh(tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR/1/1/*)"), None)

        assert_raises_rpc_error(-8, "End of range is too high", self.nodes[0].deriveaddresses, descsum_create("wpkh(tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR/1/1/*)"), 10000000000)

        assert_raises_rpc_error(-8, "Range is too large", self.nodes[0].deriveaddresses, descsum_create("wpkh(tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR/1/1/*)"), [1000000000, 2000000000])

        assert_raises_rpc_error(-8, "Range specified as [begin,end] must not have begin after end", self.nodes[0].deriveaddresses, descsum_create("wpkh(tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR/1/1/*)"), [2, 0])

        assert_raises_rpc_error(-8, "Range should be greater or equal than 0", self.nodes[0].deriveaddresses, descsum_create("wpkh(tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR/1/1/*)"), [-1, 0])

        combo_descriptor = descsum_create("combo(tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR/1/1/0)")
        assert_equal(self.nodes[0].deriveaddresses(combo_descriptor), ["TP7jWUshV2kbNakShdh99WSL2bxJ9NTfRB", address, "tTbczn6wFPL4F4z28NQHnNjzcwAiNXyvDG"])

        # P2PK does not have a valid address
        assert_raises_rpc_error(-5, "Descriptor does not have a corresponding address", self.nodes[0].deriveaddresses, descsum_create("pk(tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR)"))

        # Before #26275, connectcoind would crash when deriveaddresses was
        # called with derivation index 2147483647, which is the maximum
        # positive value of a signed int32, and - currently - the
        # maximum value that the deriveaddresses ConnectCoin RPC call
        # accepts as derivation index.
        assert_equal(self.nodes[0].deriveaddresses(descsum_create("wpkh(tcprHuVh8FbViZHtZ3RyETUTGWN7fPRT2RcFpuzPi5v3SGhiG4u85PdTXaP8e2aoVRy41reekrWrRjKGaXdAFA4qZAYaaoEKEDumiDsfLTYRmVR/1/1/*)"), [2147483647, 2147483647]), ["ccrt1qtzs23vgzpreks5gtygwxf8tv5rldxvvs3td3fd"])

        hardened_without_privkey_descriptor = descsum_create("wpkh(tcubNC1uBHhBxmWgfPoCJ8krUwtLbnqX2uhWwmgfSt2GLbCHhPuEq4LhtLBZKLwC6YkHK2hXrPWvVMgReYFtgWxenNZAuC69MaERFZ7AygMKgQd/1'/1/0)")
        assert_raises_rpc_error(-5, "Cannot derive script without private keys", self.nodes[0].deriveaddresses, hardened_without_privkey_descriptor)

        bare_multisig_descriptor = descsum_create("multi(1,tcubNC1uBHhBxmWgfPoCJ8krUwtLbnqX2uhWwmgfSt2GLbCHhPuEq4LhtLBZKLwC6YkHK2hXrPWvVMgReYFtgWxenNZAuC69MaERFZ7AygMKgQd/1/1/0,tcubNC1uBHhBxmWgfPoCJ8krUwtLbnqX2uhWwmgfSt2GLbCHhPuEq4LhtLBZKLwC6YkHK2hXrPWvVMgReYFtgWxenNZAuC69MaERFZ7AygMKgQd/1/1/1)")
        assert_raises_rpc_error(-5, "Descriptor does not have a corresponding address", self.nodes[0].deriveaddresses, bare_multisig_descriptor)

if __name__ == '__main__':
    DeriveaddressesTest(__file__).main()
