# network/: Ethernet and IPv4

This directory implements the kernel network stack. It supports one Ethernet interface, IPv4, ARP, ICMP echo, UDP, TCP, packet sockets, raw ICMP sockets, and a broadcast netconsole. User programs reach it through the Linux i386 `socketcall` ABI and the interface ioctls.

## Device selection

`init_network` allocates the TCP and socket pools before a device can receive a frame. `netdev_probe` then tries the RTL8139, the e1000, and the RTL8168GU, in that order, and binds the first device that starts.

The order has a reason at each position. The RTL8139 is first because it is what every existing suite boots against. The e1000 is second because it is what QEMU attaches when nobody asks for a card. The RTL8168GU is last because it is the one physical target, and a failure from it is the only diagnostic a machine with no serial port and no network has.

A driver that finds its device but cannot start it stops the probe and supplies the boot diagnostic. It is also silenced on the way past: a bring-up can fail after it has enabled bus mastering, and an unbound driver is one nothing else will ever reach.

Every driver publishes a `link`, which reports whether the link is up and at what speed and duplex, and a `stop`, which turns the receiver and transmitter off, masks the interrupts, and clears PCI bus mastering, in that order. The last of those is what ends DMA whatever the device believes it is doing. `kernel_shutdown` calls it after the page cache is flushed and after the goodbye line, so a machine read over netconsole sees that line and nothing `power_stop` prints after it. `stop` must be safe on a device that never finished starting and safe to call twice.

The storage drivers have no equivalent and are not quiesced at shutdown.

`link` returns 1 with its answer filled or 0 when the card cannot report a link at all, and the two are not the same: a card saying the link is down is different from a card that cannot say. Each driver's boot detail line reports what it read.

The RTL8139 driver is for QEMU's PCI device `10ec:8139`. It uses BAR0 I/O ports, an 8 KiB receive ring with a contiguous overhang, four round-robin transmit buffers, and IRQ-driven receive. It accepts frames promiscuously, pads short transmit frames to 60 bytes, drops received frames larger than 1518 bytes, and drops transmit frames larger than 1536 bytes. Its transmit path assumes QEMU completes a descriptor before the four-slot rotation reuses it. It does not wait for ownership as a real RTL8139 driver would.

The e1000 driver is for PCI device `8086:100e`, the Intel 82540EM, which is what QEMU's `e1000` device is and what QEMU attaches when nothing asks for a card. It maps BAR0, reads its station address from `RAL0` and `RAH0` when the valid bit is set and from EEPROM words 0 through 2 otherwise, and uses 32-entry receive and transmit descriptor rings with 2048-byte receive buffers. `RCTL.SECRC` is set, so a descriptor's length is the frame's length. Receive is interrupt-driven and takes only whole single-descriptor frames whose error byte is clear and whose length lies between 14 bytes and the buffer size; anything else is dropped and counted. Transmit waits at most ten 100 microsecond intervals for a full ring, then drops the frame and counts a stall. Short frames are padded to 60 bytes in software as well as by `TCTL.PSP`. The port omits jumbo frames, every offload, MSI and MSI-X, multiple queues, VLAN handling, interrupt throttling, and runtime power management. The related `e1000e` device is the 82574L and is refused by device id.

The RTL8168 driver accepts PCI device `10ec:8168` only when the `TxConfig` revision matches the RTL8168GU family. It uses MMIO and separate 64-entry receive and transmit descriptor rings. Receive buffers are 16,383 bytes, the programmed receive maximum is 16,384, and the largest transmit frame is 1514 bytes. Device interrupts remain masked; the 100 Hz timer polls receive and transmit completion. A full transmit ring waits at most ten 100 microsecond intervals before dropping the frame and counting a stall. Receive accepts only complete single-descriptor frames and removes the four-byte frame check sequence.

