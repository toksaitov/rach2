# `network/`: the IPv4 network stack

The network stack connects the machine to Ethernet: drivers for three cards, Ethernet, ARP, IPv4 with a routing table, ICMP, UDP and TCP, the sockets programs open on them, packet, raw and netlink sockets, and netconsole, which broadcasts the kernel log. It exists so that programs written for Linux, Dropbear and the DHCP clients among them, reach QEMU's user network and a real LAN through Linux's socket calls and configuration interfaces. The C is the same on i686 and x86-64; the architectures differ in how a program enters the socket calls and in the layout of the structures it passes. AF_UNIX sockets are in [`ipc/`](../ipc/Readme.md).

## Interfaces

### System calls

A program uses Linux's socket calls, each with its page in the manual: [socket(2)](../../../man/man2/socket.2), which lists the families and types, [bind(2)](../../../man/man2/bind.2), [connect(2)](../../../man/man2/connect.2), [listen(2)](../../../man/man2/listen.2), [accept(2)](../../../man/man2/accept.2), [getsockname(2)](../../../man/man2/getsockname.2), [send(2)](../../../man/man2/send.2), [recv(2)](../../../man/man2/recv.2), [setsockopt(2)](../../../man/man2/setsockopt.2) and [shutdown(2)](../../../man/man2/shutdown.2). Both architectures register the direct calls Linux defines for them. i686 also has `socketcall`, call 102, which takes an operation number and an array of arguments ([socketcall(2)](../../../man/man2/socketcall.2)); x86-64 has none. [syscalls(2)](../../../man/man2/syscalls.2) gives the numbers. [ping(2)](../../../man/man2/ping.2), Rach2's private echo request, is `socketcall` operation 0x1000 or 0x1001 on i686 and call 400, `rach2_ping`, on x86-64. Each call reaches `socket_dispatch` in [`../syscall_net.c`](../syscall_net.c), which passes a call on an AF_UNIX socket to `ipc/unix.c`.

### Configuration

* The network-interface requests of [ioctl(2)](../../../man/man2/ioctl.2): addresses, masks, flags, routes and ARP entries.
* [netlink(7)](../../../man/man7/netlink.7): the requests of `ip`, glibc's `getifaddrs` and `ss`.
* `/proc/net/*` in [proc(5)](../../../man/man5/proc.5) and `/sys/class/net` in [sysfs(5)](../../../man/man5/sysfs.5).
* The boot line: [cmdline.md](../../cmdline.md) lists `rach2.ip=`, `rach2.gateway=`, `rach2.ifaddr=`, `rach2.netconsole`, `rach2.nonet` and `rach2.nicfail=`.

With no address option the first interface takes QEMU's user network, SLIRP: 10.0.2.15/24 through 10.0.2.2 (`net.h`). `rach2.ip=A.B.C.D/prefix` replaces it, with no gateway unless `rach2.gateway=A.B.C.D` gives one, and `rach2.ip=none` leaves the interface unconfigured for a DHCP client. `netconf_apply` (`netconf.c`) refuses an option given twice, a gateway with no address or beside `none`, a number above 255 or with a leading zero, trailing text, a prefix outside 1 to 30, a first octet of 0, 127, or 224 and above, the subnet's network or broadcast address, and a gateway that is not another host of the subnet; a refusal logs `net: address refused: REASON` and keeps the SLIRP configuration. `rach2.ifaddr=MAC,A.B.C.D/prefix[,GATEWAY]`, once per card, configures the card with that station address under the same rules, after the probe, whatever order the cards were found in; a token that does not parse, is longer than 79 characters or names no card is logged and skipped.

### Inside the kernel

* `init_network` (`net.h`) brings the stack up at boot, after PCI enumeration and interrupts, unless the boot line has `rach2.nonet`, and returns 1 when a card came up; boot then calls `netconsole_enable` for `rach2.netconsole`.
* The boot processor's timer tick, 100 times a second, calls `netdev_poll`, `tcp_tick` and `ip_loop_drain`, in that order (`kernel.c`). A system call's exit and the idle wait also drain the loopback ring.
* `kernel_shutdown` stops every card after its last log record, and `klog` gives every record to `netconsole_putc`.
* `poll`, `ioctl` and `procfs` reach the stack through `socket_poll`, `netif_ioctl`, and copies made under the lock such as `tcp_snapshot`.

