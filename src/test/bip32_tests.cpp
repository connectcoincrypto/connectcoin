// Copyright (c) 2013-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <clientversion.h>
#include <key.h>
#include <key_io.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <util/bip32.h>
#include <util/strencodings.h>

#include <string>
#include <vector>

namespace {

struct TestDerivation {
    std::string pub;
    std::string prv;
    unsigned int nChild;
};

struct TestVector {
    std::string strHexMaster;
    std::vector<TestDerivation> vDerive;

    explicit TestVector(std::string strHexMasterIn) : strHexMaster(strHexMasterIn) {}

    TestVector& operator()(std::string pub, std::string prv, unsigned int nChild) {
        vDerive.emplace_back();
        TestDerivation &der = vDerive.back();
        der.pub = pub;
        der.prv = prv;
        der.nChild = nChild;
        return *this;
    }
};

TestVector test1 =
  TestVector("000102030405060708090a0b0c0d0e0f")
    ("ccpubKDazFod7ockyt4NZjUvABrxZxRXMz3tsynhhQ6ZKnFvqMPUM8A96Sne1KAxUEwBm5FtuZjQFpQNcXrV1tRWyaodu6zeq6U1DBcmeGiRaKd6",
     "ccprvNzbdrJ6DyFCgfaJ6dTP9pj1qQPgsabB2cZn6bi9iDvPrUb9CacpqtzKXTuKAj3mh9izy4nP7B72JjLHsbBgKxmdpKHvbvwVcRWAuENGPUNf",
     BIP32_HARDENED_FLAG)
    ("ccpubKFrQFWvatssy2fWGwEno4u4gHF5iN8NtfQ8bNBwSkT87ifmXYPgBZvzR6SXAzF6J5UBURxwgZ3ShWFT9qTFMeh6dJdT61muRhYuCaT6D5vW",
     "ccprvP2s3r1Ph4WKfpBRoqDFnhm7wjDFDxff3JBCzZoXqC7b8qsSNzrMw28fwFAfW8m9ermtfS5hyZTsBT6KbEmxWkqiLPAbkAuCW8WokfbMdsPT",
     1)
    ("ccpubKJ2XTJUUHam2s7YifVgffdxLLd4pNA7BbioRxTiFh5vKxDGfGC6VXVteWWykQBoe3BoXDPPnfdwB6ZByJnRFLLvZbqHwVQuhLHRysHoinzj",
     "ccprvP53B3nwaTDCjedUFZU9fJW1bnbEKxhPLEVsqA5Je8kPM5QwWienEyhaAfDrZjgvhJreMEuJQeApbb2qpN7kVY1CmtwZUEeyMoK88CTTDSpM",
     BIP32_HARDENED_FLAG | 2)
    ("ccpubKLdoVqJKzTcSjHLov6632dzUZmwLdhhAwG37Co1QrxHmN4Tn4wvvRwSa4YAar4ExPhuFpmNQ3aW9YtLG1sxnPo5QBjG5aMyP3yM7xXT9kbU",
     "ccprvP7eT6KmSA649WoGLp4Z2fW3k1k6rEEyKa37WQQboJcknVG8dXQcft986DG5BhvSBpKfjqNFL4aeVpEaZx8fgf6eSFZ82PbDJQasLq5VBbgu",
     2)
    ("ccpubKNsCLGRHAwHRokpXcMJ7v99nebE5GJ1NNjvQTkXtBPrNjA698idHdWuVjs9yFrPgs22RxmrM6KQG7JgG7BPz7PYf6WpGQYsD4fFy79k6Z8i",
     "ccprvP9sqvktPLZj8bGk4WKm7Z1D46ZParqHX1WzofN8Gd4KPrMkzbBK35ib1taUeAaqct7oSTHUtyQ6fHRV29ZD2rVEUeoD7wg31ZZLWLbLadwB",
     1000000000)
    ("ccpubKQaxox2XJ4fdL6byitrrDr9nSpoP5npQAzJxi2v2PUEsSHVXf8swtd4ZNh4v77UQLiJyU7n9dX6rhj5HD2tpxehmy4EMD2SqscstMPzk7r3",
     "ccprvPBbcQSVdTh7L7cXWcsKqriD3tnxtgL6YomPMueWQq8htZVAP7bZhLpk5XSFr5VxhVXChEeTqpSdnHbVPgdDkySt1zHyM4aniqtts5jHnxiy",
     0);

TestVector test2 =
  TestVector("fffcf9f6f3f0edeae7e4e1dedbd8d5d2cfccc9c6c3c0bdbab7b4b1aeaba8a5a29f9c999693908d8a8784817e7b7875726f6c696663605d5a5754514e4b484542")
    ("ccpubKDazFod7ockysfstqdzSe4gLCcfnfg9hfNVRh3J9ke3heNVYJJv2sgobfH3SxBYiyp8hUQ4RDUssFeDF1cS8iidqxvrHvxVsVzSkYiqTq2C",
     "ccprvNzbdrJ6DyFCgfBoRjcTSGvjbeaqJGDRrJ9ZptetYCJWimaAPkmbnKtV7oy6DuZKnPmkPp4qZTD4Vps1ydc52a8xamhzr5HQLzko8bQDpJfC",
     0)
    ("ccpubKGrjXXK5Hdb4PwUByRNKhs5rZaR3NedzDYEnrrKbq3ufLLwWsaG72XNrEk1LkGZAC8csLd4AcZiqQodvtm8Z4KJ8EyaxqJeFdUZQyvQqvB7",
     "ccprvP3sP81nBTG2mBTPisPqKLj981YaYyBv8rKKC4TuzGiNgTYcNL2wrUj4NPUN2YdAcoRzDi5FHtNUdBNeoBta5WN7ymtQuTma68AhgxfkfqEQ",
     0xFFFFFFFF)
    ("ccpubKJ1nn8LbfXmAZogNPsMyBCmQxKdwv8ogJhKKnFENUng3neAyVTgREbASr8FTW1AFyeYQzTumun9S5UWGffcfnTkkn4D2C2uGVrq1pg2YCvu",
     "ccprvP52SNcohqACsMKbuHqpxp4pgQHoTWg5pwUPiyrpkvT94uqqpwvNAgnqxzpr1Gsu6hfK7V6SvwNyjccPZvGBD5fZYCceFEHJLux3Dj47AFT2",
     1)
    ("ccpubKLpmC9KXqB4MfRMWTXWY99heHrWT2Kiwd8pBdJthng2VTU38nyD88VYpgvSbHsrYZpzxq711YuTGAGmHaa3d3oDfNLQsQaLURdVy55AKKaY",
     "ccprvP7qQndndzoW4SwH3MVyXn1kujpfxcs16FutapvV6ELVWafhzFRtsahELqd3dJb6o1cfpEPXKLvewwKAiy9hb69NqLPB45PGjPkTxCnAKa5z",
     0xFFFFFFFE)
    ("ccpubKMzo77FtSa24xNb7C1L5WfPqCayp5JMSeqY5A2b7dSHjupN4te98f1uvKaa6VFNzXGebdc4ibWn7ouvgGTA8m9v7eEjMxsRtHraYc5dxvAr",
     "ccprvP91ShbizcCTmjtWe5yo59XT6eZ9KfqdbHccUMeBW56km322vM6pt7DbSUKn1w1V5ZBxWYDJJjJEXkSG6V6c5mz41AKiNDbJMjLUUGZax3ys",
     2)
    ("ccpubKPMq4YUPxkLJhdmzv5TmVQmobeWvKdGXK19xZowcGE9Kz1he4H4tqarvPyeNcpjh3KrpuXa8VDo7JLzB6Yr35QmhHQZSuEicy9mjGfrpSUa",
     "ccprvPANUf2wW8Nn1V9hXp3vm8Gq53cgRvAYfwnEMmRXzhtcM7DNVWjkeHnYSYjT262a9bVpEpxw8BQ5Dnz9cDQyJpxniyBDsH1S5Rhp2M64vrWW",
     0);

TestVector test3 =
  TestVector("4b381541583be4423346c643850da4b320e46a87ae3d2a4e6da11eba819cd4acba45d239319ac14f863b8d5ab5a0d0c64d2e8a1e7d1457df2e5a3c51c73235be")
    ("ccpubKDazFod7ockyrjL4N2VEr3wN5FztRYJyNiFz6LKi2L8VccioxT9zpx389GaRmn8N9ttZJPwRc9higwr9rKUa94Hz6AkWfWwiHhxT5CYjbxH",
     "ccprvNzbdrJ6DyFCgeFFbFzxEUuzdXEAQ25b81VLPHwv6TzbWjpPfQuqkH9ieHxpPraZ7UUR9NzXfMdoDBWdHs3DTMcTqHzZVGikKHUTVFLfkXFK",
      BIP32_HARDENED_FLAG)
    ("ccpubKFxBzmTFUz8L4GRTbApjuJh2mccGaPisnCVvua1Yxe3rsdrGXwvruQpqSzQwW8j7MGPQ5MhAT6uDKQnVnZQkiJZvmGhQDNSWmtKMjZo7fRK",
     "ccprvP2xqbFvMeca2qnLzV9HjYAkJDamnAw12QyaL7BbwQJWszqX7zQccMcWMbjAkCXt2vd4ZPwKY1NRfq4GSSFoD7NfTj8UY1t28QVR9CfVovLC",
      0);

TestVector test4 =
  TestVector("3ddd5602285899a946114506157c7997e5444528f3003f6134712147db19b678")
    ("ccpubKDazFod7ockytnqdCkr3eoQFhVb4LniicqxPU51HtryBTWY9AMkm3AbDqPq6oDiRjS27T5AcYvUTVpdHGXVVDv9trb3bDpuXCJdg9zpVwyq",
     "ccprvNzbdrJ6DyFCggJmA6jK3HfTX9TkZwKzsFd2nfgbgLXSCaiCzcpSWVNGjz86WsJxJ3wNmVBQoHxzypXFaX8UNSS8DoERXKbdyZcKe1RHBnCP",
     BIP32_HARDENED_FLAG)
    ("ccpubKGk6eBjnBNsRXfrkq5mpfCW29RUL46FGN9a3p3BGYK39x3bFihBVBDGZzz6rpr5Z9XnUQ7ZE9rQi2kKLyJwtKECvtuUZ6dzuF7BixQADxD1",
     "ccprvP3kkEgCtM1K8KBnHj4EpJ4ZHbPdqedXQzveT1emeyyWB5FG7B9sEdQx69g1kv88f1vQD1kb2cR74mBSg8W91T519ecjCXX5yYsMSkHMthB7",
     BIP32_HARDENED_FLAG | 1)
    ("ccpubKJsnJB8nh6v2CpiPyMgim55sKMeQZ1x525csanhUCW8bj8umbBfCQLEStvsPg2iEjKPqHtVLcJkh1EeFRum32X9JEhKc4NUr7XYd1vdpAQx",
     "ccprvP5tRtfbtrjMizLdvsL9iPw98mKov9ZEDerhGnQHreAbcrLad3eLwrXuy3dyqyXAV6goKCMQzP4CwU9GPXS68jTUNtxSDdZLJQYu8qsYQrNK",
     0);

const std::vector<std::string> TEST5 = {
    "ccpubKDazFod7ockyriH2EWZxLCQxzLunkShGa4HMxCcHJCNFEHCssWoW3wVnrV13yoXWofizASN71Ki8faoKBPsY9yAfCfj8p8jeJqoii1RDhwX",
    "ccprvNzbdrJ6DyFCgeECZ8V2wy4UESK5JLyyRCqMm9pCfjrqGMUsjKyVFW9BK1RGeCtVYxWy2mNkcotAaa23ioxE37gth7TQhjczdCJTNwdEyDoV",
    "ccpubKDazFod7ockyriH2EWZxLCQxzLunkShGa4HMxCcHJCNFEHCssWoW3wVnrcn2gjKeDNRthigvdeC8UsCQqrLQknYAME1ZnjVEHTqGgSFFeW2",
    "ccprvNzbdrJ6DyFCgeECZ8V2wy4UESK5JLyyRCqMm9pCfjrqGMUsjKyVFW9BK1RdkbZB3odytoGZzxtSfsRecxBqrzvwzqDhBwQgBMbx2vMvNgPQ",
    "ccpubKDazFod7ockyriH2EWZxLCQxzLunkShGa4HMxCcHJCNFEHCssWoW3wVnrWwoA2yYf6QDJFwouuL8cuQ66WVFog1XzJoF4HRYJVp7CYwC5bZ",
    "ccprvNzbdrJ6DyFCgeECZ8V2wy4UESK5JLyyRCqMm9pCfjrqGMUsjKyVFW9BK1KoX4rpxFMxDPoptF9ag1TrJCqzi3pRNUJUsCxcVNdvsSZpTwZo",
    "ccprvNzc4g2aKNLeyiSJUnkJ19VtTzjwo6GhZh7QqrCExsLTEYp19THouxbJNRrNazUhy2XFWZfXBefXJ37iC5XuXUvUR8g9jzuorHkcCiNgyoxM",
    "ccpubKDbR5Y7DCiDGvvNwtmq1WdqCYmnHVjRR4LLSeaeaRfzDRcLHzq8AWPcrHAvjPuyBzqPYyW9YnjUfAS4BVRF7xMNn5UU4d3gw2w2LU7g9ZoJ",
    "ccprvNzbdrJ6E33DqVtBpGjaTEr9qsAgrsqPybY2GvFKVFCtAYdgqfCgW5qDYjUs5fu3PuqTaLPrJMCicCpX43QhjEj3iUWzH4izGQoAeQcmG8c1",
    "ccpubKDazFod7sQn8iNGHNm7Tbz6aRCXMHJ7pxkwsidj6oYR9RS1zCjzkddY2aoRE5LJct9bckEUfVGfyL8s3TJ3Ki9x5RKJbgrsM9yanAK7RPLH",
    "DMwo58pR1QLEFihHiXPVykYB6fJmsTeHvyTp7hRThAtCX8CvYzgPcn8XnmdfHGMQzT7ayAmfo4z3gY5KfbrZWZ6St24UVf2Qgo6oujFktLHdHY4",
    "DMwo58pR1QLEFihHiXPVykYB6fJmsTeHvyTp7hRThAtCX8CvYzgPcn8XnmdfHPmHJiEDXkTiJTVV9rHEBUem2mwVbbNfvT2MTcAqj3nesx8uBf9",
    "ccprvNzbdrJ6DyFCgeECZ8V2wy4UESK5JLyyRCqMm9pCfjrqGMUsjKyVFW9BK1HrmtdNvPwGzFzFBLZxg49FXHjNzQ7aVgfQkxovbNyvUvc2fSFZ",
    "ccprvNzbdrJ6DyFCgeECZ8V2wy4UESK5JLyyRCqMm9pCfjrqGMUsjKyVFW9BK1KoX4rpxFMxDPoptF9ag1TrDfydoWoAb1bDsx6uAgLuVeobQHR9",
    "ccpubKDazFod7ockyriH2EWZxLCQxzLunkShGa4HMxCcHJCNFEHCssWoW3wVnrYtYLGRaWX5SS5XWpUx8aDzs1d6yTNrQmwsMJS7SJ9pVggodUSm",
    "xprv9s21ZrQH143K3QTDL4LXw2F7HEK3wJUD2nW2nRk4stbPy6cq3jPPqjiChkVvvNKmPGJxWUtg6LnF5kejMRNNU3TGtRBeJgk33yuGBxrMPHL"
};

void RunTest(const TestVector& test)
{
    std::vector<std::byte> seed{ParseHex<std::byte>(test.strHexMaster)};
    CExtKey key;
    CExtPubKey pubkey;
    key.SetSeed(seed);
    pubkey = key.Neuter();
    for (const TestDerivation &derive : test.vDerive) {
        unsigned char data[74];
        key.Encode(data);
        pubkey.Encode(data);

        // Test private key
        BOOST_CHECK(EncodeExtKey(key) == derive.prv);
        BOOST_CHECK(DecodeExtKey(derive.prv) == key); //ensure a base58 decoded key also matches

        // Test public key
        BOOST_CHECK(EncodeExtPubKey(pubkey) == derive.pub);
        BOOST_CHECK(DecodeExtPubKey(derive.pub) == pubkey); //ensure a base58 decoded pubkey also matches

        // Derive new keys
        CExtKey keyNew;
        BOOST_CHECK(key.Derive(keyNew, derive.nChild));
        CExtPubKey pubkeyNew = keyNew.Neuter();
        if (!(derive.nChild & BIP32_HARDENED_FLAG)) {
            // Compare with public derivation
            CExtPubKey pubkeyNew2;
            BOOST_CHECK(pubkey.Derive(pubkeyNew2, derive.nChild));
            BOOST_CHECK(pubkeyNew == pubkeyNew2);
        }
        key = keyNew;
        pubkey = pubkeyNew;
    }
}

}  // namespace

