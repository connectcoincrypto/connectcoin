ConnectCoin Core
=============

These runtime notes describe GNU/Linux binaries produced by this repository's
Guix build. Builds using distribution-provided dependencies or different
configurations may require additional libraries or different Qt platform plugins.

General Runtime Requirements
----------------------------

These binaries require glibc (GNU C Library) 2.31 or newer.

GUI Runtime Requirements
------------------------

The packaged GUI executable, `connectcoin-qt`, is based on the Qt 6 framework and uses the `xcb` QPA (Qt Platform Abstraction) platform plugin
to run on X11. Its runtime library dependencies are as follows:
- `libfontconfig`
- `libfreetype`

On Debian, Ubuntu, or their derivatives, you can run the following command to ensure all dependencies are installed:
```sh
sudo apt install libfontconfig1 libfreetype6
```

On Fedora, run:
```sh
sudo dnf install fontconfig freetype
```

For other systems, please consult their documentation.

Desktop launcher and taskbar icon
--------------------------------

Linux desktop integration uses the application ID `org.connectcoin.ConnectCoin`.
The launcher filename and `StartupWMClass` match the Qt application's desktop
identity, including when `connectcoin-qt` is started from a file manager. This
identity is shared across networks; wallet files and per-network GUI settings
keep their existing locations and names.

Installing the GUI with CMake also installs these files under the configured
installation prefix:

- `share/applications/org.connectcoin.ConnectCoin.desktop`
- `share/icons/hicolor/1024x1024/apps/connectcoin.png`
- `share/pixmaps/connectcoin.png` (fallback for older icon themes)

The icon is the existing 1024 by 1024 pixel ConnectCoin image. These files belong
to the `connectcoin-qt` installation component, alongside the executable. The
paths honor `CMAKE_INSTALL_DATADIR` and `DESTDIR` for distribution packaging.
For a GUI already built using the instructions in [build-unix.md](build-unix.md),
install that component with:

```sh
sudo cmake --install build --component connectcoin-qt
```

The installed launcher runs `connectcoin-qt` from the desktop session's `PATH`.
When using a custom installation prefix, ensure its binary directory is on that
`PATH`, or set the launcher's `Exec` executable to the installed absolute path.
The launcher opens the application without registering a payment-link handler,
including in builds configured without wallet support.

### Portable archive, without a system-wide install

Keep the extracted archive in its intended permanent location. Open a terminal
in its root directory, where `bin/connectcoin-qt` and `share/` are located. Print
the executable's actual absolute path and install the supplied launcher and icon
for the current user:

```sh
realpath ./bin/connectcoin-qt
install -Dm644 ./share/applications/org.connectcoin.ConnectCoin.desktop \
  "${XDG_DATA_HOME:-$HOME/.local/share}/applications/org.connectcoin.ConnectCoin.desktop"
install -Dm644 ./share/icons/hicolor/1024x1024/apps/connectcoin.png \
  "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/1024x1024/apps/connectcoin.png"
install -Dm644 ./share/pixmaps/connectcoin.png \
  "${XDG_DATA_HOME:-$HOME/.local/share}/icons/connectcoin.png"
```

The last copy supplies an unthemed user icon for desktops whose installed
`hicolor` theme does not list the 1024 pixel size.

Open the installed `.desktop` file in a text editor and replace `connectcoin-qt`
in its `Exec=connectcoin-qt` line with the absolute path printed by `realpath`.
Enclose that path in double quotes. Desktop
entries do not expand `~`, `$HOME`, or other shell variables in `Exec`. For paths
containing special characters, follow the
[desktop entry quoting rules](https://specifications.freedesktop.org/desktop-entry/latest/exec-variables.html).
Keep `Icon=connectcoin` and `StartupWMClass=org.connectcoin.ConnectCoin` unchanged.
If the archive is moved later, update this absolute path again.

On Linux Mint, install `desktop-file-utils` if its tools are not already
available, then validate the edited launcher and refresh its desktop database:

```sh
desktop-file-validate "${XDG_DATA_HOME:-$HOME/.local/share}/applications/org.connectcoin.ConnectCoin.desktop"
update-desktop-database "${XDG_DATA_HOME:-$HOME/.local/share}/applications"
```

Close and reopen ConnectCoin Core. Launch it from the application menu once and
pin that menu entry to the panel; replace any older pinned entry that still
points to the extracted binary or uses an old launcher name. If the desktop
continues showing its cached generic icon, log out and back in. These steps
install desktop integration only; they do not move wallets, change the data
directory, or register a payment-link handler.