A driver implements `struct netdev_ops` (`driver/netdev.h`). `init(nd, nth)` brings up its `nth` card as the interface `nd` and returns `NETDEV_UP`, `NETDEV_ABSENT` for no such card, having touched nothing, or `NETDEV_FAILED` with `error` giving the reason. `tx` sends one frame without its frame check sequence and reports whether the card took it. `poll` is null for a card that interrupts. `stop` turns the receiver and transmitter off, masks the card's interrupts and clears PCI bus mastering, and must be safe on a card that never finished starting and safe to call twice. `link` reports the link, or 0 when the card cannot tell, which `IFF_RUNNING` takes as up. A driver gives each received frame to `ethernet_rx` with its interface and reports one it refused through `ethernet_note_rx_error` or `ethernet_note_rx_drop`.

## Design

### The interface record and the probe

An interface is `struct netdev`: the driver's operations and record for the card, the name and index, the station address, the first address with its mask and gateway, up to `NETDEV_ADDRS` (7) more addresses with their masks, the counters, and whether an administrator has it up. A kernel drives at most `NETDEV_MAX` (4) cards.

`netdev_probe` (`driver/netdev.c`) asks the drivers in the order of its table, RTL8139, e1000, RTL8168GU, each for its cards until it answers absent, and names the cards that come up `eth0` upward, with indexes from 1; on a machine with an RTL8139 and an e1000, `eth0` is the RTL8139. A card that fails is stopped, since its bring-up may have enabled bus mastering, and its reason is logged as `net: absent: REASON` when no card came up. The probe goes on to the next card and driver, and gives a driver up after `NETDEV_MAX` failures. `rach2.nicfail=N` makes the Nth card that came up, from zero, fail.

After the cards comes `lo`, with the next index: 127.0.0.1/8, Linux's loopback type 772 and MTU 65536, always up. The ioctls, netlink, `/proc/net/dev` and `/sys/class/net` list it, and no route uses it; its counters count the loopback ring, and an address, a route or taking it down is `EOPNOTSUPP`.

### The drivers

`driver/rtl8139.c` drives one RTL8139, QEMU's PCI 10ec:8139, through I/O ports. Receive is an 8192-byte ring with a 2064-byte overhang, so a frame that wraps is contiguous; a ring header with a bad status or a length outside 18 to 1518 bytes resynchronizes the ring, and an overflow is counted as a drop and drained. Transmit takes the four descriptors in turn, each with a 1536-byte buffer.

`driver/e1000.c` drives up to `E1000_MAX` (4) Intel 82540EM, PCI 8086:100e, QEMU's default card, through the memory window, with the I/O window for the reset. Its rings have 32 descriptors each, with 2048-byte receive buffers, in frames below 4 GiB. Receive takes a frame only whole in one descriptor with its error byte clear, and a full transmit ring is waited on for ten 100-microsecond intervals before the frame is dropped as a stall. It is the descriptor-ring design with nothing added, to read before the RTL8168.

`driver/rtl8168.c` drives one RTL8168, PCI 10ec:8168, whose `TxConfig` revision is of the GU family, the Lenovo G50-70's chip, which QEMU does not emulate. It is a port of Linux v6.16, commit `038d61fd642278bab63ee8ef722c50d10ab01e8f`, `r8169_main.c` and `r8169_phy_config.c`, for `RTL_GIGA_MAC_VER_42`, without the firmware patch, and each register sequence's comment gives the Linux function it follows. It has 64-descriptor rings and 16383-byte receive buffers. Completions and link changes arrive as message-signalled interrupts where the machine can route them, and a card whose MSI enable fails gives its vector back, with the handler. The handler masks the card's sources, acknowledges, services the rings at most twice and unmasks, which arms the next message. The tick services the rings too, and alone where messages cannot be routed or after a storm: 16 messages in a row that find a source the acknowledgement does not clear, with the rings empty, leave the sources masked. It advertises 10 and 100 Mbit in both duplexes, 1000 full duplex and pause, and resets the card's tally counters, which a warm restart keeps.

The e1000 and RTL8168 `stop` first waits up to 10 ms for the frames given to the card, so the last log line before shutdown reaches the wire.

### Receive

A driver gives each frame to `ethernet_rx` (`ethernet/ethernet.c`) from its interrupt handler or, for the RTL8168, also from the tick. It drops a frame shorter than 14 bytes and every frame on an interface that is down, counts the rest, offers each ARP and IPv4 frame to the packet sockets, and gives the payload to `arp_rx` or `ip_rx`.

