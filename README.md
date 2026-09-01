ConnectCoin Core development tree
=================================

ConnectCoin Core is a Bitcoin Core fork under active development. Its goal is a
UTXO cryptocurrency with native pay-to-connect (P2C) outputs backed by
independently verifiable TLS 1.3 connection proofs.

This repository is not production-ready. The current consensus milestone uses
typed transaction outputs instead of serialized output scripts. Type `1` is a
single 32-byte x-only public key authorized by one 64-byte BIP340 Schnorr
signature. Type `2` is PAY_TO_CONNECT for a canonical DNS domain and is spent
with a bounded, independently verified TLS 1.3 connection proof. There is no
certificate-specific P2C output form.

The codebase retains Bitcoin Core copyright notices and upstream attribution.

The current public identifier inventory and pre-launch registry warnings are in
[doc/connectcoin-branding.md](doc/connectcoin-branding.md).
The experimental consensus format and its compatibility boundaries are in
[doc/typed-outputs.md](doc/typed-outputs.md).
The P2C payload and proof profile are specified in
[doc/pay-to-connect.md](doc/pay-to-connect.md).

License
-------

ConnectCoin Core is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/license/MIT.

Development Process
-------------------

The development branch should be built and tested after every consensus or
networking change. See `doc/build-*.md` for the inherited build instructions.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md)
and useful hints for developers can be found in [doc/developer-notes.md](doc/developer-notes.md).

Testing
-------

Testing and code review is the bottleneck for development; we get more pull
requests than we can review and test on short notice. Please be patient and help out by testing
other people's pull requests, and remember this is a security-critical project where any mistake might cost people
lots of money.

### Automated Testing

Developers are strongly encouraged to write [unit tests](src/test/README.md) for new code, and to
submit new unit tests for old code. Unit tests can be compiled and run
(assuming they weren't disabled during the generation of the build system) with: `ctest`. Further details on running
and extending unit tests can be found in [/src/test/README.md](/src/test/README.md).

There are also [regression and integration tests](/test), written
in Python.
These tests can be run (if the [test dependencies](/test) are installed) with: `build/test/functional/test_runner.py`
(assuming `build` is your build directory).

The CI (Continuous Integration) systems make sure that every pull request is tested on Windows, Linux, and macOS.
The CI must pass on all commits before merge to avoid unrelated CI failures on new pull requests.

### Manual Quality Assurance (QA) Testing

Changes should be tested by somebody other than the developer who wrote the
code. This is especially important for large or high-risk changes. It is useful
to add a test plan to the pull request description if testing the changes is
not straightforward.

Translations
------------

The translation catalogs are inherited from Bitcoin Core and have not yet been
reviewed as ConnectCoin translations. New or changed ConnectCoin strings fall
back to English until this project establishes its own translation workflow.
See the [branding inventory](doc/connectcoin-branding.md) for this limitation.
