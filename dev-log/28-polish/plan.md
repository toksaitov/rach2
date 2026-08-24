# Milestone 28: the diagnostics

## The goal

Milestone 25 sealed the disk. Before installing it in the laptop, this milestone adds the diagnostic tools needed when the machine fails.

A sealed-image boot on the Lenovo showed that `ps`, `top`, `route`, `netstat`, `arp` and `df` were present but failed. Most needed one missing file under `/proc`. This milestone adds those files without reporting values that the kernel does not have.

Milestone 26 reviewed the documentation immediately before this work, so the Readmes describe the starting tree. Each phase updates the Readme that owns a changed contract in the same commit. This milestone does not repeat the complete documentation review.

The disk layout, boot menu, and two levels of disk authority remain unchanged. The final phase rebuilds and rehearses the sealed image, then writes it to the physical drive for the laptop.

## What was measured before this plan

The sealed disk was written to the 250 GB SSD, booted in QEMU off the physical drive, and booted on the Lenovo G50-70 over a USB adapter on a real network.

DHCP works end to end on real hardware. The netconsole capture records discovery, selection of 192.168.0.107 from server 192.168.0.1, and lease acquisition. The router's reservation matched the MAC reported by the driver, `28:D2:44:7E:E0:AF`, and by `dev-log/24-metal/baseline.md`. The client applied the address, mask, default route, and `/etc/resolv.conf` from the lease. `nslookup` reaches 1.1.1.1 and returns the correct answer.

That router hands out an infinite lease, 4294967295 seconds. The in-house client is not exposed to it: `HARD_MAX_LEASE` (`utils/dhcpc.c:89`) is seven days and `dhcp_sane_lease` clamps before `dhcp_boundaries` multiplies by seven, on both lease paths.

Six applets fail because a required file is missing: `ps` needs `/proc/uptime`, `top` needs `/proc/stat`, `route -n` needs `/proc/net/route`, `netstat` needs `/proc/net/tcp`, `arp` needs `/proc/net/arp`, and `df` needs `/proc/mounts`. `/proc/net/dev` exists and is correct.

`TERM` is not broken. With a client that sends a terminal type, a Dropbear session reports `TERM=xterm`. `stty` does fail on a pty, which is phase E.

## What two rounds of review corrected in this plan

Two reviews against the code found six incorrect claims in the draft plan. The relevant phase includes each correction. This section distinguishes those draft errors from facts discovered during implementation.

`rach2.nodisk` already exists. Milestone 24 added it as one of four device-skip tokens. It is implemented at `rach2/sys/block/block.c:100`, documented at `rach2/Readme.md:61`, and exercised by `make subset-test` (`Makefile:1018`). The draft proposed adding it. Phase H instead addresses a defect in its existing behavior.

That target boots six command lines: none of the tokens, each token alone, and all four together. It does not boot all sixteen combinations. Milestone 26 corrects the claim at `rach2/Readme.md:61`; this milestone does not widen the target to match it.

The draft also misstated probe scoring. `tools/runtests.py` uses the dedicated init-return and signal-return markers, not a bare `Process exited with status N` (`tests/Readme.md:123`). `tools/harness.py` reads the generic line only in its no-token mode. Phase A can distinguish the two cases with the existing `debug_shell_is_home` flag (`rach2/kernel.c:96`).

The draft proposed removing `utils/ping.c`, but three contracts require it. The compiler proof uses it as the two-translation-unit specimen (`tools/tcc_test.sh:161`, with `:249` identifying it as the one in-house program with a companion source). The self-build verification runs it as the unprivileged smoke test (`tools/selfbuild_test.sh:208`), and three probes depend on it. BusyBox `ping` requires a raw socket available only to root. The in-house implementation therefore remains as the unprivileged ping.

Widening the packet socket's protocol alone does not support `arping`. BusyBox creates a socket with protocol zero, binds it to the ARP protocol, and calls `getsockname` for a `sockaddr_ll` containing a hardware address. The kernel accepts only the IPv4 protocol at creation (`rach2/sys/syscall.c:8170`), ignores the bound protocol, and returns a `sockaddr_in`. Phase F addresses all three requirements.