`ip_rx` (`ip/ip.c`) drops a datagram with a bad version, length or header checksum, and every fragment, and skips options. It accepts one for any address of any interface, whichever card it came in on, for a subnet broadcast or 255.255.255.255, and, from the loopback ring only, for 127/8. It drops one from the wire whose source is this machine's or in 127/8, and gives a broadcast to UDP alone. An accepted datagram is copied to the raw sockets of its protocol, then given to ICMP, UDP or TCP; a unicast UDP datagram for a port no socket has is answered with a port unreachable.

### Transmit and routing

`ip_tx_from` builds a 20-byte header with DF set and the sender's hop limit, refuses a payload over 1480 bytes with `EMSGSIZE`, and sends the datagram:

* To 127/8 or an address an interface has: onto the loopback ring.
* To a broadcast address: UDP only, to the all-ones hardware address, by the interface on that subnet, or the first card for 255.255.255.255.
* Otherwise by the route `netdev_route` chooses, to the next hop's hardware address from ARP, or `ENETUNREACH` with no route. On a miss an ARP request goes out and the datagram waits in one of `IP_HOLD_SLOTS` (4) slots, for at most 300 ticks, until the reply sends it, so the first ping or SYN to an uncached host is not lost.

The routing table (`route.c`) is the routes the interfaces imply, each address's subnet on the link and each gateway as its interface's default route, and up to `NETDEV_ROUTES` (32) added by `SIOCADDRT` or `RTM_NEWROUTE`. A lookup takes, among the routes of interfaces that are up whose prefix covers the destination, the longest prefix, then the lowest metric, then the first found, so an implied route comes before an added one, as on Linux. The source is the connection's or socket's bound address, 127.0.0.1 for 127/8, and otherwise the outgoing interface's address on the next hop's subnet, or its first (`netdev_source`).

`ethernet_tx` adds the header with the interface's station address, and refuses a payload over 1500 bytes, IP and ARP on an interface that is down, and every frame for a card whose link is down; each refusal counts as a transmit drop. A refusal reaches a UDP or raw sender as `ENOBUFS`, and TCP keeps the segment to send again.

### Addresses and ARP

An interface answers ARP for each of its addresses, and a datagram to any of them is this machine's. Changing an interface's address (`addr_publish` in `netconf.c`) drops the ARP mappings learned on it, all of them for the first interface, and the datagrams waiting for ARP; aborts each TCP connection opened from an address the machine no longer has, with `ENETRESET`; clears a gateway off the new subnet; and removes the added routes whose gateway it no longer reaches.

The ARP cache (`arp/arp.c`) has `ARP_CACHE_SIZE` (16) entries, each with the interface it was learned on, and a lookup matches only on that interface. It learns the sender of every request and reply, unless that address cannot be a host's (a first octet of 0, 127, or 224 and above) or is the receiving subnet's network or broadcast address. A lookup refreshes an entry, and a full cache replaces the learned entry unused longest. No packet or address change touches an entry set by hand as permanent, by `arp -s` or `ip neigh add`; only `arp -d` or `ip neigh del` removes it.

### Loopback

A datagram to this machine never reaches a card. `ip_tx_from` copies it onto a ring of `LOOP_RING_SLOTS` (64), and `ip_loop_drain_from` delivers the ring through `ip_rx` from the tick, the idle wait, and a system call's exit once 3 or more wait (`LOOP_DRAIN_AT`). Delivery waits so that a send never runs the receive path, and TCP, inside the sender's own call; the threshold leaves a pair to the timer, since a simultaneous open's two SYNs cross only while neither has been delivered. A full ring refuses with `ENOBUFS` and counts the refusal in `/proc/stat`.

### ICMP and UDP

`ip/icmp.c` answers an echo request from the address it was sent to. A report that a TCP segment in flight was too big goes to `tcp_path_mtu`, and a port unreachable to `udp_refused`, which gives the UDP socket connected to that peer `ECONNREFUSED`. An error this machine sends quotes as much of the datagram as fits 576 bytes, goes to no source of 0.0.0.0, broadcast or multicast, and, unless it goes to this machine, is limited to one a second after a burst of six.

`udp/udp.c` keeps one list of bindings, a record from the heap for each bound socket, with the effective user ID that bound it; [bind(2)](../../../man/man2/bind.2) gives the rules for sharing a port. A datagram goes to the socket connected to its sender, else to one bound to its destination address, else to one bound to any. A socket's record queues up to `SOCK_DGRAM_SLOTS` (8) datagrams of up to 1500 bytes, and one that finds the queue full is dropped.

### TCP

A control block, `struct tcb` (`tcp/tcp.h`), is allocated from the heap by `listen`, by `connect`, or for an incoming SYN, and freed when the connection is done. Its 64 KiB send ring is in the block; its receive ring is from the heap, 512 KiB when more than `TCP_BIG_RING_FRAMES` (32768) frames are free as the connection opens, else 64 KiB.

