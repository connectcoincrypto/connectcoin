# Windows MSI installer

The native x64 installer contains the graphical Core, daemon, wrapper, CLI,
transaction utility, wallet utility, and general utility. Qt plugins, vcpkg
runtime DLLs, and compiler-matching Visual C++ runtime DLLs accompany the
executables. There are no installer-time dependency downloads.

It installs per-machine (administrator approval) with an English
wizard, a configurable installation directory, Start Menu/desktop shortcuts,
and Windows Installer repair/uninstall/major-upgrade support. It registers Core
as a **candidate** for `connectcoin:` links in Windows Default Apps, without
overriding another wallet's association. It does not modify `PATH`, start the
application, install a service, enable mining/claims/RPC, or change the firewall.

The MSI embeds a small icon-only Windows PE resource named `CoreIcon.exe` for
advertised shortcuts and the installed-app listing. Both GUI shortcuts use icon
index 0 and `System.AppUserModel.ID=ConnectCoin.Core`, matching the GUI and
payment-link registration. The GUI also sets its window icon before startup
dialogs appear. The resource is built by the `connectcoin-msi-icon` target,
including when packaging with `--skip-build`; it is not an installed executable.
An old taskbar pin pointing at a portable/build-directory executable is not
rewritten. Unpin it and pin the installed Start Menu shortcut instead.

Application data, including `%LOCALAPPDATA%\ConnectCoin` and custom data
directories, is not included in any MSI component. Uninstall does not remove it.
Do not choose the application installation directory as a data directory.
Close Core normally and wait for it to finish before upgrading/uninstalling.
The installer does not forcibly terminate a node or wallet. In-use files may
require a reboot; automatic reboots and Restart Manager shutdowns are disabled.

## Optional Huge Pages configuration

The final wizard page offers an **unchecked** "Configure Huge Pages for mining"
option. It opens the installed `connectcoin-huge-pages.exe configure` helper
after the MSI transaction has committed. A Start Menu entry opens the same
helper later. The helper has its own explicit, No-by-default confirmation and
UAC request; it never starts or elevates Core. Declining leaves ordinary-page
mining available. Silent install, repair, administrative extraction, rollback,
and uninstall do not invoke the helper or change account permissions.

It grants only `SeLockMemoryPrivilege` to the current account and preserves
existing rights. The elevated token must have the same SID captured before UAC;
over-the-shoulder credentials for a different administrator are refused. A
standard user's administrator can instead assign that user's right manually.
The permission is account-wide and persistent, not a Core-only sandbox. No RAM
is reserved by the helper, but applications using the right can keep physical
memory resident, reducing reclaimable memory. A new sign-in (or reboot) is
needed after assignment. Core itself runs unelevated and reports allocation
success/fallback; the right does not guarantee sufficient contiguous RAM.

`connectcoin-huge-pages.exe status` is read-only. Uninstall deliberately leaves
the right intact: it may have existed beforehand or be used by another program.
An administrator can explicitly revoke the account's "Lock pages in memory"
right in Local Security Policy. Group-policy-managed rights must be handled by
the responsible administrator. No script execution-policy change is required.

The configurator is a small static-runtime native target, built explicitly by
the MSI packager even with `--skip-build`; it has no Python/PowerShell dependency.
Its shortcut uses `ConnectCoin.Core.HugePages`, separate from the wallet's
taskbar identity. Package verification checks the unchecked opt-in, exact helper
action/arguments and UI-only scheduling, and runs only read-only helper modes.
Test UAC cancellation, alternate administrator rejection and consent/grant in a
disposable Windows VM before distribution; those are not proven by extraction.

## Build

Requirements: Windows x64, Python 3.11+, a configured **dynamic `x64-windows`** MSVC
build with GUI, daemon, CLI, wallet, transaction and utility targets enabled,
and compiler-matching redistributables installed by Visual Studio. See
[the MSVC guide](build-windows-msvc.md). Static/MinGW/ARM64 packaging is not
implemented by this script. The runtime baseline is Windows 10 1809+ / Windows
11 x64, matching the bundled Qt build; this is not a promise of OS vendor support.

From Developer PowerShell, in the repository root:

```powershell
cmake -B build --preset vs2026 -DBUILD_GUI=ON -DENABLE_WALLET=ON -DBUILD_CONNECTCOIN_BIN=ON -DBUILD_DAEMON=ON -DBUILD_CLI=ON -DBUILD_TX=ON -DBUILD_UTIL=ON -DBUILD_WALLET_TOOL=ON
$tools = & .\contrib\windeploy\get_msi_tools.ps1
py -3 contrib/windeploy/build_msi.py --wix $tools.Wix --ui-extension $tools.UiExtension
```

The tool bootstrap downloads **WiX 5.0.2**, verifies pinned SHA-256 hashes, and
administratively extracts its CLI without globally installing WiX. It also
checks the CLI and extension's Authenticode signatures. WiX 5 is the last major
version before WiX's additional maintenance-fee terms; it is now a legacy build
tool, not an application prerequisite. Upgrading to WiX 6/7 requires reviewing
their terms and updating the build script, not silently accepting an agreement.
The tooling MSI's hash is published in its official GitHub release metadata;
the UI extension archive's hash was verified against the NuGet catalog.

Output defaults to `build/msi/`:

- `connectcoin-core-<version>-win64.msi`
- `.msi.sha256`: artifact checksum (not a signature)
- `.manifest.json`: payload sizes/hashes, source commit, dirty-tree marker,
  Qt version, and build dependency versions
- `.wixpdb`: installer build diagnostics; not needed for installation

Each invocation stages into a new `build/msi-work-*` directory; it never clears
an existing directory and refuses to overwrite an existing MSI. Use
`--output-dir` for another output directory, `--jobs` for compilation parallelism,
or `--skip-build` only when all Release targets are already up to date.
The builder deliberately leaves compiler/ICE validation errors enabled.

MSI uses **three version fields** (`major <= 255`, `minor <= 255`, `build <= 65535`).
The default is the Core version. An in-place upgrade needs a larger MSI version,
including repackaging the same Core version; use `--package-version` for this.
Do not add a fourth revision (Windows Installer ignores it), reuse the same MSI
version for in-place upgrades, or change the fixed UpgradeCode.
Lower-version installs and different packages with the same version are blocked.
The original MSI can still repair its own installation. To replace an installed
MSI with a rebuilt package that keeps the same version (such as Core 1.0.0),
uninstall the existing MSI first, then install the replacement. This preserves
the wallet/data directory as described above.

This MSI does not upgrade the old NSIS `.exe` installer. Uninstall an old NSIS
installation first; preserve its wallet/data directory. Do not point MSI at an
existing portable installation with unrelated files.

## Verify and sign

Run the package verification script against the MSI and its adjacent manifest.
The verifier also requires GNU `objdump` (for example from MinGW) on `PATH`, or
an explicit `--objdump C:\path\to\objdump.exe` argument:

```powershell
py -3 contrib/windeploy/test_msi.py build/msi/connectcoin-core-1.0.0-win64.msi
```

Also test install, repair, upgrade, cancellation/rollback, and uninstall in a
clean disposable Windows VM before public distribution. Verify that preexisting
wallet/data files and another wallet's default URL association survive. A local
administrative extraction does **not** prove those full lifecycle scenarios.
Do not run installation lifecycle tests on a machine holding real wallets.

The verifier checks shortcut icon/AppUserModelID metadata and extracts the
embedded PE icon and GUI executable icons using Windows at 16-256 pixel sizes.
The isolated `connectcoin-test-desktop-icons` Qt test checks early and main-window
icons and the runtime Windows application identity without starting a node.

The script does **not** sign the result. For distribution, sign binaries and
the finished MSI using the project's Authenticode certificate and a trusted
timestamp service, then recompute the checksums and reverify the signed package.
Never commit a certificate's private key/password. Unsigned builds may display
an unknown-publisher/SmartScreen warning.

The app-local MSVC runtime is deliberately included to make this a single MSI;
rebuild packages when Microsoft publishes runtime security updates. Follow the
Visual Studio redistribution terms. Qt remains dynamically linked with notices
and source references provided; preserve corresponding source/patch availability
and all applicable third-party license obligations when distributing builds.
