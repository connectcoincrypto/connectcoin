### QoS (Quality of service) ###

This is a Linux bash script that will set up `tc` to limit outgoing bandwidth for connections to the ConnectCoin mainnet. It limits outbound TCP traffic with a source or destination port of 48173, but not if the destination IP is within a LAN.

This means one can have an always-on connectcoind instance running, and another local connectcoind/connectcoin-qt instance which connects to this node and receives blocks from it.
