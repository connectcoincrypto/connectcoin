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

Current beta seeds
------------------

`connectcoin1.com`, `connectcoin2.com`, `connectcoin3.com` and
`dememzea.tplinkdns.com` are configured only for ConnectCoin testnet4. Operators must
maintain the required DNS records and a reachable P2P bootstrap service on TCP
48179; adding hostnames to the source does not establish service availability.
See [testnet-beta.md](testnet-beta.md#testnet4-bootstrap-dns) for the filtered
query and base-hostname fallback requirements. No mainnet seed or fixed peer
snapshot is enabled by this configuration.

See also
----------
- [bitcoin-seeder](https://github.com/sipa/bitcoin-seeder) is a reference implementation of a DNS seed.
