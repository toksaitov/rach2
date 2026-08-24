# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* First unchecked file: `rach2/sys/network/Readme.md`.
* The original document, current rewrite, and current diff have been read whole during this file's work.
* The only uncommitted product document is `rach2/sys/network/Readme.md`.
* Intended commit: the network Readme with its worklist, notes, and checkpoint updates.

## Current rewrite

The rewrite is complete in structure. It covers device selection, runtime configuration, Ethernet, ARP, IPv4, ICMP, UDP, TCP, socket ABI, packet sockets, raw sockets, netconsole, concurrency, and verification.

The changed sentences must be checked against the named source before the next edit. Do not reread the full root Makefile or the whole network tree.

## Narrow evidence map

* Probe order and pool initialization: `init_network` in `rach2/sys/network/core/net_core.c` and `netdev_probe` in `rach2/sys/network/driver/netdev.c`.
* Driver limits: constants and transmit or receive paths in `driver/rtl8139.[ch]` and `driver/rtl8168.[ch]`; host-model coverage in `tests/host/rtl8168_hosttest.c`.
* Boot and runtime configuration: `netconf_apply`, `netconf_set_address`, `netconf_set_netmask`, and `netconf_set_gateway` in `netconf.c`; `ifreq_ioctl` and `route_ioctl` in `netif.c`.
* Ethernet and ARP: `ethernet_rx`, `ethernet_tx`, `arp_cache_update`, `arp_rx`, and `arp_lookup`.
* IPv4 and loopback: `ip_rx`, `ip_tx`, and `ip_loop_drain`.
* Ping and UDP: `icmp_ping`, `udp_bind`, `udp_rx`, `udp_tx`, and `socket_udp_deliver`.
* TCP limits and retry count: constants in `tcp/tcp.h`, `tcp_tick`, listener queue operations, and close paths in `tcp/tcp.c` and `socket/socket.c`.
* Socket ABI and options: socketcall cases in `rach2/sys/syscall.c` plus the relevant operations in `socket/socket.c`, `socket/packet.c`, and `socket/raw.c`.
* Netconsole: `netconsole_putc`, `netconsole_enable`, and the frame builder in `netconsole.c`.
* Timer order: the timer handler in `rach2/kernel.c`.
* Default and specialized coverage: `tools/runtests.py`, the network targets in the root `Makefile`, and only the named assertions in `tests/{loopprobe,netifprobe,dgramprobe,packetprobe,rawprobe}.c`.
* BusyBox applet claims: direct `CONFIG_PING`, `CONFIG_TRACEROUTE`, and related entries in both project BusyBox configs.

## Open checks

* Correct the TCP retry sentence if `tcp_tick` sends seven retransmissions and closes on the following timeout.
* Read only the assertions needed from `loopprobe.c`, `netifprobe.c`, `dgramprobe.c`, `packetprobe.c`, and `rawprobe.c` to confirm the verification bullets.
* Check both BusyBox configs directly before recording any source-comment contradiction about `ping` or `traceroute`.
* Recheck each proposed non-document defect at its narrow source location before adding it to `notes.md`.
* Run the file checks, inspect the complete diff, update `notes.md` and `worklist.md`, then commit.

## Checks run for this file

None after the current rewrite. Run the complete per-file checks before commit.

## Exact next action

Read the retransmission branch of `tcp_tick` and fix the single TCP sentence if the retry count requires it. Then inspect the five specialized probe assertions in bounded chunks, check the two BusyBox configs, and finish the file without another broad source read.