The timer cannot determine directly whether it interrupted an idle processor. There is no idle task. When no process is ready, `schedule()` returns with the blocked process still current (`rach2/sys/proc/sched.c:73`), and the blocking syscall waits with `sti; hlt; cli` (`rach2/sys/syscall.c:5570`). Phase C therefore adds an explicit idle marker.

A TCP control block can outlive its socket. Detach clears `tcb->sock` (`rach2/sys/network/tcp/tcp.c:909`), but the row remains in the pool through the closing states. A passive connection's socket is allocated in receive context (`tcp.c:1159`), where no calling process supplies a UID. Phase D stores the identity where both cases retain it.

## What the sockets are today, exactly

`do_socket` (`rach2/sys/syscall.c:8142`) accepts two families. `AF_PACKET` with `SOCK_DGRAM` and the IPv4 protocol provides a root-only packet socket. Milestone 25 added it because DHCP replies arrive before the machine has an address. `AF_INET` with `SOCK_RAW` and `IPPROTO_RAW` provides only interface ioctls and refuses to send or receive. ICMP echo uses the private `SC_PING` socketcall at `0x1000`, permits one request in flight, and serves `utils/ping.c`.

The starting tree therefore supports packet sockets but not raw IP sockets. `vendor/busybox/Readme.md:82` said both were absent. Milestone 26 corrects that description of the starting tree, and phases F and G update it again after changing the implementation.

`do_setsockopt` (`rach2/sys/syscall.c:8938`) records two flags and returns 0 for every other option, as its comment specifies. It therefore accepts and ignores `IP_TTL`, while `ip_tx` always sets TTL 64. Phase G must change this behavior for traceroute.

## Before phase A: the arrival check

This milestone runs on a different machine from the one where it was planned. A fresh clone on the Mac mini must pass `make doctor`, `make full`, `make verify-style`, `make verify-host`, the full test list, `make selfbuild-test`, and `make sealed-test`. Milestone 26 could not perform this check because a clone on the existing development machine would not test a new-machine setup.

The router reservation for the laptop must still exist because phase F's DHCP test checks the assigned address.

An earlier draft incorrectly required the project's generated credentials. The artifact on the SSD contains the owner's public key alongside the project's demo key. The Mac mini already has the corresponding key in `~/.ssh`, so phases F and G need none of the project's private material. Copying the five generated files is optional, and phase K no longer needs to precede the hardware tests. This correction changed both milestone plans.

Three prerequisites arrive outside the clone and require explicit checks. The laptop's SSH host key must match the fingerprint recorded by milestone 26 before the first connection trusts it. Run `ports/doom/fetch_wad.sh` because the WAD is optional in `krffs/manifests/rach2-metal-disk.manifest:163`; a build can otherwise succeed with no playable Doom data. Run `git push --dry-run` to prove write access to the remote because clone access does not prove that phase K can publish its record.

This check is complete when every target passes from a clean clone on the Mac mini, the laptop accepts the owner's key already present in that machine's `~/.ssh`, the laptop's host key matches the recorded fingerprint, the WAD is present, and the remote accepts a dry-run push.

## Phase A: a quiet exit on a normal boot, first

This phase comes first because every later phase uses the affected serial output.

`sys_exit` prints "Process exited with status N" to the screen and serial port for every process exit (`rach2/sys/syscall.c:454-455`). A machine running init and a login shell does not need this line because ash reports the status through `$?`.

Use `debug_shell_is_home` (`rach2/kernel.c:96`) to select the output. It is false when a boot runs init and becomes true at `kernel.c:3143`, which both debug-shell paths reach: an image without init and the rescue selection immediately above it. Wrap only the two generic prints. Do not write or move the flag or change its other readers (`kernel.c:271`, `kernel.c:1250`, `syscall.c:515`, `:4648`, `:4678`, `:7533`). Keep the init-return marker (`syscall.c:521`) and signal-death marker (`kernel.c:267`) unconditional because `runtests.py` scores them.

