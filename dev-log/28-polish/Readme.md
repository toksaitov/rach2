# Milestone 28: the diagnostics

This milestone added information that a user can request from the machine about its own state. `plan.md` gives each phase's design and rationale. This record describes the implementation, review corrections, and hardware work that remains.

Each phase contained one scoped change, repository verification, a test proved by an intentional failure, an independent diff review whose findings were checked against the code, an update to the owning documentation in the same commit, and one house-style commit.

## What was built

### A: the quiet exit line (a38dedc)

`sys_exit` wrote `Process exited with status N` to the console for every exit, including a forked child, even when the machine ran a real init. The line now depends on `debug_shell_is_home`: an image with `/sbin/init` remains quiet, while the kernel debug shell still reports exits. The probe runner's scoring markers, `sys_exit: init process N exited with status M` and `terminate: ... killed by signal S`, remain unchanged. `make exitline-test` verifies the behavior in four boots and matches the complete marker. Matching fragments could mistake two copies of one exit for two distinct lines, so the child exits with 42 and the probe with 0.

### B: bounded /proc text (5518611)

Opening a `/proc` file generates its contents in a buffer, and later reads use that buffer, giving each descriptor one snapshot. Previously, an appender silently stopped at the buffer limit and could reduce a sixteen-row table to nine rows. Each file now declares a maximum, and the shared buffer uses the largest maximum. A negative-array-size `typedef` rejects a declared bound that does not fit. Text exceeding its bound returns `-EOVERFLOW` and produces a serial diagnostic identifying the file.

### C: the machine's own /proc files (c6f449b)

The phase added `/proc/uptime`, `/proc/stat`, `/proc/meminfo`, and `/proc/mounts`, used by `ps`, `df`, `free`, and `uptime`. Two files required measured values. The kernel has no idle task, so the timer samples marks placed around the kernel's `sti; hlt` waits. `tools/check.sh` rejects an unmarked `sti; hlt` outside `sched.c`, preventing an unaccounted wait from being added. Supporting `df` required the previously absent `statfs64` and `fstatfs64` calls. The mount line records its source and options while the selecting branch remains in scope. `make procfiles-test` boots all three mount sources and runs the programs against expected values unavailable inside the guest.

### D: the network files under /proc (10244c7)

The phase added `/proc/net/route`, `arp`, and `tcp` in the formats parsed by their consumers. `netstat` reads a uid and inode, but sockets previously had neither, so every row would have described one root-owned socket. Each socket now receives a unique inode and a uid captured in syscall context. A connection allocated during receive processing inherits its listener's uid. Both values are copied into the control block so a closing row retains them after detaching from its socket. The inode agrees across `/proc/net/tcp`, `/proc/<pid>/fd/N`, and `fstat`.

Review found two significant defects. Send-Q counted only unsent bytes instead of the complete write queue. A descriptor for any of the three new files resolved through its link as `/proc/0/stat` because neither origin dispatcher recognized those files.

### E: the terminal ioctls over a pty (5bc86e6)

The plan reported a `stty` failure in a Dropbear session and required diagnosis before correction. The kernel was working: `TCGETS`, `TCSETS`, and `TIOCGWINSZ` succeed on a pty slave, while `stty size` and `stty -a` report the dimensions and termios over SSH when the client has a known terminal size. The failing client had no terminal and requested a zero-by-zero window, as `ssh -tt` does from a script. BusyBox `stty` treats zero rows as an error and prints only the device name because that decision has no errno.

The phase added missing coverage instead of changing the kernel. `ptyprobe` sets the master window before forking, as a login server does; the child then requires the slave to report that window and support termios reads and writes. `make ssh-test` gives its client a 40-by-100 terminal and requires the session to report those dimensions. `resize` returns zero for the same reason as `stty`: a scripted terminal does not answer its query.

The `/proc/self/fd` investigation found one kernel defect. Every pipe resolved as `pipe:[1]`, making all pipes appear identical, and `fstat` returned no pipe inode. Pipes now receive unique inodes, as sockets do.

### F: the packet socket, finished (545a754)

The packet socket originally served only a DHCP client: it accepted only IPv4, had no protocol filter, never received ARP, used a fixed EtherType when sending, and returned no `sockaddr_ll` from `getsockname`. All five limitations were removed, allowing `arping` to respond on the test network. Receive also reports a real address, an unplanned requirement for `arping`. It discards frames whose `sll_pkttype` or `sll_hatype` it does not recognize, so a missing address caused it to wait indefinitely.

Review found that bind changed the socket filter before validating the interface, so a rejected bind altered later receives. Rebinding could also present a queued IPv4 datagram as ARP.

### G: raw ICMP sockets and IP_TTL (36be791)

This was the largest phase. Root can create an `AF_INET/SOCK_RAW/IPPROTO_ICMP` socket that receives complete datagrams, including headers required by its consumers. `ip_rx` submits each accepted datagram to every matching raw socket before invoking its normal handler. `ip_tx` gained a hop limit and return value. `ip.h` defines four outcomes; an ARP miss is not an error because datagram sends do not promise delivery and a later send can use the resolved address. `IP_TTL` is a per-socket value for raw and datagram sends. `IP_HDRINCL` is rejected instead of accepted and ignored.

