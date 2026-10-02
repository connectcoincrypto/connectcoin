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
The MSI does not change any firewall ports. System permissions are unchanged
unless you explicitly approve the separate optional Huge Pages configurator.

Optional Huge Pages for CPU mining
---------------------------------
The final installer page offers an UNCHECKED "Configure Huge Pages for mining"
option. It opens a separate helper, not the wallet. You can also open
"Configure Huge Pages" from the ConnectCoin Core Start Menu folder later.
It explains the change and asks for confirmation (No is the default), then
requests administrator approval through UAC if needed. Cancelling does not
affect the installation or ordinary mining. Silent installation does nothing.

The helper adds only "Lock pages in memory" (SeLockMemoryPrivilege) to the
current account; it does not remove existing permissions or reserve RAM.
This permission applies to the account, not just ConnectCoin. Applications
can keep memory resident, leaving less RAM reclaimable by Windows.
If UAC uses a DIFFERENT administrator account, the helper refuses to modify
either account; ask that administrator to configure your account's right.
Sign out and sign in again after a new grant (or restart Windows). Then run
Core normally, not as administrator. The Mining page reports the dataset's
allocation result; permission alone cannot guarantee sufficient contiguous RAM.

Read-only diagnostics from the bin folder:
  .\connectcoin-huge-pages.exe status
Nothing automatically starts mining, logs you out, reboots, or changes JIT.
Repair and uninstall leave this account-level permission intact because other
applications may use it. To revoke it explicitly, an administrator can remove
your account from "Lock pages in memory" under Local Security Policy > Local
Policies > User Rights Assignment. Do not remove other accounts or group policy.

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
