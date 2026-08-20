# Milestone 27: the diagnostics

## The goal

Milestone 25 sealed the disk. Before it goes into the laptop, the machine gets the tools a person reaches for when something is wrong.

A boot of the sealed image on the Lenovo showed that `ps`, `top`, `route`, `netstat`, `arp` and `df` are all compiled in and all fail, most of them on a single missing file under `/proc`. This is the milestone that writes those files, and it does so without inventing any number the kernel does not hold.

Milestone 26 swept the documentation immediately before this one, so the Readmes describe the tree as it stands when this work begins. Each phase here rewrites the Readme owning any contract it changes, in its own commit, as the project requires; what it does not do is a second sweep.

Nothing here changes the disk layout, the boot menu, or the two levels of disk authority. The sealed image is rebuilt, rehearsed and physically rewritten at the end, so the drive that goes into the machine carries this milestone.

## What was measured before this plan

The sealed disk was written to the 250 GB SSD, booted in QEMU off the physical drive, and booted on the Lenovo G50-70 over a USB adapter on a real network.

DHCP works end to end on real hardware. The netconsole capture holds the whole exchange: discover, select for 192.168.0.107 from server 192.168.0.1, lease obtained. The router's reservation matched the MAC the driver reports, `28:D2:44:7E:E0:AF`, which is what `dev-log/24-metal/baseline.md` recorded. The lease was applied, not merely received: address, mask, default route, and `/etc/resolv.conf` written from the lease. `nslookup` reaches 1.1.1.1 and answers correctly.

That router hands out an infinite lease, 4294967295 seconds. The in-house client is not exposed to it: `HARD_MAX_LEASE` (`utils/dhcpc.c:89`) is seven days and `dhcp_sane_lease` clamps before `dhcp_boundaries` multiplies by seven, on both lease paths.

Six applets fail, and each names its missing file: `ps` on `/proc/uptime`, `top` on `/proc/stat`, `route -n` on `/proc/net/route`, `netstat` on `/proc/net/tcp`, `arp` on `/proc/net/arp`, `df` on `/proc/mounts`. `/proc/net/dev` exists and is correct.

`TERM` is not broken. With a client that sends a terminal type, a dropbear session reports `TERM=xterm`. `stty` does fail on a pty, which is phase E.

## What two rounds of review corrected in this plan

An adversarial review against the code overturned six of this plan's claims. Each correction is carried into the phase it belongs to; they are listed here because a reader of the finished milestone should know which of these were mistakes rather than discoveries.

`rach2.nodisk` already exists. It was built in milestone 24 as one of four device-skip tokens, implemented at `rach2/sys/block/block.c:100`, documented at `rach2/Readme.md:61`, and exercised by `make subset-test` (`Makefile:1018`). An earlier draft proposed building it. What is actually wrong with it is different, and phase H addresses that.

That target boots six lines and not sixteen: none, each token alone, and all four together. `rach2/Readme.md:61` calls it every combination, which milestone 26 corrects rather than this one widening the target to match the prose.

The test runners do not score what an earlier draft said they score. `tools/runtests.py` scores from the dedicated init-return and signal-return markers and explicitly not from a bare `Process exited with status N` (`tests/Readme.md:123`). `tools/harness.py` reads the generic line, but only in its no-token mode. The discriminator phase A needs already exists as `debug_shell_is_home` (`rach2/kernel.c:96`).

Retiring `utils/ping.c` costs more than it saves. It is the two-translation-unit specimen the compiler proof compiles (`tools/tcc_test.sh:161`, with `:249` naming it as the one in-house program with a companion source), the unprivileged smoke test in the self-build gate (`tools/selfbuild_test.sh:208`), and the basis of three probes. busybox `ping` needs a raw socket and a raw socket is root's, so removing the in-house one also removes unprivileged ping. It stays.

Widening the packet socket's protocol is not enough for `arping`. busybox creates the socket with protocol zero, binds with the ARP protocol, and calls `getsockname` expecting a `sockaddr_ll` carrying a hardware address. The kernel accepts only the IPv4 protocol at creation (`rach2/sys/syscall.c:8170`), ignores the bound protocol, and returns a `sockaddr_in`. Phase F carries the whole list.

