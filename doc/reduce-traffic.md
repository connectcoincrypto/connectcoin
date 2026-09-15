Reduce Traffic
==============

Some node operators need to deal with bandwidth caps imposed by their ISPs.

By default, ConnectCoin Core allows up to 200 automatic peer connections, with 11
slots reserved for outbound connections and at most 189 available for inbound
connections. The outbound allocation is 8 full-relay peers, 2 block-relay-only
peers, and 1 feeler or extra peer-management connection. At most 50% of inbound
slots can be used by transaction-relaying peers with the default
`-inboundrelaypercent`; the remainder can serve lower-traffic block-relay-only peers.
Manually added peers and private broadcast connections use the separate limits
described below.

The default settings can result in relatively significant traffic consumption.

Ways to reduce traffic:

## 1. Use `-maxuploadtarget=<MiB per day>`

A major component of the traffic is caused by serving historic blocks to other nodes
during the initial blocks download phase (syncing up a new node).
This option can be specified in MiB per day and is turned off by default.
This is *not* a hard limit; only a threshold to minimize the outbound
traffic. When the limit is about to be reached, the uploaded data is cut by no
longer serving historic blocks (blocks older than one week).
Keep in mind that new nodes require other nodes that are willing to serve
historic blocks.

Peers with the `download` permission will never be disconnected, although their traffic counts for
calculating the target.

## 2. Disable "listening" (`-listen=0`)

Disabling listening removes inbound connections. With the default connection settings,
there are up to 11 automatic outbound peers; manually added peers and short-lived
private broadcast connections have separate limits. Fewer connected nodes generally
reduce traffic, as you relay blocks and transactions to fewer nodes.

## 3. Reduce maximum connections (`-maxconnections=<num>`)

Reducing the maximum connected nodes to a minimum could be desirable if traffic
limits are tiny. Keep in mind that ConnectCoin's trustless model works best if you are
connected to a handful of nodes.

`-maxconnections` limits automatic connections, including outbound connections when
listening is disabled. It does not limit `-addnode`/`addnode` RPC connections (separate
limit of 8) or short-lived private broadcast connections (separate limit of 64 when
`-privatebroadcast` is enabled, which is not the default).

## 4. Turn off transaction relay (`-blocksonly`)

Forwarding transactions to peers increases the P2P traffic. To only sync blocks
with other peers, you can disable transaction relay.

Be reminded of the effects of this setting.

- Fee estimation will no longer work.
- It sets the flag "-walletbroadcast" to be "0", only if it is currently unset.
  Doing so disables the automatic broadcasting of transactions from wallet. Not
  relaying other's transactions could hurt your privacy if used while a wallet
  is loaded or if you use the node to broadcast transactions.
- If a peer has the forcerelay permission, we will still receive and relay
  their transactions.
- It makes block propagation slower because compact block relay can only be
  used when transaction relay is enabled.
