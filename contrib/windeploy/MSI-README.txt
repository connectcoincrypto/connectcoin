ConnectCoin Core - Windows x64 installer
======================================

This package installs the Core graphical application and command-line tools.
It requires Windows 10 1809 or later (64-bit), including Windows 11.
Qt and Visual C++ libraries are included: you do not need to install
Visual Studio or Python, or download dependencies, to run Core.

Open ConnectCoin Core from the Start Menu or the desktop shortcut.
To pin it to the taskbar, use the installed Start Menu shortcut. If an old
pin still points to a portable or build-directory executable, unpin that old
shortcut and pin the newly installed one; the installer does not alter your pins.
The executables are in the bin folder of the chosen installation directory.
The default network is mainnet; an existing configuration may select another.
The installer does not automatically start mining, claims, RPC, or services.

If Core is already running, close it normally before upgrading, repairing,
or uninstalling. Wait for the process to exit and save its data. The installer
does not forcibly terminate the wallet or node.

Your data is normally stored in %LOCALAPPDATA%\ConnectCoin (or the directory
you choose in Core). Installation, repair, and uninstall do NOT remove
wallets, blockchain data, configuration, or backups from that directory.
Do not use the installation directory as a data directory.

Core is available as an option for opening connectcoin: links in
Windows Settings > Apps > Default apps. The installer does not replace
your choice of another application, such as ConnectWallet.

Tools (PowerShell, from the bin folder):
  .\connectcoin-cli.exe -help
  .\connectcoind.exe -help
  .\connectcoin-wallet.exe -help
  .\connectcoin-tx.exe -help
  .\connectcoin-util.exe -help
The share\rpcauth\rpcauth.py helper is optional and requires Python 3.
share\examples\connectcoin.conf is only an example; it is not enabled automatically.
The MSI does not change any firewall ports or permissions.

Licenses and source code
-----------------------
Core: COPYING.txt; dependencies: licenses; versions: build-info.json.
Source and build instructions: https://github.com/connectcoincrypto/connectcoin
Qt is distributed as replaceable DLLs under the applicable LGPL/GPL terms.
Source code for Qt releases: https://download.qt.io/official_releases/qt/
Dependency recipes and patches: https://github.com/microsoft/vcpkg
Microsoft Visual C++ Runtime: distributed under the applicable Microsoft terms.
The app-local Visual C++ DLLs must be updated in new packages when Microsoft
publishes fixes; this installer does not maintain them in the background.