Every SYN this side sends offers an MSS of 1460, a window scale shift of 4 for the large ring or 1 for the small, and selective acknowledgement. When the peer's SYN lacks window scaling neither side scales, and at most 65535 bytes are offered. The peer's MSS is taken between 48 and 1460 bytes, or 536 when its SYN has none. The window offered is the receive ring's free space; after a read an update goes out once the window has grown by the smaller of one MSS and half the ring (`tcp/rcvwnd.c`, RFC 1122 4.2.3.3).

The congestion window starts at four segments and the slow-start threshold at 65535 bytes (RFC 5681). The third duplicate acknowledgement in a row resends the missing segment and begins NewReno recovery (RFC 6582), in which each partial acknowledgement resends the next hole. A timeout sets the window to one segment and resends from the oldest unacknowledged byte. The round trip is sampled one segment at a time, never from a resend (RFC 6298, Karn's rule); [connect(2)](../../../man/man2/connect.2) gives the timeout's bounds, resets (RFC 5961) and the initial sequence number (RFC 6528). A report that a segment was too big lowers the segment size to the link's MTU less 40, with 536 as the floor (RFC 1191). A closed peer window is probed.

Data past a hole is kept in the receive ring in up to `TCP_OOO_RANGES` (8) ranges, and, when both SYNs permitted it, an acknowledgement lists up to 4 of them in a SACK option, the latest first (RFC 2018). A segment the driver refuses stays in the send ring; each tick resends for up to `TCP_TICK_PUMPS` (4) connections in rotation, and a connection whose transmits are all refused for 30 seconds is given up.

A listener's completed connections wait in a ring of `SOCK_ACCEPT_BACKLOG` (16) slots in its socket, one kept empty ([listen(2)](../../../man/man2/listen.2)). A SYN that finds the ring full, or no memory for a block, is answered with a reset; one that comes while free memory is within the reserve of [memory(7)](../../../man/man7/memory.7) is dropped for the peer to resend. The connection takes the address the SYN was sent to, and a SYN goes to a listener bound to that address before one bound to any. The socket's last close detaches the block (`tcp_close`), which finishes the close alone: `TIME_WAIT` lasts two seconds, and a detached block in `FIN_WAIT_2` is freed after ten.

### Sockets

A socket, `struct socket` (`socket/socket.h`), is allocated from the heap when it is opened, or for an incoming SYN in interrupt context, which the heap allows since it masks interrupts and never sleeps, and is freed when its last reference goes. A descriptor of type `FD_TYPE_SOCKET` refers to it, and `dup` and `fork` share it. An accepted connection's socket takes its owner from the listener. A socket made in a network namespace other than the first reaches no interface ([`../proc/ns.Readme.md`](../proc/ns.Readme.md)).

Blocked callers and pollers share the socket's wait queue, and each wake gives its kind for an edge-triggered `epoll`: a datagram on a UDP, raw, packet or netlink socket, and TCP data the receive ring took, are data; an acknowledgement that frees send space is room; a connection that completes or ends, a FIN, and a connection queued on a listener are every kind. Data that comes to a socket watched for output alone is no edge, as in Linux.

### Packet and raw sockets

A packet socket (`socket/packet.c`), `AF_PACKET` with `SOCK_DGRAM`, is how a DHCP client sees an offer, which `ip_rx` drops since it comes before the machine has the address, and what `arping` uses for ARP. It is offered each ARP or IPv4 frame of its protocol before the kernel's handler, which still runs, from every interface when bound to index 0, or from the one it is bound to, which it also sends through. A receive gives the payload and a 20-byte `sockaddr_ll`, whose packet type is judged against the receiving card's station address.

A raw socket (`socket/raw.c`), `AF_INET` with `SOCK_RAW` and `IPPROTO_ICMP`, is what `ping` and `traceroute` use. It receives a copy of every accepted ICMP datagram, header included, and sends the caller's ICMP message and checksum unchanged with the socket's hop limit. `IPPROTO_RAW` gives a descriptor for the interface ioctls alone, which BusyBox's DHCP client opens. Both kinds are root's, and each has a queue of 8 entries of up to 1500 bytes from the heap, which drops the newest when full.

### Netlink and netconsole

`socket/netlink.c` answers the requests of [netlink(7)](../../../man/man7/netlink.7) from the interface records, routing table, ARP cache and socket lists, in datagrams of at most 4096 bytes, and makes changes as the ioctls do.

`netconsole.c` is given every log record from the first, before any card is up, and keeps the characters in an 8192-byte ring, 8191 of them usable; a full ring drops the newest and counts it. After `netconsole_enable`, a newline or 512 waiting characters starts a drain: frames of up to 512 characters behind a 16-byte header, broadcast with EtherType `0x88b5` through the first interface, laid out as [`netconsole.h`](netconsole.h) describes. While that card's link is down, or when it refuses a frame, the characters wait for the next newline. Netconsole works below IP, so it needs no address and continues while the interface is down. A flag keeps a record raised during a send from starting another.

### Counters and concurrency

Each interface has seven 32-bit counters, which wrap, kept by `ethernet.c`: frames and bytes each way, receive errors, receive drops and transmit drops. The RTL8168's `counters` adds the card's tallies. `net_mib` (`net.h`) has the counters of RFC 1213 for IP, ICMP, TCP and UDP, and the ARP cache and routing table count their lookups and misses. [proc(5)](../../../man/man5/proc.5) gives the files.

Every entry to the kernel takes the big lock ([`../proc/smp.h`](../proc/smp.h)), so the stack runs on one processor at a time. Two contexts meet in it: interrupt handlers, the cards' and the tick, which run with interrupts off and do not nest, and system calls, which run with interrupts on. The shared state, each layer's transmit buffer, the driver rings, the ARP cache, the counters and the lists, has no lock of its own: a system call brackets every use of it with `net_lock` and `net_unlock`, which save the interrupt flag, disable interrupts and nest, as each driver's transmit does. The loopback ring is drained only with interrupts off, after the sender has left its bracket.

## Limits

* IPv4 only; [socket(2)](../../../man/man2/socket.2) refuses `AF_INET6`.
* No datagram is fragmented or reassembled. The MTU is 1500 bytes on every card: 1480 bytes of IP payload, 1472 of UDP, 1460 of TCP data. There are no jumbo frames.
* No IP options are sent, no datagram is forwarded between interfaces, and no multicast group can be joined.
* UDP checksums are neither sent nor checked.
* TCP sends no timestamps and reads no SACK blocks.
* ARP entries do not expire, any host on the link can replace a learned one, and there is no proxy ARP.
* The ICMP error limit is the machine's, shared by all peers, and one ping runs at a time.
* One RTL8139, up to four e1000 and one RTL8168GU, four cards in all. The RTL8139 driver does not wait for a transmit to complete, since QEMU completes it as it starts. The RTL8139 and e1000 need a legacy interrupt line from 0 to 15 and have no polled fallback. On i686 an e1000 or RTL8168 whose memory window is above 4 GiB does not start. No driver has offloads, VLAN tags, Wake-on-LAN, MSI-X or runtime power management.
* Netlink sends no notifications, and netconsole uses the first interface only, unacknowledged.

## Tests

`tests/run host` runs the host tests `tests/host/NAME_hosttest.c` of `arp`, `packet`, `tcp`, `tcp_reset`, `rcvwnd`, `socket_wake`, `raw`, `netif`, `netconf`, `netdev`, `netconsole`, `rtl8139`, `e1000` and `rtl8168`, which compile the stack's files natively under the sanitizers and drive them with crafted frames, segments and calls, the last two against a software model of the chip ([`tests/host/Readme.md`](../../../tests/host/Readme.md)). The `ip`, `network`, `netif`, `netconfig`, `rtl8139`, `tcp` and `tcp_reset` mutation scripts, `tests/host/NAME_mutate.sh`, require their rows to fail when a fault is compiled in.

The suites boot QEMU on the RTL8139 unless they say otherwise; [`tests/system/suites.md`](../../../tests/system/suites.md) says what each proves, and [`tests/Readme.md`](../../../tests/Readme.md) how to run one.

* Core tier: `default`, whose `loopprobe` runs UDP and TCP over 127.0.0.1, with `sockettest` and the musl probes `sockflagmusl`, `epollmusl`, `netlinkmusl`, `callsmusl` and `nsmusl`; `socket`, whose `udptest` and `tcptest` reach an echo peer on the host; and `dgram`, `netconsole`, `e1000`, `ssh` and `pools`.
* Full tier: `addressed`, `ping`, `netif`, `packet`, `raw` and `dhcp`, and the e1000 twins `ping-e1000`, `packet-e1000`, `netif-e1000`, `raw-e1000`, `dhcp-e1000`, `dgram-e1000`, `netconsole-e1000` and `ssh-e1000`.
