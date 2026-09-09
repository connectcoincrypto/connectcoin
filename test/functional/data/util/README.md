# Utility transaction fixtures

`connectcoin-util-test.json` exercises both utilities without launching a node.
The transaction fixtures use ConnectCoin's typed serialization, 10 decimal
places per CC, and testnet4 addresses (the tool's default network).

- `typed-edit-input.hex` preserves the 21 input outpoints, sequences, and two
  output values from the old edit test. Inputs are unsigned; the two outputs
  now authorize the x-only public keys for the public test scalars 1 and 2.
- Signing fixtures use those publicly known scalars, testnet WIF encoding, all
  previous-output amounts, and 64-byte Schnorr `SIGHASH_DEFAULT` witnesses.
  Outputs are added **before** signing, since the signature commits to them.
- Expected transaction bytes and signatures were constructed with the Python
  test framework's typed serializer and BIP340/BIP341 helpers. JSON formatting
  comes from `connectcoin-tx -json`, after checking its hex, txid, size, weight,
  and amounts against the independent Python model.
- `tool_utils.py` verifies every successful signing case cryptographically,
  and separately checks type-1/P2C serialization, witness replacement, and
  signing with multiple inputs. A matching text snapshot alone is insufficient.
- `tx394b54bb.hex` remains an intentionally rejected historical Bitcoin
  transaction. Unsupported Script, data, multisig, and wrapping commands are
  explicit rejection tests, not skipped successes. Old unused Script snapshots
  are retained as historical test data; they are not valid ConnectCoin outputs.

All keys in these fixtures are public test keys, never wallet secrets. P2C
witness attachment tests check the raw tool's encoding only; full TLS proof
authentication and work-target validation belong to the consensus tests.
