# ConnectCoin Core

ConnectCoin Core is an experimental Bitcoin Core fork. It is not
production-ready; use regtest for development and do not treat development
coins or wallet formats as durable.

## Running

After building, the principal binaries are:

- `bin/connectcoin-qt` — graphical node and wallet;
- `bin/connectcoind` — headless node;
- `bin/connectcoin-cli` — RPC command-line client;
- `bin/connectcoin` — wrapper with `gui`, `node`, `rpc`, `wallet`, and `tx`
  subcommands.

On Windows these binaries have an `.exe` suffix. On macOS, run
`ConnectCoin-Qt.app`.

ConnectCoin uses its own data directory and configuration file. See
[Files](files.md), [`connectcoin.conf`](connectcoin-conf.md), and
[init scripts](init.md).

## Building and development

The build system and most engineering documentation are inherited from Bitcoin
Core, so some internal target names and upstream links intentionally retain
their original names.

- [Dependencies](dependencies.md)
- [macOS build notes](build-osx.md)
- [Unix build notes](build-unix.md)
- [Windows build notes](build-windows-msvc.md)
- [Developer notes](developer-notes.md)
- [ConnectCoin public identity](connectcoin-branding.md)
- [Typed transaction outputs](typed-outputs.md)
- [Pay-to-connect protocol](pay-to-connect.md)
- [JSON-RPC interface](JSON-RPC-interface.md)
- [Testing](../src/test/README.md)

## License and upstream attribution

Distributed under the [MIT software license](../COPYING). ConnectCoin Core
copyright notices and upstream attribution are retained in inherited files.
