# Rach2 Network Stack

A from-scratch TCP/IP stack for the Rach2 kernel, targeting QEMU's RTL8139 NIC on the SLIRP user network. It brings up, bottom to top: an RTL8139 driver, Ethernet, ARP, IPv4, ICMP, UDP, TCP, and a BSD-style socket layer exposed to user space through a Linux `socketcall` syscall.

## Layers

| Directory          | Responsibility                                                              |
|--------------------|-----------------------------------------------------------------------------|
| `driver/rtl8139.*` | PCI NIC driver: RX ring, four TX descriptors, IRQ-driven receive            |
| `ethernet/`        | Frame parse and build; dispatch by EtherType to ARP or IP                   |
| `arp/`             | IP-to-MAC resolution with an 8-entry cache                                  |
| `ip/ip.*`          | IPv4 header build/parse, checksum, routing, fragment drop, loopback ring    |
| `ip/icmp.*`        | Echo (ping): replies to requests, sends our own and sleeps for the reply    |
| `udp/`             | Datagrams, a port-to-socket table, length validation                        |
| `tcp/`             | RFC 793 state machine with RFC 6298 RTT/RTO and RFC 5681 congestion control |
| `socket/`          | Socket pool, blocking recv/accept/connect, refcounted lifecycle             |
| `core/`            | `init_network`, byte-order helpers, shared configuration                    |

## Configuration

The stack is configured for QEMU SLIRP in `net.h`: guest `10.0.2.15/24`, gateway `10.0.2.2`. The MAC address is read from the NIC at init.

## Loopback

The stack knows exactly one local address, and `127/8` is an alias for it: `connect`, `sendto`, and the `ping` sub-call canonicalize a `127/8` destination to `10.0.2.15` at the socket boundary, before any connection state exists, so no lower layer ever carries a second local address and `getsockname`/`getpeername` report the canonical one. A datagram addressed to ourselves then never touches ARP or the wire: `ip_tx` copies it onto a small loopback ring, and the 100 Hz timer delivers the queued datagrams to `ip_rx` (`ip_loop_drain`), under the same interrupts-off discipline as NIC delivery; delivering inline would reenter TCP under the sender's own call frame. A full ring drops the datagram, exactly as a full NIC ring would. There is no loopback interface proper (no routes, no `lo` device): the alias is the whole feature, and `loopprobe` proves it end to end. Because a genuine loopback datagram carries our own address as its source, `ip_rx` takes a `from_loopback` flag and drops a wire packet whose source is our own address or `127/8`: such a martian is a spoof no honest peer sends, and without the check it could steer a reply onto the loopback ring or out the wire carrying a loopback source.

## Socket interface

The `ping` sub-call waits by sleeping, not by spinning, and that is a rule about who else gets to run: the timer skips `schedule()` while a process is inside a syscall, so a poll with `sti; hlt` from there gives every other process no CPU at all for as long as the wait lasts, which on a machine several people can log into is a freeze any of them can fire at a dead address. The waiter blocks on a wait queue that `icmp_rx` wakes, and the wait is bounded twice over: `wakeup_tick` is armed so the timer makes the process runnable, and the same deadline is in the wait predicate, because a woken sleeper whose condition is still false re-enqueues with its timer already disarmed and would then never wake. The kernel debug shell keeps the spin and correctly so: it runs with the scheduler stopped and no process to block. There is one in-flight echo for the whole machine, claimed under `net_lock` and released on every exit path, which is the raw-socket non-goal made visible to users: with a real `SOCK_RAW` each caller would own a socket, and here a second caller is refused with `-EBUSY` rather than queued, since a queue needs a fairness rule to give a shell command a wait it cannot see. So the sub-call returns ticks, `-ETIMEDOUT`, `-EBUSY` or `-EINTR`, and a ping at a dead address holds the slot, and refuses everyone else, for about four seconds.

Sockets are first-class file descriptors (`FD_TYPE_SOCKET`). `read`, `write`, `close`, `dup2`, `fork`, and process exit all handle them through the FD layer, with a reference count so a socket shared by `fork`/`dup2` is torn down only when its last descriptor closes. The Linux i386 `socketcall` (syscall 102) multiplexes socket, bind, connect, listen, accept, send, recv, sendto, recvfrom, plus a `ping` sub-call; every user pointer argument is range-checked before use. A datagram socket that sends before an explicit `bind` is assigned an ephemeral source port automatically, so replies can return and `close` releases the port.

## Name resolution

The kernel resolves no names; it provides only the transport. Host names are turned into addresses in userspace by a small stub resolver (`utils/resolv.c`, shared through `utils/netdb.h`) that `ping` and `nc` call: it tries a numeric literal, then the hosts file, then a DNS `A` query over UDP to the nameserver in the resolver config (SLIRP's forwarder `10.0.2.3`). Each file is looked for under `/etc` first and in the flat KRFFS root second, since os-real stages a real `/etc` and the test image stages neither directory. This mirrors a libc stub resolver and leaves room to add `getaddrinfo` over the same files.

## Concurrency

The shared TX assembly buffers, the TCB and socket pools, and the ARP cache are unlocked, so every access to them must run with interrupts disabled. IRQ-context senders (the NIC RX handler and `tcp_tick`) already do, since IRQ handlers clear IF and do not nest. Syscalls, however, run with interrupts enabled (so the timer and audio keep working during long syscalls), so a syscall-context sender can be re-entered by an IRQ mid-build. Syscall-context code therefore brackets each access to this shared state with `net_lock()`/`net_unlock()` (save IF and disable interrupts), a harmless no-op when called from an IRQ. The full rule is documented in `net.h`; the driver's TX handoff guards itself as well. TCP timers run from the 100 Hz timer handler (`tcp_tick`).

## Testing

Userspace programs exercise the stack:

* The freestanding probes `pingtest`, `udptest`, `tcptest`, `sockettest`, and `dnstest` are staged into `test.krffs`. `make test` boots it headless and runs them, or boot it by hand and `exec` each from the kernel debug shell. `loopprobe` (in the `make test` pass) needs no host at all: it exercises the UDP and TCP paths against the machine itself over `127.0.0.1`.
* The hand-written utilities `ping`, `nc`, and `httpd` are staged into `os-simple.krffs`. Run them from the ISH shell. (os-real carries busybox, whose own `ping`/`nc` applets exercise the same stack.)

The UDP and TCP tests expect a host echo server at `10.0.2.2:7777`. Boot the image that carries the program you want with a NIC, and (for an inbound server like `httpd`) a host forward. The probes need `test.krffs`, the utilities `os-simple.krffs`:

```sh
qemu-system-i386 -cpu max -kernel ../build/rach2/kernel -initrd ../build/images/test.krffs -serial stdio \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 -device rtl8139,netdev=net0
```

The kernel debug shell also provides `netping` (ping the gateway) and `netbad` (ping an unreachable host to show the timeout).