The RTL8168GU initialization sequence is derived from Linux v6.16 commit `038d61fd642278bab63ee8ef722c50d10ab01e8f`. The port omits MSI, MSI-X, device interrupts, jumbo frames, offloads, RSS, VLAN support, Wake-on-LAN, runtime power management, and the upstream firmware patch. The host model checks register access, bounded waits, initialization order, and descriptor ownership. It cannot check PCIe ordering, DMA on the physical device, PHY behavior, or whether the Lenovo board needs the omitted firmware.

## Interface configuration

The kernel has one interface, `eth0`, with index 1 and MTU 1500. With no address options it uses the QEMU SLIRP configuration `10.0.2.15/24` through `10.0.2.2`.

The boot line accepts:

```text
rach2.ip=A.B.C.D/prefix rach2.gateway=A.B.C.D
rach2.ip=none
```

An explicit address without `rach2.gateway` has no default route. `rach2.ip=none` leaves IPv4 unconfigured for userland. A gateway beside `none` is invalid. The parser rejects duplicate options, leading zeroes, malformed or out-of-range octets, trailing text, prefixes outside 1 through 30, non-host addresses, and a gateway outside the configured subnet. A refusal preserves the complete SLIRP default tuple.

`SIOCGIFFLAGS` reports `IFF_RUNNING` when the bound card says its link is up. A card that cannot report a link is treated as running while a device is bound, because for such a card that is the most that can honestly be said.

Programs configure the interface through `SIOCGIF*`, `SIOCSIF*`, `SIOCADDRT`, and `SIOCDELRT` on a socket descriptor. Getters are available to every user; setters and route changes require effective uid 0. The supported state consists of one address, one mask, the derived broadcast address, interface flags, and at most one default gateway. While an address is waiting for a separate netmask ioctl, the kernel assigns its classful mask. Adding the already implied subnet route succeeds without changing state. Removing it and adding any other non-default route are unsupported.

Changing the address flushes ARP, aborts connections opened from another local address with `ENETRESET`, and removes a gateway that is no longer on the subnet. Listening sockets remain active and use the new address. Setting the address to zero also clears the mask. Taking the interface down drops IP and ARP in both directions. It does not disable netconsole.

## Ethernet, ARP, and IPv4

Ethernet accepts IPv4 and ARP. Packet sockets see a matching frame before the kernel protocol handler. The same frame still reaches that handler.

This layer keeps seven 32-bit counters, which wrap, and `/proc/net/dev` reads them: frames and bytes each way, receive errors, receive drops, and transmit drops. A frame is counted as transmitted only after the driver reports that it took it. A driver reports what it refused through `ethernet_note_rx_error` and `ethernet_note_rx_drop` and through its transmit verdict; it does not keep the interface's counters itself. The other nine columns of that file are zero because nothing here reports them.

The ARP cache has eight entries. It learns a usable sender from requests and replies, including unsolicited packets. It rejects zero, loopback, multicast, reserved, broadcast, and subnet-network addresses. A lookup refreshes an entry's use order. When full, the cache replaces the entry unused for the longest time. Entries do not expire by age. On a transmit miss, the stack sends an ARP request, drops the current datagram, and reports success to the datagram sender. A later send can use the learned mapping.

IPv4 validates the version, header length, total length, and header checksum. Receive skips options according to IHL but drops every fragment. It accepts the local address, the limited broadcast, and the subnet broadcast. Broadcast datagrams reach UDP only. A wire packet whose source is the local address or `127/8` is dropped. Accepted datagrams are copied to matching raw sockets before ICMP, UDP, or TCP handles them.

Transmit creates a 20-byte header, sets DF, and uses the caller's hop limit. The stack does not fragment. An absent route returns `ENETUNREACH`. UDP broadcast uses the all-ones hardware address and never performs ARP.

There is no `lo` interface. At the socket boundary, a `127/8` destination becomes the current local address. Self-addressed datagrams enter an eight-slot ring and are delivered from the next timer tick, which avoids re-entering TCP in the sender's call frame. A full loopback ring returns `ENOBUFS`. Address-reporting calls return the canonical local address rather than `127.0.0.1`.

