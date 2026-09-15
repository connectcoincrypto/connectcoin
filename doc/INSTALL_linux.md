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