The parser self-test uses canned strings and cannot prove the boot behavior. Use four real boots: a normal init boot whose child exits and produces no generic line; a debug-shell probe whose forked child still produces the generic lines; a signal-killed probe that produces the dedicated marker; and a no-token `harness.py` run that still receives the generic line it requires.

While here, correct `tools/runtests.py`'s own top-level prose, which still describes the older scheme.

This phase is complete when all four boots pass, `make test` remains unchanged, and the scoring description in `tests/Readme.md` remains correct.

## Phase B: the /proc snapshot mechanism

At open time, every synthetic `/proc` file is built in a fixed `char data[512]` buffer (`rach2/sys/fs/procfs.c:38`). Its appenders silently stop when full (`procfs.c:49`). Sixteen TCP rows or eight ARP rows exceed the buffer, and the resulting table appears valid but incomplete.

Keep the existing open-time snapshot and give it a proved worst-case bound. The existing `file_ops` and shared offset could support streaming (`rach2/sys/fs/file.h:23`, `procfs.c:457`), but regenerating a live table on each read could combine different states in one open file. Every relevant pool has a fixed size, so each file can declare a maximum. An appender that exceeds it must fail instead of truncating the snapshot.

This phase is complete when a table larger than 512 bytes reads back in full, an appender that exceeds its declared bound fails the build or test, and a test proves snapshot timing by opening a file before changing the underlying table. Two reads of unchanged state are insufficient because regeneration would produce the same result.

## Phase C: the process and system files under /proc

Add `/proc/uptime`, `/proc/stat`, `/proc/meminfo`, and `/proc/mounts`, together with the `statfs64` syscall required by `df`.

The existing counters provide most of the data. `kernel_uptime_seconds` and the within-second counter are at `rach2/kernel.c:80` and `:81`; the timer handler advances them at `:148`. `frame_total_count` and `frame_free_count` at `rach2/sys/mm/paging.h:165` and `:168` directly provide `/proc/meminfo`.

Idle time requires new accounting. A blocked process remains current while the processor halts because there is no idle task. Testing `current_proc` or its state would therefore count idle ticks as busy. Set an explicit marker around each wait and sample it at timer entry before a wakeup can change state. Count delivered ticks rather than wall-clock time because paths that disable interrupts can lose a tick. Read multiword counters atomically.

Every halt site needs the marker: `rach2/sys/syscall.c:5570`, `:6774`, `:7183`, `:7551`, `rach2/sys/proc/wait_queue.c:134`, and `rach2/sys/io/poll.c:203`. Route all of them through one marked-wait helper. Add a style check that rejects a bare `sti; hlt` elsewhere so a later unmarked wait cannot count idle time as busy.

A check that busy plus idle equals delivered ticks is insufficient because a marker that is never set passes with idle fixed at zero. Test three properties over two intervals. During a controlled halt, idle increases and busy does not. During a tight user loop, busy increases and idle does not. Across both intervals, the two deltas sum to the delivered timer ticks.

Store a boot epoch for the stable `btime` required by `/proc/stat`. `rtc_boot_time` (`rach2/sys/rtc.c:469`) gives the time of each clock read, not a retained origin.

`/proc/mounts` has no persistent source of mount details. The three mount branches keep only transient local state at `rach2/kernel.c:3002-3018` and discard their `via` strings. The surviving `_krffs_on_disk` bit at `kernel.c:1514` distinguishes only disk from non-disk roots. Record the source and options when a mount succeeds, then report those values. A fixed line would misreport a RAM root as a disk root on two of the three boot paths.

`df` needs `statfs64`, which currently returns `-ENOSYS`. KRFFS can supply its actual size and free space.

The stored boot epoch also keeps `/proc/stat`'s `btime` fixed. Since `rtc_boot_time` (`rach2/sys/rtc.c:469`) reports the moment of a clock read instead of a retained origin, using it directly would change `btime` between reads.

