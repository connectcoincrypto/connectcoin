# src/node/

The [`src/node/`](./) directory contains code that needs to access node state
(state in `CChain`, `CBlockIndex`, `CCoinsView`, `CTxMemPool`, and similar
classes).

Code in [`src/node/`](./) is meant to be segregated from code in
[`src/wallet/`](../wallet/) and [`src/qt/`](../qt/), to ensure wallet and GUI
code changes don't interfere with node operation and to allow wallet and GUI
code to run in separate processes.

As a rule of thumb, code in one of the [`src/node/`](./),
[`src/wallet/`](../wallet/), or [`src/qt/`](../qt/) directories should avoid
calling code in the other directories directly, and only invoke it indirectly
through the more limited [`src/interfaces/`](../interfaces/) classes.

Node functionality also includes implementations outside this directory, such
as [`src/validation.cpp`](../validation.cpp) and
[`src/txmempool.cpp`](../txmempool.cpp).
