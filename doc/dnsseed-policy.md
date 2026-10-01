Expectations for DNS Seed operators
====================================

ConnectCoin Core attempts to minimize the level of trust in DNS seeds,
but DNS seeds still pose a small amount of risk for the network.
As such, DNS seeds must be run by entities which have some minimum
level of trust within the ConnectCoin community.

Other implementations of ConnectCoin software may also use the same
seeds and may be more exposed. In light of this exposure, this
document establishes some basic expectations for operating dnsseeds.

0. A DNS seed operating organization or person is expected to follow good
host security practices, maintain control of applicable infrastructure,
and not sell or transfer control of the DNS seed. Any hosting services
contracted by the operator are equally expected to uphold these expectations.

1. The DNS seed results must consist exclusively of fairly selected and
functioning ConnectCoin nodes from the public network to the best of the
operator's understanding and capability.

2. For the avoidance of doubt, the results may be randomized but must not
single-out any group of hosts to receive different results unless due to an
urgent technical necessity and disclosed.

3. The results may not be served with a DNS TTL of less than one minute.

4. Any logging of DNS queries should be only that which is necessary
for the operation of the service or urgent health of the ConnectCoin
network and must not be retained longer than necessary nor disclosed
to any third party.

5. Information gathered as a result of the operators node-spidering
(not from DNS queries) may be freely published or retained, but only
if this data was not made more complete by biasing node connectivity
(a violation of expectation (1)).

6. Operators are encouraged, but not required, to publicly document the
details of their operating practices.

7. A reachable email contact address must be published for inquiries
related to the DNS seed operation.

If these expectations cannot be satisfied the operator should
discontinue providing services and contact the active ConnectCoin Core
maintainers through the [project issue tracker](https://github.com/connectcoincrypto/connectcoin/issues)
for non-sensitive operational reports. Do not post credentials or private keys.

Behavior outside of these expectations may be reasonable in some
situations but should be discussed in public in advance.

Current bootstrap hostnames
---------------------------

| Network | Built-in DNS/DDNS hostnames | Native P2P port |
| --- | --- | ---: |
| Mainnet | `connectcoin2.com`, `connectcoin3.com`, `connectcoin4.com`, `dememzea.tplinkdns.com` | 48173 |
| Testnet4 | `connectcoin1.com` | 48179 |

Operators must maintain the required DNS records and reachable P2P bootstrap
services running the matching ConnectCoin network. Mainnet nodes must use the
[launch genesis](mainnet-genesis.md) and select mainnet (the default, or
`-chain=main`); testnet4 nodes must explicitly select `-testnet4` or
`-chain=testnet4`. Changing a hostname's source assignment does not switch the
network running on a VPS. This configuration deploys no nodes, DNS records or
services and does not establish their availability. Fixed seed arrays remain
empty on every network; testnet3, signet and regtest have no public DNS seeds.

The discovery code queries A/AAAA records for `x9.<seed>`. These filtered
results must advertise reachable, non-pruned nodes with
`NODE_NETWORK | NODE_WITNESS` and v2 transport support, on the selected
network's native P2P port. If the filtered name has no addresses, or a name
proxy is used, the client falls back to connecting to the base hostname on
that same port to request peer addresses. Base hostnames therefore must also
resolve to P2P nodes, not just DNS servers or website/CDN addresses. See
[testnet-beta.md](testnet-beta.md#testnet4-bootstrap-dns) for a static DNS setup
example using testnet4's hostname and port.

A DDNS provider may not allow a nested record such as
`x9.dememzea.tplinkdns.com`. The base-hostname fallback still works when
`dememzea.tplinkdns.com` resolves to a reachable mainnet node on TCP 48173.
Keep its dynamic address updated and allow incoming P2P connections; DDNS
alone does not bypass NAT or a firewall. Multiple names pointing to the same
node do not provide independent bootstrap redundancy.

See also
----------
- [bitcoin-seeder](https://github.com/sipa/bitcoin-seeder) is a reference implementation of a DNS seed.