Defer `top` and document the omission. It reads several CPU categories from `/proc/stat` and `/proc/loadavg`; load averages require run-queue sampling outside this milestone. Do not make `top` run by supplying static values.

This phase is complete when `ps`, `df`, and, with their configuration options enabled, `free` and `uptime` report values that match independent kernel data. The three-interval idle test must pass. Three separate boots must show the correct `/proc/mounts` source and options for a RAM root, a window, and a disk. Repeated reads during one boot must leave `btime` unchanged, and a probe must validate the format of every file.

## Phase D: the network files under /proc

Add `/proc/net/route`, `/proc/net/arp`, and `/proc/net/tcp` alongside `/proc/net/dev`.

Derive the route table from `netconf` (`rach2/sys/network/netconf.h:7`), which has one on-link subnet and at most one default gateway. The private ARP table has eight entries, and the private TCP table is the tcb pool. Give each a lock-protected snapshot function instead of exposing its internals to the proc reader.

Socket identity requires additional state. BusyBox netstat parses a UID and inode from `/proc/net/tcp`, but the current socket has neither. Printing zero would incorrectly assign every socket to root. Allocate an inode on the socket and capture the owner UID in syscall context. A passive connection inherits the listening socket's UID because receive context has no calling process when it allocates the socket. Copy both values into the control block so a closing row retains them after `tcb->sock` is cleared. Use the same inode in `/proc/net/tcp`, `/proc/<pid>/fd/N`, and socket `fstat`, replacing the current constant `socket:[1]` (`rach2/sys/fs/procfs.c:248`).

`/proc/net/arp` uses a textual table, while `/proc/net/route` uses hexadecimal fields. Verify each format against the BusyBox parser that consumes it.

`netstat` also opens `/proc/net/tcp6`, but the system has no IPv6. Leave the file absent. Netstat can report that family as unavailable after printing the IPv4 table, and the Readme must describe this behavior.

This phase is complete when `route -n`, `arp`, and `netstat -tn` report the machine's current state. For a live SSH session, netstat's peer address must match the peer's observation. One socket must have the same inode in `/proc/net/tcp`, `/proc/<pid>/fd/N`, and `fstat`, which the current constant cannot satisfy. Test accepted-connection ownership with a listener whose UID differs from the process current when the SYN arrives; otherwise an incorrect current-process UID would pass. A closing row must retain its identity after socket detachment, and a capacity test must fill each table without truncation.

## Phase E: stty on a pty

In a Dropbear session, both `stty size` on standard input and `stty -F /dev/pts/0 size` fail after printing only the device name. `stty -a` reports "Not a tty", although `tty` correctly reports `/dev/pts/0`.

The initial dispatch inspection shows the expected order. `sys_ioctl` calls `fd_tty(entry)` and `tty_common_ioctl` before the generic operations dispatch so a pty slave's `TCGETS` can reach the shared handler (`rach2/sys/syscall.c:7796-7810`; the handler is at `:7671`). The `TIOCGWINSZ` handler at `rach2/sys/tty/pty.c:254` belongs to the master through `ptm_ioctl`, not to the slave path. The diagnosis must determine which handler the slave reaches. An empty diagnostic could mean that musl received an errno without a known string, perhaps because the kernel returned a value outside the errno range. Treat this only as a hypothesis until the probe decides it.

A probe opens a pty pair, issues `TCGETS`, `TIOCGWINSZ`, and `TCSETS` on the slave, and prints the raw return values. Those results distinguish the errno hypothesis from a failure of `fd_tty` on Dropbear's child descriptor or a request that `tty_common_ioctl` appears to support but does not claim.

The same phase must explain why `/proc/self/fd` showed descriptor 0 as `/dev/pts/0` but descriptors 1 and 2 as `pipe:[1]`. This may be normal Dropbear behavior or another symptom of the same defect.

This phase is complete when `stty size` and `stty -a` work through SSH, `resize` reports the window, and `/proc/self/fd` reports the expected objects for descriptors 0, 1, and 2. Record the result even if Dropbear's descriptor arrangement is correct. A regression probe must fail if the ioctl path returns to its starting behavior.