## ICMP and UDP

ICMP implements echo request and reply. The private ping socketcall permits one machine-wide request at a time. It makes four attempts with a one-second limit for each, which lets the first request populate ARP. A process sleeps on a wait queue; the kernel debug shell polls because no process scheduler is running there. The result is the round-trip time in ticks, `ETIMEDOUT`, `EBUSY`, or `EINTR`.

UDP has an eight-entry binding table and assigns ephemeral ports from 49152 through 65535. A nonzero port below 1024 requires effective uid 0. Two sockets may share a port only when both use `SO_REUSEADDR` and have the same effective uid. A matching connected socket receives before an unconnected socket on the same port.

Outgoing UDP checksums are zero, which disables the checksum under IPv4. Incoming UDP checksums are not verified. The maximum user payload is 1472 bytes. An ordinary UDP socket has one 1500-byte receive slot. A newly delivered datagram replaces an unread datagram in that slot. Empty datagrams are represented separately from an empty slot and therefore return a valid zero-byte read. `connect` records a peer without a handshake, filters receive to that peer, and may be undone with `AF_UNSPEC`.

## TCP

TCP uses a pool of 16 transmission control blocks. When the pool is full, allocation may recycle the closing block whose `TIME_WAIT` or `FIN_WAIT_2` deadline expires first. Send and receive rings are 4096 bytes each; MSS is 1460 bytes. The initial congestion window is four MSS and the initial slow-start threshold is 65,535 bytes.

The implementation includes passive, active, and simultaneous open; in-order delivery; retransmission; slow start and congestion avoidance; RTT measurement with Karn's rule; zero-window probes; and an update when a reader reopens enough receive space. The update threshold is the smaller of one MSS and half the receive ring. TCP does not negotiate window scaling, so advertised windows remain 16-bit.

The initial and minimum retransmission timeout is one second and the maximum is 60 seconds. The stack sends at most seven retransmissions; the next timeout ends the connection. Retransmission starts from the oldest unacknowledged data. `TIME_WAIT` lasts two seconds and `FIN_WAIT_2` has a ten-second backstop.

Only one live listener may hold a port. The `listen` backlog argument is ignored. The fixed circular accept array has 16 entries but leaves one unused to distinguish full from empty, so it queues at most 15 completed connections. Closing a socket flushes queued data before FIN where the state permits it.

## Socket interface

All network families share a pool of 16 socket objects. Sockets are `FD_TYPE_SOCKET` descriptors and retain one object across `dup`, `dup2`, and `fork` until the last reference closes.

Linux i386 syscall 102 implements these subcalls: `socket`, `bind`, `connect`, `listen`, `accept`, `getsockname`, `getpeername`, `send`, `recv`, `sendto`, `recvfrom`, `shutdown`, `setsockopt`, `getsockopt`, and `recvmsg`. Private subcall `0x1000` performs ICMP echo. `sendmsg` and `socketpair` are not implemented. Pointer ranges are checked before the protocol layer receives them. `O_NONBLOCK` and `MSG_DONTWAIT` apply to the supported blocking operations.

The supported families and types are:

* `AF_INET/SOCK_STREAM` for TCP.
* `AF_INET/SOCK_DGRAM` for UDP.
* `AF_INET/SOCK_RAW/IPPROTO_ICMP` for root-only raw ICMP.
* `AF_INET/SOCK_RAW/IPPROTO_RAW` for the root-only, ioctl-only descriptor used by network configuration programs. Data operations on it return `EOPNOTSUPP`.
* `AF_PACKET/SOCK_DGRAM` for root-only IPv4 and ARP packet sockets.

`recvmsg` supports datagram sockets, including packet sockets, and supplies no ancillary data. `PACKET_AUXDATA` and `IP_HDRINCL` return `ENOPROTOOPT`. `shutdown` applies only to a connected TCP socket. `SHUT_RD` is a no-op; a direction that includes write starts the normal TCP close.