The timer does not know whether it interrupted an idle processor. There is no idle task: `schedule()` returns with the blocked process still current when nothing is ready (`rach2/sys/proc/sched.c:73`), and the wait is `sti; hlt; cli` inside the blocking syscall (`rach2/sys/syscall.c:5570`). Phase C therefore builds the idle marker rather than assuming one.

A TCP control block outlives the socket that made it. `tcb->sock` is cleared on detach (`rach2/sys/network/tcp/tcp.c:909`) while the row stays in the pool through the closing states, and a passive connection's socket is allocated in receive context (`tcp.c:1159`), where there is no calling process to take a UID from. Phase D stores identity so it survives both.

## What the sockets are today, exactly

`do_socket` (`rach2/sys/syscall.c:8142`) accepts two families. `AF_PACKET` with `SOCK_DGRAM` and the IPv4 protocol is a working packet socket, root only, added in milestone 25 because DHCP replies arrive before the machine owns an address. `AF_INET` with `SOCK_RAW` and `IPPROTO_RAW` is a stub that carries interface ioctls and refuses to send or receive. ICMP echo is a private socketcall, `SC_PING` at `0x1000`, one request in flight, used by `utils/ping.c`.

So: packet sockets yes, raw IP sockets no. `vendor/busybox/Readme.md:82` said both were absent; milestone 26 corrects it to that state, and phases F and G move it again as they change what is true.

`do_setsockopt` (`rach2/sys/syscall.c:8938`) keeps two flags and returns 0 for every other option, by a decision its comment records. `IP_TTL` is therefore accepted and ignored today, and `ip_tx` sets TTL 64 unconditionally. That matters to traceroute and is named in phase G.

## Before phase A: the arrival check

This milestone runs on a different machine from the one that wrote it, so its first act is proving that machine can do the work. From a fresh clone on the Mac mini: `make doctor`, `make full`, `make verify-style`, `make verify-host`, the full test list, `make selfbuild-test`, and `make sealed-test`. Milestone 26 could not run this, because a clone on the machine that already has everything proves very little.

One precondition comes with it: the router reservation that gives the laptop its address must still exist, since phase F's DHCP test asserts the address it hands out.

The credential dependency an earlier draft carried is gone, and the reason is worth recording because it changed the shape of both milestones. The artifact now on the SSD was built with the owner's own public key alongside the project's demo key, so the Mac mini reaches the laptop with the key already in its `~/.ssh` and needs none of the project's private material to run phases F and G. Copying the five generated files is now a convenience, not a precondition, and the alternative of moving phase K's rewrite ahead of the hardware tests is no longer needed.

Three things arrive by hand rather than by clone, and each is checked here rather than assumed, because every target above can pass while all three are missing. The laptop's presented SSH host key is compared against the fingerprint milestone 26 recorded, so the first connection authenticates the machine instead of trusting whatever answers. `ports/doom/fetch_wad.sh` is run, because the WAD is staged as an optional file (`krffs/manifests/rach2-metal-disk.manifest:163`) and its absence is accepted silently, leaving a disk whose Doom cannot start. And write access to the remote is proved with `git push --dry-run`, since a machine that can clone cannot necessarily publish, and phase K's record has to reach the remote.

Done when every one of those targets passes on the Mac mini from a clean clone, the laptop answers SSH with the owner's own key, already present in `~/.ssh` on that machine, and its host key matches the recorded fingerprint, the WAD is present, and the remote accepts a dry-run push.

## Phase A: a quiet exit on a normal boot, first

This goes first because it changes the serial channel through which every later phase is judged.

`sys_exit` prints "Process exited with status N" to the screen and the serial port on every process exit (`rach2/sys/syscall.c:454-455`). On a machine running init and a login shell it is noise, and ash answers the question through `$?`.

The gate is `debug_shell_is_home` (`rach2/kernel.c:96`), false on a boot that runs init and assigned true at `kernel.c:3143`, which both debug-shell paths reach: an init-less image, and the rescue selection made just above it. The change wraps the two generic prints and nothing else, so it neither writes nor moves the flag and cannot disturb its other readers (`kernel.c:271`, `kernel.c:1250`, `syscall.c:515`, `:4648`, `:4678`, `:7533`). The two dedicated markers stay unconditional: the init-return line (`syscall.c:521`) and the signal-death line (`kernel.c:267`), because those are what `runtests.py` scores.