BusyBox `traceroute` uses this support and reports the gateway as its first hop. Its `ping` applet remains disabled because it would take precedence over `/bin/ping`, the unprivileged in-house program whose output several tests inspect. `make raw-test` runs four boots and captures traffic. The capture verifies two properties invisible inside the guest: each datagram's transmitted hop limit and preservation of the caller's checksum on the wire.

### H: a boot line that contradicts itself (f537358)

`rach2.nodisk` skips disk probing. When an image used that disk as its root, no root remained to mount and the kernel entered the unauthenticated debug shell. That combination now halts after a diagnostic identifies both tokens. `rach2.rescue` is exempt because it explicitly requests the shell.

Review broadened the refusal because any root-mount failure could enter the same shell. A boot with no mounted root now halts regardless of its request. An image with a mounted root but no init still uses the debug shell as its intended environment. The test image depends on that case, and the harness still runs it.

### I: the mapping ceiling (db0c5fb, fa09074)

Milestone 22 set `VMA_MAX` to 128 after a link exhausted the table. Repeated 1 KB `malloc` calls failed after about 3.1 MB, while one 64 MB request succeeded, because musl creates a mapping for each small block and the table imposed the effective limit. The measurement justified raising the value to 512, after which the same walk passed 8 MiB. Mapping refusal had also been silent and resembled a library failure. The kernel now reports the process once on the serial log. Review moved the once-only marker from a remembered pid into the process because pid reuse could suppress the diagnostic for a later process.

### J: the boot log, under the name a person expects (f270b85, 9220448)

`dmesg` attempted the unsupported `klogctl` system call. Adding Linux's logging call for one command was rejected because the boot log already existed as a device. `/bin/dmesg` became a short script that prints `/dev/bootlog`, and the BusyBox applet was disabled to prevent it from taking precedence. The script rejects options instead of printing the log when the caller requested clearing or following behavior.

### The three notes milestone 27 left (dce720d)

The three notes identified these defects. `rach2/.gdbinit` still used `file kernel` after build-directory unification. `vga_putc_raw` checked control bytes with `c < 31`, allowing 0x1F to render as a glyph. `blockprobe` constructed file names one byte at a time and produced punctuation after `'9'` for files beyond the tenth. All three were corrected.

## What the reviews overturned

Every phase received independent review, and each review found an omission. These findings changed code rather than prose:

* Phase D: Send-Q counted only unsent bytes. Neither origin dispatcher recognized the three new `/proc/net` files, so their descriptors reported another file's identity.
* Phase E: every wait in `ptyprobe` could hang instead of fail, and the pipe-link parser accepted non-pipe text. Socket and pipe inode counters could allocate a number still in use after wrapping.
* Phase F: a rejected bind changed the socket filter, and rebinding could relabel a queued datagram.
* Phase G: raw send used shared transmit buffers without the network lock, allowing reentry by an interrupt-context sender. The claim that `raw_ready` required the lock was refuted. `wq_wait_event` evaluates its predicate with interrupts disabled, providing serialization; the packet socket had used the same method since milestone 25.
* Phase H: the refusal covered too few mount failures. The test also terminated its machine as soon as the diagnostic appeared and therefore could not distinguish a halt from continued execution.
* Phase I: "once per process" used a reusable pid as its key.
* Phase J: the script accepted and ignored options, while the test read the boot banner from the console instead of the command's output.

## What was not done, and why

* BusyBox `ping` is not built. The plan requested it and the machine now supports it, but the applet would take precedence over `/bin/ping`, the protected unprivileged in-house program whose output several tests inspect. `traceroute` has no collision and remains enabled; `rawprobe` exercises the socket directly.
* The fan-out test covers an echo reply and a time-exceeded message. The plan also requested an unreachable message, but QEMU's network does not generate one for its closed ports. The delivery path has no message-type branch, so the two available types exercise the same code.
* Packet-socket `recvfrom` reports the link address for the most recently received datagram and stores it per socket. `arping` uses that behavior. It differs from Linux's per-datagram address when queued packets are read out of order, but no program in this tree does so.

## Clean-tree verification

After `make distclean`, the following commands passed in order: `make full`, `verify-style`, `verify-host`, `test` with 63 of 63 probes, `packet-test`, `raw-test`, `bootline-test`, `procfiles-test`, `exitline-test`, `dhcp-test`, `ssh-test`, and `selfbuild-test`. The final test built the kernel twice in clean workspaces, compared equal hashes, and booted the result through GRUB and the complete probe suite. `make sealed-test` then rebuilt the sealed artifact at its required 660602880 bytes and exercised all eight phases over SSH. Its hash intentionally changes between builds because each build generates a demo password. The contract covers geometry and size; the hash identifies the particular copy awaiting a write.

## Waiting on hardware

The laptop was not powered on during this session. Hardware verification therefore remains for a DHCP lease after the packet-socket change, `traceroute` across a real route instead of one QEMU hop, and the sealed drive. Phase K's disk rewrite requires the owner to approve a specific node in person for every write, and no such approval was available. The complete local verification passed, including `make sealed-test`, which exercised the finished artifact over SSH in eight phases.