## Phase F: the packet socket ABI, completed

The packet socket implements only the operations needed by its original caller. Complete its ABI for `arping` before adding the independent raw ICMP support.

The implementation has five parts. Accept socket creation with protocol zero. Store the bound protocol and use it to filter deliveries. Deliver ARP frames to a matching packet socket as well as the kernel's own `arp_rx` (`rach2/sys/network/ethernet/ethernet.c:60`). Send with the bound protocol's EtherType instead of the fixed IPv4 value (`rach2/sys/network/socket/packet.c:197`). Return a complete `sockaddr_ll` from `getsockname`, including the interface index, hardware type, address length, and MAC.

This work must preserve the existing DHCP path. Require both `make dhcp-test` and a real lease on the laptop.

This phase is complete when `arping` answers on the test network and a probe shows that an ARP-bound socket receives ARP frames while an IP-bound socket does not. One injected ARP frame must reach the socket and create an address-to-MAC entry visible through `/proc/net/arp` from phase D. A wire reply is useful but cannot replace the cache assertion: insertion can fail when the table is full while `arp_rx` still replies (`rach2/sys/network/arp/arp.c:64`, `:126`), and a socket-only test would accept an implementation that diverts ARP away from `arp_rx`. `getsockname` must return a `sockaddr_ll` with the actual MAC, and DHCP must still acquire a lease on hardware.

## Phase G: raw ICMP sockets, and IP_TTL

Add root-only send and receive support for `AF_INET` with `SOCK_RAW` and `IPPROTO_ICMP`, allowing BusyBox `ping` and `traceroute` to work. This is the largest phase, so it follows the smaller changes.

The receive path delivers the complete IPv4 datagram, including its header, to every matching raw socket because BusyBox parses that format. Delivery must not consume the kernel's own ICMP handling. Raw sockets also receive time-exceeded and unreachable messages required by traceroute. Two successful pings do not prove fan-out because an implementation could demultiplex by echo identifier and send each datagram to only one socket. Instead, inject one known datagram and require byte-identical copies on two raw sockets while the kernel's ICMP path also processes it. Repeat the test with a time-exceeded message.

The send path accepts an ICMP message with a caller-computed checksum, preserves that checksum, and supplies the IPv4 header. The current `ip_tx` returns void. It silently drops an oversized datagram (`rach2/sys/network/ip/ip.c:195`), returns without transmission when no route exists (`ip.c:260`), and sends a request before returning on an ARP miss (`ip.c:264`). Give it a return value with these four specified outcomes:

    IP_HDRINCL set          -ENOPROTOOPT, because it is not implemented
    datagram over the MTU   -EMSGSIZE
    no route to the address -ENETUNREACH
    ARP not yet resolved    success, and the datagram is dropped

An unresolved neighbor is the only non-error outcome. Datagram send does not guarantee delivery, the ARP request has been sent, and a later packet can use the resolved entry; `ping` retries. No route is a persistent condition and must be returned to the caller.

Reject `IP_HDRINCL` instead of silently accepting it as the current `do_setsockopt` does.

Store `IP_TTL` per socket and apply it to UDP and raw sends. Without this change, traceroute's hop limit is ignored, every probe leaves with TTL 64, and a one-hop target can answer even though traceroute's mechanism is absent. Capture transmitted frames and require the TTL field to change with each probe. Confirm multi-hop behavior once against a real route from the laptop.

Use the packet socket's existing limits for raw sockets instead of defining another scheme: eight one-MTU datagrams per socket, drop-new behavior when the queue is full, a drop counter, and one diagnostic on the first drop (`rach2/sys/network/socket/packet.c:18-21`, `:116-122`). Dropping the arriving datagram preserves the queued data for a slow reader.

Raw and TCP sockets, including accepted connections, share a pool of sixteen (`rach2/sys/network/socket/socket.h:26`). Pool exhaustion already returns `-ENOMEM` (`rach2/sys/syscall.c:8185`) and must continue to do so. The test below verifies that raw-socket exhaustion does not break an existing SSH connection.

