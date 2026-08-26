*The release notes draft is a temporary file that can be added to by anyone. See
[/doc/developer-notes.md#release-notes](/doc/developer-notes.md#release-notes)
for the process.*

*version* Release Notes Draft
===============================

ConnectCoin Core version *version* is now available from:

  `CONNECTCOIN_RELEASE_URL` (replace before publishing)

This release includes new features, various bug fixes and performance
improvements, as well as updated translations.

Please report bugs using the ConnectCoin issue tracker:

  `CONNECTCOIN_ISSUE_TRACKER_URL` (replace before publishing)

Security reports and update notifications must use the project-owned channels
listed in `SECURITY.md` (verify those channels before publishing).

With the release of this new major version, versions *version minus 3* and
older are at "End of Life" and will no longer receive updates.

In accordance with the security policy, we will in two weeks disclose:

* Medium and high severity vulnerabilities fixed in *version minus 2*. There are N of these.

* Low severity vulnerabilities fixed in *version*. There are M of these.

How to Upgrade
==============

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes in some cases), then run the
installer (on Windows) or just copy over `/Applications/ConnectCoin-Qt` (on macOS)
or `connectcoind`/`connectcoin-qt` (on Linux).

Upgrading directly from a version of ConnectCoin Core that has reached its EOL is
possible, but it might take some time if the data directory needs to be migrated. Old
wallet versions of ConnectCoin Core are generally supported.

Compatibility
==============

ConnectCoin Core is supported and tested on the following operating systems or newer:
Linux Kernel 3.17, macOS 14, and Windows 10 (version 1903). ConnectCoin
Core should also work on most other Unix-like systems but is not as
frequently tested on them. It is not recommended to use ConnectCoin Core on
unsupported systems.

Notable changes
===============

P2P and network changes
-----------------------

Updated RPCs
------------


Changes to wallet related RPCs can be found in the Wallet section below.

New RPCs
--------

Build System
------------

Updated settings
----------------


Changes to GUI or wallet related settings can be found in the GUI or Wallet section below.

New settings
------------

Tools and Utilities
-------------------

Wallet
------

GUI changes
-----------

Low-level changes
=================

RPC
---

Tests
-----

*version* change log
====================

Credits
=======

Thanks to everyone who directly contributed to this release:


Translation credits must be populated from the project-owned translation
workflow before publishing. Do not reuse the inherited Bitcoin Transifex
credits or project link.