Proving it needs real boots, not the parser self-test, which feeds canned strings and never boots a kernel. Four: a normal init boot with a child that exits, asserting no generic line appears; a debug-shell probe with a forked child, asserting the generic lines remain; a signal-killed probe, asserting the dedicated marker; and one `harness.py` run in its no-token mode, asserting the generic line it depends on is still there.

While here, correct `tools/runtests.py`'s own top-level prose, which still describes the older scheme.

Done when those four boots pass, `make test` passes unchanged, and `tests/Readme.md`'s scoring description is still true.

## Phase B: the /proc snapshot mechanism

Every synthetic `/proc` file is built into a fixed `char data[512]` at open (`rach2/sys/fs/procfs.c:38`) through appenders that stop silently when full (`procfs.c:49`). Sixteen TCP rows or eight ARP rows do not fit, and a truncated table reads as a short one with no error anywhere.

The design is a snapshot with a proved worst-case bound, not streaming generation. Streaming would be feasible against the existing `file_ops` and the shared offset (`rach2/sys/fs/file.h:23`, `procfs.c:457`), but regenerating a live table on each read splices different states of it into one open file, and every pool here is a fixed size, so a bound can be proved instead. Each file declares its maximum, the appenders fail loudly rather than stopping, and the snapshot is taken once at open as it is today.

Done when a table larger than 512 bytes reads back whole, an appender that would overflow its declared bound fails the build or the test rather than truncating, and a test that opens a file, changes the underlying table, and then reads on takes the state captured at open. Reading twice without changing anything proves nothing here: a regenerating implementation passes that too.

## Phase C: the process and system files under /proc

Add `/proc/uptime`, `/proc/stat`, `/proc/meminfo` and `/proc/mounts`, plus the `statfs64` syscall that `df` needs.

What exists: `kernel_uptime_seconds` and the within-second counter (`rach2/kernel.c:80`, `:81`, advanced by the timer handler at `:148`), and `frame_total_count` and `frame_free_count` (`rach2/sys/mm/paging.h:165`, `:168`), which is `/proc/meminfo` outright.

Idle time has to be built, and the shape of it is the phase's real work. There is no idle task, and a blocked process stays current while the processor halts, so testing `current_proc` or its state would classify idle ticks as busy. An explicit marker is set around the wait and sampled at timer entry before any wakeup changes state, and the counters record delivered ticks rather than claiming wall-clock time, since interrupts are off in several paths and a tick can be lost. Multiword counters are read atomically.

Every halt site has to carry the marker, and there are more than one: `rach2/sys/syscall.c:5570`, `:6774`, `:7183`, `:7551`, `rach2/sys/proc/wait_queue.c:134` and `rach2/sys/io/poll.c:203`. One marked-wait helper that all of them call is the answer, with a style check that rejects a bare `sti; hlt` outside it, because a site added later and left unmarked would silently report its idle time as busy.

The test cannot be that busy plus idle equals the delivered ticks, because a marker that is never set satisfies that with idle at zero. It asserts three things over two intervals: during a controlled halt, idle increases and busy does not; during a tight user loop, busy increases and idle does not; and across both, the two deltas sum to the ticks the timer delivered.

A boot epoch is stored, because `rtc_boot_time` (`rach2/sys/rtc.c:469`) is the moment the clock was read rather than a kept origin, and `/proc/stat` needs a stable `btime`.

`/proc/mounts` needs a real source, and there is not one today. The three mount branches are transient local state (`rach2/kernel.c:3002-3018`) whose `via` string is discarded, and what survives is one bit, disk or not disk, in `_krffs_on_disk` (`kernel.c:1514`). So the mount records what it mounted and with which options at the moment it succeeds, and the file reports that. A fixed line would call a RAM root a disk one on two boots out of three.

`df` needs `statfs64`, which returns `-ENOSYS` today. KRFFS knows its own size and free space, so the numbers are real.

A stored boot epoch is needed for `/proc/stat`'s `btime`, because `rtc_boot_time` (`rach2/sys/rtc.c:469`) is the moment the clock was read rather than a kept origin, and `btime` that moved between reads would be worse than absent.