The effective options are `SO_BROADCAST`, `SO_REUSEADDR`, `SO_BINDTODEVICE` for `eth0`, `IP_TTL`, `SO_ERROR`, and `SO_TYPE`. `SO_KEEPALIVE` and `TCP_NODELAY` report off and have no effect. Most other unknown options are accepted and ignored. `SO_ERROR` returns and clears the pending TCP error.

## Packet and raw sockets

A packet socket binds to interface index 0 or 1 and to IPv4 or ARP. At most two may be bound. Each has an eight-entry queue of payloads up to 1500 bytes. A full queue drops the newest frame. Receive omits the Ethernet header and reports the sender, protocol, interface, hardware type, and packet type in a 20-byte `sockaddr_ll`. Send accepts a nonempty payload of at most 1500 bytes plus a destination MAC address and supplies the Ethernet header.

At most four raw ICMP sockets may be registered. Each has an eight-entry queue of whole IPv4 datagrams up to 1500 bytes. Every matching socket receives a copy; a full queue drops the newest datagram. A raw send preserves the caller's ICMP bytes and checksum, adds the IPv4 header, and applies the socket's `IP_TTL`. The syscall accepts an ICMP message as large as 1500 bytes, but Ethernet later drops a message from 1481 through 1500 bytes because the added IPv4 header exceeds its MTU; the syscall still reports the original length.

## Netconsole

Netconsole is the last destination of the kernel log (see [the kernel Readme](../../Readme.md)), so it is offered every record from the first one, before there is a device to send through. Records collect until `netconsole_enable`, which `rach2.netconsole` asks for once a network device is up, and it then drains them. The ring is 8192 bytes with 8191 usable slots; new characters are dropped when it is full. After enable, a newline or 512 buffered bytes starts a transmission. The debug shell's own output is not a record and never reaches the wire.

Each broadcast Ethernet frame uses EtherType `0x88b5`. Its payload starts with a 16-byte big-endian header containing `RCH2`, a nonzero per-boot id, a sequence number, the payload length, and a one's-complement checksum. Frames have no acknowledgement or retry. Fixed buffers, the network interrupt lock, and a recursion guard permit calls from interrupt and error paths.

## Concurrency

Protocol transmit buffers, driver descriptors, ARP state, and the socket and TCP pools are shared. IRQ handlers already run with interrupts disabled. Syscall paths use `net_lock` to save the interrupt flag and disable interrupts across shared-state operations. All three transmit paths also protect their descriptor handoff. The timer runs `netdev_poll`, `tcp_tick`, and `ip_loop_drain` in that order at 100 Hz.

## Verification

`make test` runs 63 guest probes and includes `loopprobe`, which covers UDP and TCP against the local address without using the wire. Live-network probes are excluded from that shared run.

The specialized network targets are:

* `tests/run addressed` runs the default suite with the SLIRP address supplied on the boot line.
* `tests/run ping` checks replies, timeout, serialization, process progress, and slot release.
* `tests/run netif` checks interface queries, runtime reconfiguration, interface-down behavior, connection reset, and listener survival.
* `tests/run dgram` checks connected UDP, broadcast permission, shared binds, service-port permission, and socket options.
* `tests/run packet` checks packet-socket validation and live IPv4 and ARP exchange.
* `tests/run raw` checks raw ICMP fan-out, queue limits, routing errors, hop limits, caller checksums, and `traceroute`.
* `tests/run dhcp` checks the BusyBox and in-house DHCP clients, renewal, fixed configuration, routing, and unattended startup.
* `tests/run netconsole` validates captured frame headers, sequence numbers, checksums, and early buffered output.

`tests/run host` drives ARP replacement, address parsing and transitions, netconsole buffering and recursion, receive-window updates, and the RTL8168GU software model. `pingtest`, `udptest`, `tcptest`, `sockettest`, and `dnstest` are staged but excluded from the default runner. All except `sockettest` require a live peer or DNS service.
