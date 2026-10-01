ConnectCoin Core - Linux x86_64 portable bundle
=============================================

This bundle includes the graphical wallet, node, command-line client, wallet,
transaction and utility tools, and the connectcoin command dispatcher.
Qt 6 and redistributable shared libraries are included.

Compatibility
-------------
Built on Ubuntu 22.04, targeting x86_64 distributions with glibc 2.35 or newer.
Other distributions still need compatible system graphics drivers, a desktop
session for the GUI, and the system libraries excluded from the bundle. This
is not a guarantee of compatibility with every Linux distribution. Alpine
and other musl-only systems are not supported.

Run the AppImage
----------------
Mark the downloaded file executable in your file manager, or use:
  chmod +x ConnectCoin-Core-<version>-x86_64.AppImage
  ./ConnectCoin-Core-<version>-x86_64.AppImage

If FUSE is unavailable, use the built-in extraction mode:
  APPIMAGE_EXTRACT_AND_RUN=1 ./ConnectCoin-Core-<version>-x86_64.AppImage

Command-line tools are available through the AppImage too:
  ./ConnectCoin-Core-<version>-x86_64.AppImage --tool connectcoin-cli -help
  ./ConnectCoin-Core-<version>-x86_64.AppImage --tool connectcoind -help

Run the tar.gz bundle
---------------------
Extract the archive, keep the entire extracted folder together, and run:
  ./AppRun
  ./connectcoin-cli -help
  ./connectcoind -help
  ./connectcoin-wallet -help
  ./connectcoin-tx -help
  ./connectcoin-util -help
  ./connectcoin --help

Use these launchers, which set the bundled library and Qt plugin paths. The
executables and libraries under usr/ must stay in their relative locations.
No root access or installation is required. This bundle does not install a
service, change firewall rules, or automatically enable RPC or mining.

Data and upgrades
-----------------
Portable refers to the application files. Core continues to use its normal
Linux configuration, wallet and blockchain data directory (normally
~/.connectcoin, or the directory selected in Core). It does not move your
data into the bundle. Close Core normally before replacing the application
folder. Removing the bundle does not remove your data or wallet backups.

Licenses and source
-------------------
Core's license and staged dependency notices are under usr/share/doc/ and
usr/share/licenses/ when applicable. linuxdeploy copies distribution copyright
notices for bundled shared libraries under usr/share/doc/. Qt is dynamically
linked under its applicable LGPL/GPL terms. The libraries in usr/lib and Qt
plugins in usr/plugins can be replaced with compatible modified versions in
the extracted tar bundle or an extracted AppImage. Core does not restrict
reverse engineering for debugging modifications to these libraries.

Core source and build instructions:
  https://github.com/connectcoincrypto/connectcoin
Qt source releases:
  https://download.qt.io/official_releases/qt/
Ubuntu dependency source packages, including distribution patches:
  https://packages.ubuntu.com/jammy/
AppImage runtime source and license:
  https://github.com/AppImage/type2-runtime/tree/20251108
  usr/share/doc/connectcoin/AppImage-runtime-LICENSE.txt

PACKAGING.txt records the Qt version, observed glibc requirement and packaging
tool hashes. Updates to this bundle supply updated bundled dependencies.
