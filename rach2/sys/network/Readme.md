# Rach2 Network Stack

A from-scratch TCP/IP stack for the Rach2 kernel, targeting QEMU's RTL8139 NIC on the SLIRP user network. It brings up, bottom to top: an RTL8139 driver, Ethernet, ARP, IPv4, ICMP, UDP, TCP, and a BSD-style socket layer exposed to user space through a Linux `socketcall` syscall.

## Layers

| Directory          | Responsibility                                                              |
|--------------------|-----------------------------------------------------------------------------|
| `driver/rtl8139.*` | PCI NIC driver: RX ring, four TX descriptors, IRQ-driven receive            |
| `ethernet/`        | Frame parse and build; dispatch by EtherType to ARP or IP                   |
| `arp/`             | IP-to-MAC resolution with an 8-entry cache                                  |
| `ip/ip.*`          | IPv4 header build/parse, checksum, routing, fragment drop                   |
| `ip/icmp.*`        | Echo (ping): replies to requests, sends our own with a timeout              |
| `udp/`             | Datagrams, a port-to-socket table, length validation                        |
| `tcp/`             | RFC 793 state machine with RFC 6298 RTT/RTO and RFC 5681 congestion control |
| `socket/`          | Socket pool, blocking recv/accept/connect, refcounted lifecycle             |
| `core/`            | `init_network`, byte-order helpers, shared configuration                    |

## Configuration

The stack is configured for QEMU SLIRP in `net.h`: guest `10.0.2.15/24`, gateway `10.0.2.2`. The MAC address is read from the NIC at init.

## Socket interface

Sockets are first-class file descriptors (`FD_TYPE_SOCKET`). `read`, `write`, `close`, `dup2`, `fork`, and process exit all handle them through the FD layer, with a reference count so a socket shared by `fork`/`dup2` is torn down only when its last descriptor closes. The Linux i386 `socketcall` (syscall 102) multiplexes socket, bind, connect, listen, accept, send, recv, sendto, recvfrom, plus a `ping` sub-call; every user pointer argument is range-checked before use. A datagram socket that sends before an explicit `bind` is assigned an ephemeral source port automatically, so replies can return and `close` releases the port.

## Name resolution

The kernel resolves no names; it provides only the transport. Host names are turned into addresses in userspace by a small stub resolver (`utils/resolv.c`, shared through `utils/netdb.h`) that `ping` and `nc` call: it tries a numeric literal, then the `/hosts` file, then a DNS `A` query over UDP to the nameserver in `/resolv.conf` (SLIRP's forwarder `10.0.2.3`). Both config files live in the flat KRFFS root. This mirrors a libc stub resolver and leaves room to add `getaddrinfo` over the same files.

## Concurrency

The shared TX assembly buffers, the TCB and socket pools, and the ARP cache are unlocked, so every access to them must run with interrupts disabled. IRQ-context senders (the NIC RX handler and `tcp_tick`) already do, since IRQ handlers clear IF and do not nest. Syscalls, however, run with interrupts enabled (so the timer and audio keep working during long syscalls), so a syscall-context sender can be re-entered by an IRQ mid-build. Syscall-context code therefore brackets each access to this shared state with `net_lock()`/`net_unlock()` (save IF and disable interrupts), a harmless no-op when called from an IRQ. The full rule is documented in `net.h`; the driver's TX handoff guards itself as well. TCP timers run from the 100 Hz timer handler (`tcp_tick`).

## Testing

Userspace programs exercise the stack:

* `tests/pingtest`, `tests/udptest`, `tests/tcptest`, `tests/sockettest`, `tests/dnstest`
* `utils/ping`, `utils/nc`, `utils/httpd`

The UDP and TCP tests expect a host echo server at `10.0.2.2:7777`. Boot with a NIC and (for inbound servers like `httpd`) a host forward:

```sh
qemu-system-i386 -kernel ../build/rach2/kernel -initrd ../build/images/os-simple.krffs -serial stdio \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 -device rtl8139,netdev=net0
```

The kernel debug shell also provides `netping` (ping the gateway) and `netbad` (ping an unreachable host to show the timeout).