Keep `utils/ping.c` and `SC_PING`. Raw sockets are restricted to root, while the in-house ping serves ordinary users, runs under the `test` account during self-build verification, and provides the compiler proof's two-file specimen. Both implementations use `ip_rx`, `icmp_rx`, and `ip_tx`; the new work does not create another network stack. The fan-out test verifies the receive ordering that could otherwise distinguish them.

This phase is complete when a separate test proves each requirement:

* BusyBox `ping` and `traceroute` work as root on the test network and the laptop, while the in-house `ping` still works for an ordinary user.
* An ordinary user opening a raw socket is refused.
* The fan-out test passes separately for an echo reply, a time-exceeded message, and an unreachable message.
* A capture shows the TTL changing per traceroute probe, on a raw send and on a UDP send.
* `IP_HDRINCL`, an oversized datagram, and an address with no route each return the errno specified above. An unresolved neighbor returns success and puts the ARP request on the wire.
* A capture shows the caller's ICMP checksum unchanged.
* When nine datagrams reach an unread raw socket, the first eight remain readable in order, the ninth is dropped, and the drop counter reports one.
* After every remaining pool slot is occupied, the next socket creation returns `-ENOMEM`. An SSH session opened beforehand still transfers data during and after exhaustion.
* `make selfbuild-test` and `tools/tcc_test.sh` pass untouched.

## Phase H: a boot line that contradicts itself

`rach2.nodisk` is a diagnostic option rather than an authority boundary. Milestone 25 established that an editable token cannot control authority (`dev-log/25-sealed/plan.md:17`, `rach2/sys/block/recovery.h:14`). The rule also applies to a token that removes access because deleting it from an expected boot line would grant access.

The defect occurs when the image's root is on the skipped disk. Discovery precedes the root mount (`rach2/kernel.c:2996`). Skipping the probe leaves no mounted root, after which `boot_init_process` prints "falling back to kernel debug shell" (`kernel.c:1331`) and starts an unauthenticated shell. Another path deliberately refuses that outcome (`kernel.c:1296`).

Refuse a boot that combines the skip token with either disk-root spelling (`rach2.fsdisk` or `rach2.root=disk`, `kernel.c:3008`) and has no explicit rescue request. Exempt rescue because it deliberately requests the debug shell (`kernel.c:1298`) through either `rach2.rescue` or `init=rescue` (`kernel.c:3137`); skipping a suspect disk probe during rescue is a valid diagnostic use. Every prohibited case halts after a diagnostic reports both tokens.

Audit the other three skip tokens for equivalent failures.

This phase is complete when both disk-root spellings halt with an explanation in the absence of rescue, while both rescue spellings still reach the shell when combined with the skip token and a disk root. A boot test must assert all four cases and must never accept the debug shell as success for a prohibited case. `make subset-test` must still pass, and `rach2/Readme.md` must identify the tokens as diagnostics rather than boundaries.

## Phase I: the mapping ceiling, and saying so when it is hit

Measurements taken while debugging an SSH login showed that small `malloc` allocations fail after about 3.1 MB, although one 64 MB allocation succeeds. Musl creates many mappings for small blocks and reaches `VMA_MAX`, which is 128 at `rach2/sys/mm/vm.h:37`. Raising the limit to 512 allowed the same workload to pass 8 MB with no other change.

Increase the limit and report its exhaustion. The value 128 came from a milestone 22 link that needed more than 64, not from the measured small-allocation workload of a machine that compiles its own kernel. The failure is also silent: `vm.c:82` and `vm.c:134` return without a diagnostic, making mapping exhaustion appear to be a library failure. This ambiguity cost hours during the session that found it.

The existing comment accounts for the kernel-memory cost: 16 bytes per entry across the 16-process table. A 512-entry limit uses 128 KB of `.bss`, compared with the current 32 KB.

Report exhaustion in the `sys_mmap2` path and the split-failure caller in `syscall.c`, where the kernel knows the requesting process. Keep the diagnostic out of `vm.c`, which deliberately avoids kernel globals (`rach2/sys/mm/vm.h:16`).