`top` is deferred and said to be. It reads several CPU categories from `/proc/stat` and `/proc/loadavg`, and load averages need run-queue sampling this milestone is not adding. A `top` printing static numbers would be worse than one that does not run.

Done when `ps`, `df`, and (with their config options enabled) `free` and `uptime` run and print values that match what the kernel independently reports; the three-interval idle test passes; `/proc/mounts` names the right source and options on a RAM-root boot, a window boot and a disk boot, which are three separate boots; `btime` is unchanged across repeated reads within one boot; and a probe reads each file and checks its shape.

## Phase D: the network files under /proc

Add `/proc/net/route`, `/proc/net/arp` and `/proc/net/tcp`, following `/proc/net/dev`.

The route table derives from `netconf` (`rach2/sys/network/netconf.h:7`): one on-link subnet and at most one default gateway. The ARP table is eight entries and the TCP table is the tcb pool, and both are private with no iterator today, so each gets a lock-protected snapshot function rather than a reader reaching into its internals.

Identity is the subtle part. busybox netstat parses a UID and an inode from `/proc/net/tcp`, and a socket carries neither; printing zero would claim every socket belongs to root. The inode is allocated on the socket, the owner UID is taken in syscall context, a passive connection inherits the listening socket's UID because its socket is allocated in receive context where there is no caller, and both values are copied into the control block so a row in a closing state still reports them after `tcb->sock` is cleared. The same inode appears in `/proc/net/tcp`, in `/proc/<pid>/fd/N`, and in `fstat` of the socket, replacing the constant `socket:[1]` rendered today (`rach2/sys/fs/procfs.c:248`).

`/proc/net/arp` is a textual table, not the hexadecimal one `/proc/net/route` uses. Each file follows its own real format, verified against busybox's parser rather than against a guess.

`netstat` also opens `/proc/net/tcp6`. There is no IPv6 here. The file stays absent, netstat says so for that family alone after printing the IPv4 table, and the Readme records that as the intended answer.

Done when `route -n`, `arp` and `netstat -tn` print the machine's real state; the address netstat shows for a live SSH session matches what the peer sees; the inode for one socket is the same value in `/proc/net/tcp`, in `/proc/<pid>/fd/N` and in `fstat`, which the constant rendered today would fail; the owner of an accepted connection is the listener's UID, proved with a listener whose owner differs from whatever process is current when the SYN arrives, since otherwise taking the current process's UID would pass by accident; a row in a closing state still reports its identity after its socket is detached; and a test fills each table to capacity with no truncation.

## Phase E: stty on a pty

`stty size` on standard input and `stty -F /dev/pts/0 size` both fail on a dropbear session, printing the device name with no reason after it, and `stty -a` reports "Not a tty" where `tty` correctly answers `/dev/pts/0`.

The dispatch looks right: `sys_ioctl` calls `fd_tty(entry)` and `tty_common_ioctl` before the generic ops dispatch, precisely so a pty slave's `TCGETS` reaches the shared handler (`rach2/sys/syscall.c:7796-7810`, the handler itself at `:7671`). The `TIOCGWINSZ` at `rach2/sys/tty/pty.c:254` is the master's, inside `ptm_ioctl`, and is not the path a slave takes; which handler a slave actually reaches is part of what the diagnosis establishes. A message with an empty reason suggests an errno musl has no string for, which would mean a return value outside the errno range rather than an unhandled request. That is a hypothesis, and the phase diagnoses before it fixes.

A probe opens a pty pair, issues `TCGETS`, `TIOCGWINSZ` and `TCSETS` on the slave, and prints the raw return values, which decides between that hypothesis, `fd_tty` failing for the descriptor dropbear hands the child, and `tty_common_ioctl` not claiming a request it appears to handle.

Resolve in the same phase why `/proc/self/fd` showed descriptor 0 as `/dev/pts/0` and descriptors 1 and 2 as `pipe:[1]` in that session. It may be dropbear behaving normally or it may be the same fault.

Done when `stty size` and `stty -a` work over an SSH session, `resize` reports the window, `/proc/self/fd` in that session names the expected object for descriptors 0, 1 and 2 with the answer written down whichever way it turns out, and a regression probe fails if the ioctl path returns to what it does today.

## Phase F: the packet socket ABI, completed

