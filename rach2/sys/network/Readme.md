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

The `ping` sub-call waits by sleeping, not by spinning, and that is a rule about who else gets to run: the timer skips `schedule()` while a process is inside a syscall, so a poll with `sti; hlt` from there gives every other process no CPU at all for as long as the wait lasts, which on a machine several people can log into is a freeze any of them can fire at a dead address. The waiter blocks on a wait queue that `icmp_rx` wakes, and the wait is bounded twice over: `wakeup_tick` is armed so the timer makes the process runnable, and the same deadline is in the wait predicate, because a woken sleeper whose condition is still false re-enqueues with its timer already disarmed and would then never wake. The deadline never lands on 0, which is `wakeup_tick`'s own "not sleeping" sentinel: one tick value in every wrap of the counter would otherwise arm nothing and hang the caller, and `sys_nanosleep` steps around the same tick. That case is argued rather than probed, since the counter wraps about every 497 days at 100 Hz and no test here can set it. The kernel debug shell keeps the spin and correctly so: it runs with the scheduler stopped and no process to block. There is one in-flight echo for the whole machine, claimed under `net_lock` and released on every exit path, which is the raw-socket non-goal made visible to users: with a real `SOCK_RAW` each caller would own a socket, and here a second caller is refused with `-EBUSY` rather than queued, since a queue needs a fairness rule to give a shell command a wait it cannot see. So the sub-call returns ticks, `-ETIMEDOUT`, `-EBUSY` or `-EINTR`, and a ping at a dead address holds the slot, and refuses everyone else, for about four seconds.

A stream socket that bound a port keeps it: `connect` uses the bound port when there is one and a fresh ephemeral only when there is not, so a client can pin its source port the way binding one is meant to. What is refused is the exact triple the connection table keys on, a local port already paired with this same peer address and port, since two such blocks would have one connection's segments delivered to the other; a bound port shared with a listener, or with a connection to a different peer, stays legal. Listener exclusivity is enforced in `listen` rather than in `bind`, because the two are separate syscalls with arbitrary user code between them: binding both sockets and only then listening on each walks past any check that lives in `bind` alone, and leaves two LISTEN blocks on one port for the exact-match and listener lookups to disagree about. The check `bind` still makes is a courtesy, so a server hears about the collision where it expects to. The scope is live listeners and not every block in the table, since an accepted connection and a TIME_WAIT remnant hold a port against nobody and `setsockopt` is a no-op here, so counting them would make restarting a server after a session fail where it succeeds today. `listen` and `connect` report `-EADDRINUSE`, `-EDESTADDRREQ` and `-ENOBUFS` rather than a bare -1, which musl's return convention decodes as `EPERM`, and `connect` passes them up unchanged: the refusal a closed port earns still comes from the handshake.

Simultaneous open is implemented, which loopback makes reachable: two sockets each bound to the port the other dials will each meet a SYN that acknowledges nothing. The answer is a SYN-ACK emitted at `iss`, not at `snd_nxt`, and the rewind is the whole of it. The first SYN left `snd_nxt` at `iss + 1` while the peer's answering SYN-ACK acknowledges `iss + 1`, so a SYN-ACK sent from `iss + 1` is acknowledged by nothing, the retransmit sequence names something the peer will never confirm, and the connection wedges. Before this, a bare SYN in SYN_SENT was dropped and both ends hung until their retransmissions gave up. An active open reaches SYN_RCVD through that arm, so the block records which kind of open made it: a passive child is handed to a listener's `accept` queue and freed on a reset, since nothing else holds it, while an active opener is woken as its own completed `connect` and never unref'd, since a descriptor holds it already. Only the first half of that is gated by a probe. The loopback ring is drained by the timer while the caller is still inside its own `connect`, so the crossing usually finishes before the wait predicate is first evaluated and the wake is never needed here; over a real interface it is, and that is where the distinction earns its keep.

Sockets are first-class file descriptors (`FD_TYPE_SOCKET`). `read`, `write`, `close`, `dup2`, `fork`, and process exit all handle them through the FD layer, with a reference count so a socket shared by `fork`/`dup2` is torn down only when its last descriptor closes. The Linux i386 `socketcall` (syscall 102) multiplexes socket, bind, connect, listen, accept, send, recv, sendto, recvfrom, plus a `ping` sub-call; every user pointer argument is range-checked before use. A datagram socket that sends before an explicit `bind` is assigned an ephemeral source port automatically, so replies can return and `close` releases the port. A datagram read may return 0, and that is a datagram of no bytes rather than end of input: UDP carries an empty payload, so the receive slot's occupancy is a flag and not its length. A slot that read its own emptiness as "nothing here" built such a datagram, sent it, accepted it, delivered it to the right socket and dropped it at the last step, leaving a blocked `recvfrom` blocked. The same 0 reaches `read` on a socket descriptor, where a stream would have meant end of input; nothing in the tree confuses the two, since the resolver requires a header's worth of bytes and `nc` reads only streams.

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
