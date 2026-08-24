# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* File ready to commit: `rach2/sys/network/Readme.md`.
* First unchecked file after this commit boundary: `krffs/Readme.md`. Do not start it before the network commit.
* The original document, current rewrite, and current diff have been read whole during this file's work.
* The only uncommitted product document is `rach2/sys/network/Readme.md`.
* Intended commit: the network Readme with its worklist, notes, and checkpoint updates.
* The file is complete and ready to commit. The worklist is marked only for this commit boundary.

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

None. The file is ready to commit.

## Checks run for this file

* `tcp_tick` and `TCP_MAX_RETX` confirm seven retransmissions followed by failure on the next timeout. The Readme sentence is corrected.
* The named assertions in `loopprobe.c`, `netifprobe.c`, `dgramprobe.c`, `packetprobe.c`, and `rawprobe.c` confirm the verification summaries. `tools/rawwire_check.py` checks hop limits and the caller's checksum, and the `raw-test` recipe runs `traceroute` and checks its first hop.
* Both BusyBox configs omit `ping`. The normal config builds `traceroute`; the set-id config omits it.
* The proposed source and test comment defects and the raw-send MTU defect were reopened at their narrow locations. Confirmed defects are recorded in `notes.md`.
* The banned-term check, direct ASCII check, phrase report, `tools/textcheck.sh`, `git diff --check`, `make verify-style`, and the 36-test `make verify-host` suite pass.

## Exact next action

Inspect the complete diff, stage the network Readme and three milestone control files, inspect the staged diff and identity, then commit.