The packet socket was built for one caller and stops where that caller stopped. Finishing it is what `arping` needs, and it is independent of raw ICMP, so it comes first.

Five pieces: accept creation with protocol zero; store the bound protocol and filter deliveries by it; deliver ARP frames to a matching packet socket alongside the kernel's own `arp_rx` rather than instead of it (`rach2/sys/network/ethernet/ethernet.c:60`); send with the bound protocol's EtherType instead of the hardcoded IPv4 one (`rach2/sys/network/socket/packet.c:197`); and return a real `sockaddr_ll` from `getsockname` with interface index, hardware type, address length and MAC.

The existing DHCP path must be unaffected, which is the regression this phase is most likely to cause. `make dhcp-test` and a real lease on the laptop are both required.

Done when `arping` answers on the test network; a probe proves a socket bound to ARP sees ARP frames and one bound to IP does not; one injected ARP frame both reaches the socket and leaves an address-to-MAC entry visible in `/proc/net/arp`, which phase D provides; a reply on the wire may be asserted as well but cannot stand in for the cache entry, since insertion can fail on a full table while `arp_rx` still replies (`rach2/sys/network/arp/arp.c:64`, `:126`), and a delivery that diverted ARP away from `arp_rx` would pass a socket-only test while quietly breaking address resolution; `getsockname` returns a `sockaddr_ll` with the real MAC; and DHCP still takes a lease on hardware.

## Phase G: raw ICMP sockets, and IP_TTL

Add `AF_INET` with `SOCK_RAW` and `IPPROTO_ICMP` that genuinely sends and receives, root only, so busybox `ping` and `traceroute` work. This is the largest phase and it is placed after everything cheap.

The receive side delivers a full IPv4 datagram, header included, because that is what busybox parses, to every raw socket whose protocol matches, without consuming the kernel's own ICMP handling. Time-exceeded and unreachable messages reach raw sockets too, or traceroute learns nothing. Proving fan-out needs more than two working pings: an implementation that demultiplexed by echo identifier would pass that while delivering each datagram to exactly one socket. So the test injects one known datagram and asserts that two raw sockets receive byte-identical copies of it while the kernel's own ICMP path also processes it, and repeats for a time-exceeded message.

The send side takes an ICMP message whose checksum the caller computed and does not rewrite it, and supplies the IPv4 header itself. `ip_tx` returns void today and silently drops an oversized datagram (`rach2/sys/network/ip/ip.c:195`), returns without transmitting when there is no route (`ip.c:260`), and on an ARP miss sends a request and returns (`ip.c:264`). It gains a return value, and the four outcomes are named here rather than left to the implementation:

    IP_HDRINCL set          -ENOPROTOOPT, because it is not implemented
    datagram over the MTU   -EMSGSIZE
    no route to the address -ENETUNREACH
    ARP not yet resolved    success, and the datagram is dropped

The last one is deliberate and is the only one that is not an error. A datagram send does not promise delivery, the ARP request has gone, and the next packet goes out resolved; `ping` retries by design. It is distinguished from no route, which is a permanent condition the caller must hear about.

`IP_HDRINCL` is rejected rather than silently accepted, which is what `do_setsockopt` would do with it today.

`IP_TTL` becomes a real per-socket value for UDP and raw sends. Without it traceroute sets a hop limit that is silently ignored, every probe leaves with TTL 64, and a one-hop target answers, which would let this phase pass with traceroute's entire mechanism absent. The test captures transmitted frames and asserts the TTL field changes per probe; multi-hop behaviour is confirmed once on the laptop against a real route.

Budget the sockets, in the numbers the packet socket already uses rather than a second convention: eight datagrams of one MTU per raw socket, the arriving datagram dropped when the queue is full so a client that is behind keeps what it already has, a dropped counter, and one diagnostic the first time it happens (`rach2/sys/network/socket/packet.c:18-21`, `:116-122`).

The pool those sockets come from holds sixteen and is shared with TCP, accepted connections included (`rach2/sys/network/socket/socket.h:26`). Exhaustion is already `-ENOMEM` (`rach2/sys/syscall.c:8185`) and stays that. A raw socket must not be able to starve the machine of the sockets SSH needs, which is what the test below is for.

