### QoS (Quality of service) ###

This Linux bash script sets up `tc` to limit outgoing bandwidth. Its port filters
currently match outbound TCP traffic with source or destination port `48173`,
except for destinations in the configured local networks. That port is reserved
for mainnet, which is not available. To use the script with the current testnet4
beta, change its source and destination port filters to `48179` before running it.
Review the interface, bandwidth, and local-network settings as well; the script
changes system traffic-control and firewall rules.

Changing the ports alone is not sufficient for IPv6: the current `ip6tables`
rules mark packets with `0x4`, but the IPv6 `tc` filter for the limited class
matches `handle 2`. Do not rely on IPv6 rate limiting until that mismatch has
been separately corrected and tested.

This means one can have an always-on connectcoind instance running, and another local connectcoind/connectcoin-qt instance which connects to this node and receives blocks from it.
