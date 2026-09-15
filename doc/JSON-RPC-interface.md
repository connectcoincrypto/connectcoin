# JSON-RPC Interface

The headless daemon `connectcoind` has the JSON-RPC API enabled by default, the GUI
`connectcoin-qt` has it disabled by default. This can be changed with the `-server`
option. In the GUI it is possible to execute RPC methods in the Debug Console
Dialog.

## Endpoints

There are two JSON-RPC endpoints on the server:

1. `/`
2. `/wallet/<walletname>`

### `/` endpoint

This endpoint is always active.
It can always service non-wallet requests and can service wallet requests when
exactly one wallet is loaded.

### `/wallet/<walletname>` endpoint

This endpoint is only activated when the wallet component has been compiled in.
It can service both wallet and non-wallet requests.
It MUST be used for wallet requests when two or more wallets are loaded.

This is the endpoint used by connectcoin-cli when a `-rpcwallet=` parameter is passed in.
Percent-encode the wallet name when constructing the URL. Do not append a slash:
the server treats it as part of the wallet name.

Best practice would dictate using the `/wallet/<walletname>` endpoint for ALL
requests when multiple wallets are in use.

### Examples

These examples use testnet4's default RPC port, `48178`, with a configured
RPC user named `alice`. The wallet example also requires `desc-wallet` to be
loaded. The HTTP wallet path selects the wallet; `-rpcwallet` is a client option.

```sh
# Get block count from the / endpoint
$ curl --user alice --data-binary '{"jsonrpc": "2.0", "id": "0", "method": "getblockcount", "params": []}' -H 'content-type: application/json' localhost:48178/

# Get balance from the /wallet/desc-wallet endpoint
$ curl --user alice --data-binary '{"jsonrpc": "2.0", "id": "0", "method": "getbalance", "params": []}' -H 'content-type: application/json' localhost:48178/wallet/desc-wallet

```

## Parameter passing