`utils/ping.c` and `SC_PING` stay. Raw sockets are root's; the in-house ping is what an ordinary user runs, what the self-build gate smokes as the `test` account, and the two-file specimen the compiler proof builds. Both share `ip_rx`, `icmp_rx` and `ip_tx`, so this is not a second stack; the hazard is receive ordering, which the fan-out test above closes.

Done when all of these hold, each as its own test:

* busybox `ping` and `traceroute` work as root on the test network and on the laptop, and the in-house `ping` still works as an ordinary user.
* An ordinary user opening a raw socket is refused.
* The fan-out test passes for an echo reply and again for a time-exceeded message, and once more for an unreachable.
* A capture shows the TTL changing per traceroute probe, on a raw send and on a UDP send.
* `IP_HDRINCL`, an oversized datagram and an address with no route each return the errno named above, and an unresolved neighbour returns success with the ARP request on the wire.
* A capture shows the caller's ICMP checksum unchanged.
* Nine datagrams are delivered to a raw socket nobody is reading: the first eight are readable in order, the ninth is gone, and the dropped counter says one.
* Every remaining slot in the pool is taken, not merely most of them, and the next socket creation returns `-ENOMEM`; an SSH session opened beforehand still carries data through it and afterwards.
* `make selfbuild-test` and `tools/tcc_test.sh` pass untouched.

## Phase H: a boot line that contradicts itself

`rach2.nodisk` exists and is a diagnostic, not a boundary. The plan says so plainly, because an editable token cannot be an authority boundary: milestone 25 settled that (`dev-log/25-sealed/plan.md:17`, `rach2/sys/block/recovery.h:14`), and the argument does not reverse because this token removes access. Deleting it from a boot line that expects it grants access.

The defect is what it does on an image whose root is on the disk. Discovery runs before the root is mounted (`rach2/kernel.c:2996`), so with the probe skipped nothing mounts, and `boot_init_process` prints "falling back to kernel debug shell" (`kernel.c:1331`) and hands the machine to an unauthenticated shell, which the kernel elsewhere refuses to do on purpose (`kernel.c:1296`).

The prohibited combination is the skip token, and a disk root by either spelling (`rach2.fsdisk` or `rach2.root=disk`, `kernel.c:3008`), and no explicit rescue request. Rescue is the exemption because it is the documented way to reach the debug shell deliberately (`kernel.c:1298`), by either spelling (`rach2.rescue` or `init=rescue`, `kernel.c:3137`), and a person skipping a suspected disk probe on the way there is doing exactly what the tokens exist for. Everything else halts with a diagnostic naming both tokens.

The three sibling tokens are audited for the same shape of fault.

Done when both disk-root spellings without rescue halt and say why, both rescue spellings with the skip token and a disk root still reach the shell, a boot test asserts each of those four and never accepts the debug shell as success for the prohibited case, `make subset-test` still passes, and `rach2/Readme.md` says the tokens are diagnostics and not boundaries.

## Phase I: the mapping ceiling, and saying so when it is hit

Found while debugging an SSH login and measured on the machine: a process can make about 3.1 MB of small allocations before `malloc` starts failing, while a single 64 MB allocation succeeds. The cause is `VMA_MAX` at 128 (`rach2/sys/mm/vm.h:37`) against musl's allocator, which carves many mappings for small blocks. Raising it to 512 lifted the ceiling past 8 MB with no other change.

Both halves are worth fixing. The limit itself is low for a machine that compiles its own kernel, and 128 was chosen in milestone 22 for a link that needed more than 64, not from any measurement of the small-allocation case. And the failure is silent: `vm.c:82` and `vm.c:134` return without a word, so a process that ran out of mappings looks like a process whose library failed for its own reasons. That silence cost hours of misdiagnosis in the session that found it.

Raising the number costs kernel memory that is already accounted for in the comment there: 16 bytes an entry across the 16-process table, so 512 entries is 128 KB of `.bss` against 32 KB today.

The diagnostic belongs where the kernel knows which process is asking, which is the `sys_mmap2` path and the split-failure caller in `syscall.c`, not `vm.c`, which is deliberately independent of kernel globals (`rach2/sys/mm/vm.h:16`).