BOOST_FIXTURE_TEST_SUITE(bip32_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(bip32_test1) {
    RunTest(test1);
}

BOOST_AUTO_TEST_CASE(bip32_test2) {
    RunTest(test2);
}

BOOST_AUTO_TEST_CASE(bip32_test3) {
    RunTest(test3);
}

BOOST_AUTO_TEST_CASE(bip32_test4) {
    RunTest(test4);
}

BOOST_AUTO_TEST_CASE(bip32_test5) {
    for (const auto& str : TEST5) {
        auto dec_extkey = DecodeExtKey(str);
        auto dec_extpubkey = DecodeExtPubKey(str);
        BOOST_CHECK_MESSAGE(!dec_extkey.key.IsValid(), "Decoding '" + str + "' as xprv should fail");
        BOOST_CHECK_MESSAGE(!dec_extpubkey.pubkey.IsValid(), "Decoding '" + str + "' as xpub should fail");
    }
}

BOOST_AUTO_TEST_CASE(bip32_derive_ext_key)
{
    const CExtKey master{DecodeExtKey(test1.vDerive[0].prv)};
    const std::vector<uint32_t> path{test1.vDerive[0].nChild, test1.vDerive[1].nChild};
    const auto derived{DeriveExtKey(master, path)};
    BOOST_REQUIRE(derived);
    BOOST_CHECK(EncodeExtKey(derived->first) == test1.vDerive[2].prv);

    KeyOriginInfo expected_origin;
    expected_origin.fingerprint = master.id_key_fingerprint();
    expected_origin.path = path;
    BOOST_CHECK(derived->second == expected_origin);

    const auto root{DeriveExtKey(master, {})};
    BOOST_REQUIRE(root);
    BOOST_CHECK(root->first == master);
    expected_origin.path.clear();
    BOOST_CHECK(root->second == expected_origin);

    CExtKey max_depth{master};
    for (auto i{0}; i++ < 255;) {
        CExtKey next_key;
        BOOST_REQUIRE(max_depth.Derive(next_key, 0));
        max_depth = next_key;
    }
    BOOST_CHECK(!DeriveExtKey(max_depth, {0}));
}

BOOST_AUTO_TEST_CASE(bip32_has_hardened_derivation)
{
    const std::vector<uint32_t> empty;
    const std::vector<uint32_t> unhardened{0, 1, 2};
    const std::vector<uint32_t> hardened{BIP32_HARDENED_FLAG};
    const std::vector<uint32_t> mixed{0, BIP32_HARDENED_FLAG | 1, 2};
    BOOST_CHECK(!HasHardenedDerivation(empty));
    BOOST_CHECK(!HasHardenedDerivation(unhardened));
    BOOST_CHECK(HasHardenedDerivation(hardened));
    BOOST_CHECK(HasHardenedDerivation(mixed));
}

BOOST_AUTO_TEST_CASE(bip32_max_depth) {
    CExtKey key_parent{DecodeExtKey(test1.vDerive[0].prv)}, key_child;
    CExtPubKey pubkey_parent{DecodeExtPubKey(test1.vDerive[0].pub)}, pubkey_child;

    // We can derive up to the 255th depth..
    for (auto i = 0; i++ < 255;) {
        BOOST_CHECK(key_parent.Derive(key_child, 0));
        std::swap(key_parent, key_child);
        BOOST_CHECK(pubkey_parent.Derive(pubkey_child, 0));
        std::swap(pubkey_parent, pubkey_child);
    }

    // But trying to derive a non-existent 256th depth will fail!
    BOOST_CHECK(key_parent.nDepth == 255);
    BOOST_CHECK(pubkey_parent.nDepth == 255);
    BOOST_CHECK(!key_parent.Derive(key_child, 0));
    BOOST_CHECK(!pubkey_parent.Derive(pubkey_child, 0));
}

BOOST_AUTO_TEST_CASE(parse_hd_keypath)
{
    std::vector<uint32_t> keypath;

    BOOST_CHECK(ParseHDKeypath("1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1", keypath));
    BOOST_CHECK(!ParseHDKeypath("///////////////////////////", keypath));

    BOOST_CHECK(ParseHDKeypath("1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1'/1", keypath));
    BOOST_CHECK(!ParseHDKeypath("//////////////////////////'/", keypath));

    BOOST_CHECK(ParseHDKeypath("1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/", keypath));
    BOOST_CHECK(!ParseHDKeypath("1///////////////////////////", keypath));

    BOOST_CHECK(ParseHDKeypath("1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1'/", keypath));
    BOOST_CHECK(!ParseHDKeypath("1/'//////////////////////////", keypath));

    BOOST_CHECK(ParseHDKeypath("", keypath));
    BOOST_CHECK(!ParseHDKeypath(" ", keypath));

    BOOST_CHECK(ParseHDKeypath("0", keypath));
    BOOST_CHECK(!ParseHDKeypath("O", keypath));

    BOOST_CHECK(ParseHDKeypath("0000'/0000'/0000'", keypath));
    BOOST_CHECK(!ParseHDKeypath("0000,/0000,/0000,", keypath));

    BOOST_CHECK(ParseHDKeypath("01234", keypath));
    BOOST_CHECK(!ParseHDKeypath("0x1234", keypath));

    BOOST_CHECK(ParseHDKeypath("1", keypath));
    BOOST_CHECK(!ParseHDKeypath(" 1", keypath));

    BOOST_CHECK(ParseHDKeypath("42", keypath));
    BOOST_CHECK(!ParseHDKeypath("m42", keypath));

    // A path element's numeric part is capped at 2^31-1; the top bit is
    // reserved for the hardened marker (h or ').
    BOOST_CHECK(ParseHDKeypath("2147483647", keypath));  // 0x7fffffff, largest normal index
    BOOST_CHECK(!ParseHDKeypath("2147483648", keypath)); // 0x80000000, would set the hardened bit
    BOOST_CHECK(!ParseHDKeypath("4294967295", keypath)); // 0xffffffff
    BOOST_CHECK(!ParseHDKeypath("4294967296", keypath)); // uint32_t max + 1

    BOOST_CHECK(ParseHDKeypath("m", keypath));
    BOOST_CHECK(!ParseHDKeypath("n", keypath));

    BOOST_CHECK(ParseHDKeypath("m/", keypath));
    BOOST_CHECK(!ParseHDKeypath("n/", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0", keypath));
    BOOST_CHECK(!ParseHDKeypath("n/0", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0'", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0''", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0h", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0hh", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0x", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0a", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0G", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/h0", keypath));

    keypath.clear();
    BOOST_REQUIRE(ParseHDKeypath("m/0h/1h/2h", keypath));
    BOOST_REQUIRE_EQUAL(keypath.size(), 3);
    BOOST_CHECK_EQUAL(keypath[0], BIP32_HARDENED_FLAG);
    BOOST_CHECK_EQUAL(keypath[1], BIP32_HARDENED_FLAG | 1);
    BOOST_CHECK_EQUAL(keypath[2], BIP32_HARDENED_FLAG | 2);

    BOOST_CHECK(ParseHDKeypath("m/0'/0'", keypath));
    BOOST_CHECK(ParseHDKeypath("m/0h/0h", keypath));
    BOOST_CHECK(ParseHDKeypath("m/0'/0h", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/'0/0'", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/h0/0'", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/0", keypath));
    BOOST_CHECK(!ParseHDKeypath("n/0/0", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/0/00", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0/0/f00", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/0/000000000000000000000000000000000000000000000000000000000000000000000000000000000000", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/1/1/111111111111111111111111111111111111111111111111111111111111111111111111111111111111", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/00/0", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0'/00/'0", keypath));

    BOOST_CHECK(ParseHDKeypath("m/1/", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/1//", keypath));

    // The cap applies to every element, wherever it sits in the path.
    BOOST_CHECK(ParseHDKeypath("m/2147483647", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/2147483648", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/4294967295", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/4294967296", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/2147483647", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0/2147483648", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0/4294967295", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0/4294967296", keypath));
}

BOOST_AUTO_TEST_SUITE_END()