The JSON-RPC server supports both _by-position_ and _by-name_ [parameter
structures](https://www.jsonrpc.org/specification#parameter_structures)
described in the JSON-RPC specification. For extra convenience, to avoid the
need to name every parameter value, all RPC methods accept a named parameter
called `args`, which can be set to an array of initial positional values that
are combined with named values.

Examples:

```sh
# "params": ["mywallet", false, false, "", false, true, true]
connectcoin-cli createwallet mywallet false false "" false true true

# "params": {"wallet_name": "mywallet", "load_on_startup": true}
connectcoin-cli -named createwallet wallet_name=mywallet load_on_startup=true

# "params": {"args": ["mywallet"], "load_on_startup": true}
connectcoin-cli -named createwallet mywallet load_on_startup=true
```

`connectcoin rpc` can also be substituted for `connectcoin-cli -named`.
New wallets must use descriptors; the positional example explicitly sets
`descriptors=true`, which the named examples obtain from the default.

## Versioning

The RPC interface might change from one major version of ConnectCoin Core to the
next. This makes the RPC interface implicitly versioned on the major version.
The version tuple can be retrieved by e.g. the `getnetworkinfo` RPC in
`version`.

Usually deprecated features can be re-enabled during the grace-period of one
major version via the `-deprecatedrpc=` command line option. The release notes
of a new major release come with detailed instructions on what RPC features
were deprecated and how to re-enable them temporarily.

## Legacy JSON-RPC vs 2.0

The server recognizes [JSON-RPC v2.0](https://www.jsonrpc.org/specification) requests
and responds accordingly. A 2.0 request is identified by the presence of
`"jsonrpc": "2.0"` in the request body. Omitting `jsonrpc` selects the inherited
legacy protocol. `"jsonrpc": "1.0"` is also accepted for compatibility; other
non-null version values are rejected. New clients should use version 2.0.

|| Legacy | 2.0 |
|-|-|-|
| Request marker | none (also accepts `"jsonrpc": "1.0"`) | `"jsonrpc": "2.0"` |
| Response marker | (none) | `"jsonrpc": "2.0"` |
| `"error"` and `"result"` fields in response | both present | only one is present |
| HTTP codes in response | `200` unless there is any kind of RPC error (invalid parameters, method not found, etc) | Always `200` unless there is an actual HTTP server error (request parsing error, endpoint not found, etc) |
| Notifications: requests that get no reply | (not supported) | Supported for requests that exclude the "id" field. Returns HTTP status `204` "No Content" |

## Security

The RPC interface allows other programs to control ConnectCoin Core,
including the ability to spend funds from your wallets, affect consensus
verification, read private data, and otherwise perform operations that
can cause loss of money, data, or privacy.  This section suggests how
you should use and configure ConnectCoin Core to reduce the risk that its
RPC interface will be abused.

- **Securing the executable:** Anyone with physical or remote access to
  the computer, container, or virtual machine running ConnectCoin Core can
  compromise either the whole program or just the RPC interface.  This
  includes being able to record any passphrases you enter for unlocking
  your encrypted wallets or changing settings so that your ConnectCoin Core
  program tells you that certain transactions have multiple
  confirmations even when they aren't part of the best block chain.  For
  this reason, you should not use ConnectCoin Core for security sensitive
  operations on systems you do not exclusively control, such as shared
  computers or virtual private servers.

- **Securing local network access:** By default, the RPC interface can
  only be accessed by a client running on the same computer and only
  after the client provides a valid authentication credential (username
  and passphrase).  Any program on your computer with access to the file
  system and local network can obtain this level of access.
  Additionally, other programs on your computer can attempt to provide
  an RPC interface on the same port as used by ConnectCoin Core in order to
  trick you into revealing your authentication credentials.  For this
  reason, it is important to only use ConnectCoin Core for
  security-sensitive operations on a computer whose other programs you
  trust.

- **RPC Credentials Security Boundary:** Any client with valid RPC credentials
  should be treated as having significant control over both the ConnectCoin Core node
  and the filesystem resources accessible by the `connectcoind` process. RPC commands
  can load wallet files from paths that the `connectcoind` process has permission to
  access, specify file paths for operations, and potentially gain broader access
  than intended. This means that someone with RPC access can potentially compromise
  not only the ConnectCoin Core node, but also the machine it is running on. ConnectCoin Core
  provides the `-rpcwhitelist` option to restrict which RPC commands specific users
  can access, and `-rpcwhitelistdefault` to control the default behavior for users
  without explicit whitelists. However, when using multiple wallets or sharing access
  with different users, these should not be considered robust security boundaries, as
  users with access to certain commands may still be able to exploit functionality in
  unexpected ways. For security-sensitive operations, implement proper system-level
  isolation (containers, virtualization, separate user accounts with restricted
  permissions) rather than relying solely on RPC access controls.

- **Securing remote network access:** You may optionally allow other
  computers to remotely control ConnectCoin Core by setting the `rpcallowip`
  and `rpcbind` configuration parameters.  These settings are only meant
  for enabling connections over secure private networks or connections
  that have been otherwise secured (e.g. using a VPN or port forwarding
  with SSH or stunnel).  **Do not enable RPC connections over the public
  Internet.**  Although ConnectCoin Core's RPC interface does use
  authentication, it does not use encryption, so your login credentials
  are sent as clear text that can be read by anyone on your network
  path.  Additionally, the RPC interface has not been hardened to
  withstand arbitrary Internet traffic, so changing the above settings
  to expose it to the Internet (even using something like a Tor onion
  service) could expose you to unconsidered vulnerabilities.  See
  `connectcoind -help` for more information about these settings and other
  settings described in this document.

    Related, if you use ConnectCoin Core inside a Docker container, you may
    need to expose the RPC port to the host system.  The default way to
    do this in Docker also exposes the port to the public Internet.
    Instead, expose it only on the host system's localhost, for example for
    testnet4: `-p 127.0.0.1:48178:48178`.

- **Secure authentication:** By default, when no `rpcpassword` is specified, ConnectCoin Core generates unique
  login credentials each time it restarts and puts them into a file
  readable only by the user that started ConnectCoin Core, allowing any of
  that user's RPC clients with read access to the file to login
  automatically. By default, `.cookie` is in the selected network's data
  directory (`testnet4/` for the beta), and using these credentials is the preferred
  RPC authentication method.  If you need to generate static login
  credentials for your programs, you can use the script in the
  `share/rpcauth` directory in the ConnectCoin Core source tree.  As a final
  fallback, you can directly use manually-chosen `rpcuser` and
  `rpcpassword` configuration parameters---but you must ensure that you
  choose a strong and unique passphrase (and still don't use insecure
  networks, as mentioned above).

- **Secure string handling:** The RPC interface does not guarantee any
  escaping of data beyond what's necessary to encode it as JSON,
  although it does usually provide serialized data using a hex
  representation of the bytes. If you use RPC data in your programs or
  provide its data to other programs, you must ensure any problem strings
  are properly escaped. For example, the `createwallet` RPC accepts
  arguments such as `wallet_name` which is a string and could be used
  for a path traversal attack without application level checks. Multiple
  websites have been manipulated because they displayed decoded hex strings
  that included HTML `<script>` tags. For this reason, and others, it is
  recommended to display all serialized data in hex form only.

## RPC consistency guarantees

State that can be queried via RPCs is guaranteed to be at least up-to-date with
the chain state immediately prior to the call's execution. However, the state
returned by RPCs that reflect the mempool may not be up-to-date with the
current mempool state.

### Transaction Pool

The mempool state returned via an RPC is consistent with itself and with the
chain state at the time of the call. Thus, the mempool state only encompasses
transactions that are considered mine-able by the node at the time of the RPC.

The mempool state returned via an RPC reflects all effects of mempool and chain
state related RPCs that returned prior to this call.

### Wallet

The wallet state returned via an RPC is consistent with itself and with the
chain state at the time of the call.

Wallet RPCs will return the latest chain state consistent with prior non-wallet
RPCs. The effects of all blocks (and transactions in blocks) at the time of the
call is reflected in the state of all wallet transactions. For example, if a
block contains transactions that conflicted with mempool transactions, the
wallet would reflect the removal of these mempool transactions in the state.

However, the wallet may not be up-to-date with the current state of the mempool
or the state of the mempool by an RPC that returned before this RPC. For
example, a wallet transaction that was fee-replaced in the mempool prior to
this RPC may not yet be reflected as such in this RPC response.

## Limitations

Concurrent HTTP connections and RPC work consume file descriptors, memory, and
CPU time. Excessive client concurrency can exhaust resources or make the node
unresponsive. Bound the number of outstanding requests and monitor resource use;
there is no universally safe connection count. Raising a file-descriptor limit
does not remove the other constraints. See [File descriptor limits](file-descriptor-limits.md).