This phase is complete when the workload that reached 3.1 MB, using 1 KB blocks until `malloc` returns null, reaches at least 8 MiB. Exhaustion must identify the process on the serial log. `tests/musl/vmamusl.c` must request more mappings than the former limit of 128 while retaining its deliberate margin below the new maximum. The comment in `vm.h` must record the measurement in addition to the link that exhausted the earlier limit.

## Phase J: dmesg from the boot log

`dmesg` fails because `klogctl` is not implemented. Do not add the Linux logging call for this one command; `/dev/bootlog` already provides the log read by an SSH session.

Provide an ordinary command that prints the boot log. If it uses a small program or script instead of BusyBox `dmesg`, document that implementation and its limits rather than presenting it as a complete `dmesg` implementation.

This phase is complete when a shell command reads the boot log and the Readme identifies both the command and the reason for its implementation.

## Phase K: the checks, the record, and the disk

From a clean tree, run `make verify-style`, `make verify-host`, the full suite, `make selfbuild-test`, and `make sealed-test`. Rebuild the sealed disk and write `dev-log/28-polish/Readme.md`.

Then update the physical drive so it contains the completed artifact.

The work runs on a different host, with different attached drives and possibly a different node for the same SSD. Before writing, follow `CLAUDE.md`: list every visible physical disk and report the candidate's node, size in bytes, bus, internal or external status, whole-disk status, and current partition table. Wait for approval that identifies that node. Neither the old host's approval nor its node number applies. Do not assume the drive is `/dev/disk6`.

After approval, follow `dev-log/25-sealed/checklist.md` as corrected in milestone 26. Compare the artifact's hash and size with the rehearsed copy, write it under the guarded block, read back the complete device, and compare hashes again.

This phase is complete when every check passes from a clean tree, the record exists, and the drive's readback hash matches the rehearsed artifact with the target identification recorded beside it. The rewritten drive must then boot to a login prompt, accept a keyed SSH session, and have `bootctl status` report a committed candidate with no trial armed. A matching hash alone does not prove that the drive boots, so the checklist requires both results.

## Risks, in expected order

Phase G has the greatest schedule risk. Raw sockets change the shared receive path, where a defect can drop packets under load rather than merely break one applet. It follows the other network phases, and the final three phases are small. If the milestone stops at this phase, the preceding diagnostic work still provides its intended results.

Phase A can make an unscored probe appear to pass. Its proof therefore requires four real boots.

Phases C and D must not invent Linux `/proc` fields without an equivalent in this kernel. Either compute a missing value from actual state or defer and document the applet that needs it. Do not supply a plausible constant.

Phase F can break DHCP, without which the sealed machine cannot boot.

Phase E begins with diagnosis. It may produce no finding in an hour and then require a day.

## What this milestone deliberately leaves out

* IPv6. `/proc/net/tcp6` stays absent and netstat says so.
* `top`, together with `/proc/loadavg` and per-category CPU accounting.
* `IP_HDRINCL`, unless phase G finds that traceroute requires it.
* `klogctl`. The command reads the boot log through the existing system interface.
* The `ip` suite. `ifup` continues to use the ifconfig backend.
* Any change to the disk layout, the boot menu, or the two authority levels.

## What must not be broken

* The probe scoring contract. Phase A changes only when the generic line is printed. Its text and the two dedicated markers remain unchanged.
* DHCP on real hardware. Phase F must obtain a lease on the laptop.
* The two levels of disk authority and their compile-time switches. Phase H adds no authority and documents that its token is not a boundary.
* Unprivileged `ping`, the self-build smoke test that runs it, and the two-translation-unit compiler proof that compiles it.
* Agreement between the guest and host build lists, checked by `tools/check_guest.py` and `make selfbuild-test`.
* The sealed artifact's geometry and 660602880-byte size. Phase K rebuilds the existing design.
* Kernel C89 within the dialect shared by `i686-elf-gcc` and the staged TinyCC.
* ASCII and the project voice in every prose line changed by this milestone.