Done when the same workload that measured 3.1 MB (1 KB blocks until `malloc` returns null) reaches at least 8 MiB after the change, exhaustion says so on the serial log naming the process, `tests/musl/vmamusl.c` asks for more mappings than the old 128 could grant while keeping its deliberate margin below the new maximum, and the comment in `vm.h` records the measurement rather than only which link ran out.

## Phase J: dmesg from the boot log

`dmesg` fails on `klogctl`, which is not implemented, and implementing the Linux logging call is the wrong shape for this kernel. `/dev/bootlog` already exists and carries the log the SSH session reads.

Give a person on the machine an ordinary command that prints it. If that means a small program or a script rather than busybox's `dmesg`, the Readme says so plainly instead of shipping a `dmesg` that is not one.

Done when the boot log is readable from a shell on the machine and the Readme names the command and the reason.

## Phase K: the gate, the record, and the disk

Run the whole gate from a clean tree: `make verify-style`, `make verify-host`, the full suite, `make selfbuild-test`, `make sealed-test`. Rebuild the sealed disk and write the record in `dev-log/27-polish/Readme.md`.

Then rewrite the physical drive, because a milestone that improves the machine and leaves the old artifact on the disk has not finished.

This runs on a different machine from the one that wrote this plan, with a different set of drives attached and quite possibly a different node for the same SSD. So the write begins by asking, as `CLAUDE.md` requires of any destructive hardware operation: list every physical disk the host can see, list what it reports about the candidate (node, size in bytes, bus, internal or external, whole disk, and the table it currently carries), and wait for an approval naming that node. The approval given on the old machine does not carry over, and neither does the node number. Nothing here may assume the drive is `/dev/disk6` or that the last approval still stands.

After the approval the write follows `dev-log/25-sealed/checklist.md` as corrected in milestone 26: compare the artifact's hash and size against the rehearsed one, write under the guarded block, read the whole device back, and compare hashes again.

Done when every gate passes from a clean tree, the record is written, the drive's readback hash equals the rehearsed artifact's with the target's identification recorded beside it, and the rewritten drive then boots and answers: a login prompt, an SSH session by key, and `bootctl status` reporting a committed candidate with no trial armed. A drive that hashes correctly and does not boot has not finished the milestone, which is why the checklist requires the boot as well.

## Risks, in expected order

Phase G can go long. A raw socket touches the receive path every protocol shares, and a mistake is not a failed applet but a stack that drops packets under load. It is the last of the network phases, and the three phases after it are small, so a milestone that stops early still delivers the diagnostics.

Phase A can break the suite in a way that looks like a passing run: a probe that stops being scored rather than one that fails. That is why its proof is four real boots.

Phases C and D invite invention. Linux `/proc` carries fields this kernel has no equivalent for. A field that is not real is either computed for the first time here or the applet needing it is deferred and said to be; it is not filled with a plausible number.

Phase F can break DHCP, which is the one capability the sealed machine cannot boot without.

Phase E may find nothing in an hour and then take a day, because it starts as a diagnosis.

## What this milestone deliberately leaves out

* IPv6. `/proc/net/tcp6` stays absent and netstat says so.
* `top`, and with it `/proc/loadavg` and per-category CPU accounting.
* `IP_HDRINCL`, unless phase G finds traceroute needs it.
* `klogctl`. The boot log is reached the way this system already reaches it.
* The `ip` suite. `ifup` uses the ifconfig backend.
* Any change to the disk layout, the boot menu, or the two authority levels.

## What must not be broken

* The probe scoring contract. Phase A changes when the generic line is printed, never what it says, and never the two dedicated markers.
* DHCP on real hardware. Phase F is the risk; a lease on the laptop is part of its done-when.
* The two levels of disk authority and their compile-time switches. Phase H adds no authority and states that its token is not a boundary.
* Unprivileged `ping`, the self-build smoke test that uses it, and the two-translation-unit compiler proof that compiles it.
* The guest build's equality with the host lists, held by `tools/check_guest.py` and proved by `make selfbuild-test`.
* The sealed artifact's geometry and its 660602880 bytes. Phase K rebuilds it; it does not redesign it.
* C89 in the kernel, inside the dialect `i686-elf-gcc` and the staged TinyCC share.
* ASCII only, and the voice rule, in every line of prose this milestone touches.
