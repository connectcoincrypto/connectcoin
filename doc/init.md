Sample init scripts and service configuration for connectcoind
==========================================================

Sample scripts and configuration files for systemd, Upstart and OpenRC
can be found in the contrib/init folder.

    contrib/init/connectcoind.service:    systemd service unit configuration
    contrib/init/connectcoind.openrc:     OpenRC compatible SysV style init script
    contrib/init/connectcoind.openrcconf: OpenRC conf.d file
    contrib/init/connectcoind.conf:       Upstart service configuration file
    contrib/init/connectcoind.init:       CentOS compatible SysV style init script

Service User
---------------------------------

The systemd, Upstart, and OpenRC configurations assume the existence of a
"connectcoin" user and group. The CentOS script also runs as the "connectcoin"
user by default; `CONNECTCOIND_USER` can select a different dedicated service
account. The account and group must be created before using these scripts.
The macOS configuration assumes connectcoind will be set up for the current user.

Configuration
---------------------------------

Running connectcoind as a daemon does not require any manual configuration. By
default, local RPC clients authenticate with a random cookie created when the
daemon starts and removed when it exits.

The supplied Linux service scripts create an empty configuration file with
restricted permissions on first start if it does not already exist. They keep
the configuration directory and file owned by root and readable by the
connectcoin group, preventing the daemon from replacing the configuration path.

Administrators who need static credentials may instead configure `rpcauth`.
Generate the salted password hash with
[`share/rpcauth/rpcauth.py`](../share/rpcauth/rpcauth.py), store only that hash
in `connectcoin.conf`, and supply the matching username and password to the RPC
client. Use a strong, unique password because RPC access is security-sensitive,
especially when the wallet is enabled.

By default the cookie is stored in the data directory, but its location can be
overridden with the option `-rpccookiefile`. Default file permissions for the
cookie are "owner" (i.e. user read/writeable) via default application-wide file
umask of `0077`, but these can be overridden with the `-rpccookieperms` option.

This allows for running connectcoind without having to do any manual configuration.

`conf`, `pid`, and `wallet` accept relative paths which are interpreted as
relative to the data directory. `wallet` *only* supports relative paths.

To generate an example configuration file that describes the configuration settings,
see [contrib/devtools/README.md](../contrib/devtools/README.md#gen-connectcoin-confsh).

Paths
---------------------------------

### Linux

All four Linux configurations assume several paths that might need to be adjusted.

    Binary:              /usr/bin/connectcoind
    Configuration file:  /etc/connectcoin/connectcoin.conf
    Data directory:      /var/lib/connectcoind
    PID file:            /var/run/connectcoind/connectcoind.pid (OpenRC, Upstart, and CentOS) or
                         /run/connectcoind/connectcoind.pid (systemd)
    Lock file:           /var/lock/subsys/connectcoind (CentOS)

The PID directory (if applicable) and data directory should both be owned by the
connectcoin user and group. The configuration directory and file are owned by
root, with group read access for the connectcoin group; the service account must
not be able to replace the configuration file. Access to connectcoin-cli and
other connectcoind RPC clients can be controlled by group membership.

NOTE: When using the systemd .service file, systemd creates the runtime, state,
and configuration directories. The unit then makes the configuration directory
root-owned with mode 750 and the configuration file root-owned with mode 640.
The runtime and state directories use mode 710, giving the connectcoin group
access to files under them _if_ the files themselves grant group access. This
does not allow directory listings.

NOTE: It is not currently possible to override `datadir` in
`/etc/connectcoin/connectcoin.conf` with the current systemd, OpenRC, Upstart, and CentOS init
files out-of-the-box. This is because the command line options specified in the
init files take precedence over the configurations in
`/etc/connectcoin/connectcoin.conf`. However, some init systems have their own
configuration mechanisms that would allow for overriding the command line
options specified in the init files (e.g. setting `CONNECTCOIND_DATADIR` for
OpenRC).

### macOS

    Binary:              /usr/local/bin/connectcoind
    Configuration file:  ~/Library/Application Support/ConnectCoin/connectcoin.conf
    Data directory:      ~/Library/Application Support/ConnectCoin
    Lock file:           ~/Library/Application Support/ConnectCoin/.lock

Installing Service Configuration
-----------------------------------

### systemd

Installing this .service file consists of just copying it to
/usr/lib/systemd/system directory, followed by the command
`systemctl daemon-reload` in order to update running systemd configuration.

To test, run `systemctl start connectcoind` and to enable for system startup run
`systemctl enable connectcoind`

NOTE: When installing for systemd in Debian/Ubuntu the .service file needs to be copied to the /lib/systemd/system directory instead.

### OpenRC

Rename connectcoind.openrc to connectcoind and drop it in /etc/init.d.  Double
check ownership and permissions and make it executable.  Test it with
`/etc/init.d/connectcoind start` and configure it to run on startup with
`rc-update add connectcoind`

### Upstart (for Debian/Ubuntu based distributions)

Upstart is the default init system for Debian/Ubuntu versions older than 15.04. If you are using version 15.04 or newer and haven't manually configured upstart you should follow the systemd instructions instead.

Drop connectcoind.conf in /etc/init.  Test by running `service connectcoind start`
it will automatically start on reboot.

NOTE: This script is incompatible with CentOS 5 and Amazon Linux 2014 as they
use old versions of Upstart and do not supply the start-stop-daemon utility.

### CentOS

Copy connectcoind.init to /etc/init.d/connectcoind. Test by running `service connectcoind start`.

Using this script, you can adjust the path and flags to the connectcoind program by
setting `CONNECTCOIND_BIN` and `CONNECTCOIND_OPTS` in
`/etc/sysconfig/connectcoind`. You can also set `CONNECTCOIND_CONFIGFILE`,
`CONNECTCOIND_DATADIR`, `CONNECTCOIND_PIDFILE`, `CONNECTCOIND_USER`,
`CONNECTCOIND_GROUP`, and use the `DAEMONOPTS` variable there.

### macOS

Copy invalid.connectcoin.connectcoind.plist into ~/Library/LaunchAgents. Load the launch agent by
running `launchctl load ~/Library/LaunchAgents/invalid.connectcoin.connectcoind.plist`.

This Launch Agent will cause connectcoind to start whenever the user logs in.

NOTE: This approach is intended for those wanting to run connectcoind as the current user.
You will need to modify invalid.connectcoin.connectcoind.plist if you intend to use it as a
Launch Daemon with a dedicated connectcoin user.

Auto-respawn
-----------------------------------

Auto respawning is currently only configured for Upstart and systemd.
Reasonable defaults have been chosen but YMMV.
