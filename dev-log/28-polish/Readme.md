# Milestone 28: the diagnostics

What a person on this machine can ask it about itself. The plan and the
reasoning behind each phase are in `plan.md`; this file records what was built,
what the reviews overturned, and what is still waiting on hardware.

Every phase held the same shape: one scoped change, the repository's own gate,
a test proved by making it fail, an independent review of the diff with every
finding adjudicated against the code, the documentation that owns the changed
contract rewritten in the same commit, and one commit in the house style.

## What was built

### A: the quiet exit line (a38dedc)

`sys_exit` printed `Process exited with status N` for every process that
exited, including a forked child's, onto the console of a machine running a
real init. It is gated on `debug_shell_is_home` now, so an image with
`/sbin/init` is quiet and the kernel debug shell still prints it. The two
dedicated markers the probe runner scores from (`sys_exit: init process N
exited with status M` and `terminate: ... killed by signal S`) are untouched,
because they are the scoring contract. `make exitline-test` proves it in four
boots and asserts the whole marker line, not a fragment: two plain lines could
otherwise be two copies of one exit, so the child exits 42 and the probe 0.

### B: bounded /proc text (5518611)

A `/proc` file is generated into a buffer when it is opened and read back from
there, so one descriptor shows one moment. The appenders used to stop when they
ran out of room, which turned a sixteen-row table into a nine-row one with
nothing saying so. Each file now declares its own maximum, the shared buffer is
the largest of them, a `typedef` of a negative array size stops the build if a
declared bound will not fit, and a text that exceeds its bound is refused with
`-EOVERFLOW` and one line on the serial log naming the file.

### C: the machine's own /proc files (c6f449b)

`/proc/uptime`, `/proc/stat`, `/proc/meminfo` and `/proc/mounts`, which is what
`ps`, `df`, `free` and `uptime` read. Two of them needed something real behind
them rather than a plausible number. There is no idle task here, so idle time
is measured by marking the kernel's own `sti; hlt` waits and sampling the mark
from the timer, and `tools/check.sh` now rejects an unmarked `sti; hlt` outside
`sched.c` so a new wait cannot be added silently. `df` needed `statfs64` and
`fstatfs64`, which this kernel had never had. The mount line names its source
and options, recorded where the branch that chose them is still in scope.
`make procfiles-test` boots the three mount sources and then runs the programs
against numbers the guest does not hold.

### D: the network files under /proc (10244c7)

`/proc/net/route`, `arp` and `tcp`, each in the shape the program that reads it
parses, taken from the parser rather than from a description. The part worth
recording is identity: `netstat` reads a uid and an inode, and a socket had
neither, so every row would have said the same socket owned by root. Each
socket now carries an inode of its own and a uid taken in syscall context; a
connection allocated in receive context inherits the listener's uid, which is
whose socket it is, and both are copied into the control block so a row in a
closing state still reports them after its socket is detached. The same inode
appears in `/proc/net/tcp`, in `/proc/<pid>/fd/N` and in `fstat`.

The review of this phase found the two defects that mattered most in it: the
Send-Q column reported only unsent bytes rather than the whole write queue, and
a descriptor opened on one of the three new files reported `/proc/0/stat`
through its link because neither origin dispatcher had been taught about them.

### E: the terminal ioctls over a pty (5bc86e6)

The plan reported `stty` failing on a dropbear session and asked for a
diagnosis before a fix. The diagnosis found no fault in the kernel: `TCGETS`,
`TCSETS` and `TIOCGWINSZ` on a pty slave all work, and over SSH with a client
terminal of a known size, `stty size` prints it and `stty -a` prints the
termios. What was reported was a client with no terminal of its own asking for
a window of zero by zero, which is what `ssh -tt` from a script does; busybox
`stty` treats a zero row count as an error and prints the device name with
nothing after it, because there is no errno behind that decision.

So the phase added the coverage that was missing rather than a fix: `ptyprobe`
now sets the window on the master before the fork, as a login server does, and
the child requires the slave to report it and the termios to read and write;
`make ssh-test` gives its client a terminal 40 by 100 and requires the session
to report that size back. `resize` answers zero for the same reason `stty` did:
it asks the terminal, and a terminal that is a script does not answer.

One real defect came out of the `/proc/self/fd` half of the question: every
pipe on the machine resolved to the constant `pipe:[1]`, which said they were
all one pipe, and `fstat` reported no inode for a pipe at all. Pipes now carry
their own, like sockets.

### F: the packet socket, finished (545a754)

The packet socket was built for one caller, a DHCP client, and stopped where
that caller stopped: IPv4 only, no protocol filter, ARP frames never offered,
a hardcoded EtherType on send, and no `sockaddr_ll` from `getsockname`. All
five are done, and `arping` answers on the test network as a result. The
address a receive reports is real too, which the plan did not ask for and
`arping` cannot work without: it discards every frame whose `sll_pkttype` and
`sll_hatype` it does not recognise, so a receive that reported no address left
it waiting forever.

The review caught a bind that changed the socket's filter before the interface
was validated, so a refused bind left the socket receiving something else, and
a rebind that could hand a reader a queued IPv4 datagram labelled as ARP.

### G: raw ICMP sockets and IP_TTL (36be791)

The largest phase. `AF_INET/SOCK_RAW/IPPROTO_ICMP` is a real socket now, root's,
receiving whole datagrams with the header included because that is what the
programs parse, and `ip_rx` offers each accepted datagram to every matching raw
socket before its own handler runs, which still runs. `ip_tx` gained a hop
limit and a return value, and the four outcomes are named in `ip.h`: the ARP
miss is the one that is not an error, because a datagram send promises no
delivery and the next one leaves resolved. `IP_TTL` is a real per-socket value
for a raw send and a datagram send alike, and `IP_HDRINCL` is refused rather
than accepted and ignored.

busybox `traceroute` runs on this and names the gateway as its first hop. Its
`ping` is deliberately not built: the applet would answer from the shell's
table before `/bin/ping`, the in-house program an ordinary account can run, and
several tests read that program's output. `make raw-test` is four boots and a
capture; the capture is what proves the two things no program inside the
machine can see, the hop limit each datagram left with and the caller's own
checksum crossing the wire unrepaired.

### H: a boot line that contradicts itself (f537358)

`rach2.nodisk` skips the disk probe, and on an image whose root is that disk it
left nothing to mount, so the kernel fell through to the debug shell, which
authenticates nobody. That combination halts now with a line naming both
tokens, and `rach2.rescue` is the exemption, because asking for that shell
deliberately is what the token is for.

The review widened it, correctly: the same fall-through opens for any root that
does not mount, not only for the skipped one. A boot that mounted nothing now
halts whatever asked for it, and the one case where the debug shell is a home
and not a way in, an image whose root mounted and has no init, is untouched.
That is the test image, and the harness still drives it.

### I: the mapping ceiling (db0c5fb, fa09074)

`VMA_MAX` was 128, chosen in milestone 22 for a link that ran out, and a
process asking `malloc` for 1 KB at a time got about 3.1 MB before it began to
fail while a single 64 MB request succeeded: musl carves a mapping per small
block, so the ceiling a program meets is the table. It is 512, from that
measurement, and the same walk passes 8 MiB. The other half was silence: the
kernel refused the mapping and said nothing, so the failure looked like the
library's own. It now names the process, once, on the serial log. The review
moved that mark from a remembered pid onto the process itself, since a pid is
reused and the next process to hold the number would have been silenced.

### J: the boot log, under the name a person expects (f270b85, 9220448)

`dmesg` failed on `klogctl`, a system call this kernel does not have.
Implementing Linux's logging call for one command would be the wrong shape, and
the log is already a device, so `/bin/dmesg` is a short script that prints
`/dev/bootlog` and the applet is turned off, which is also what keeps it from
answering before the file. It refuses options rather than printing the log for
a caller that asked to clear or follow it.

### The three notes milestone 27 left (dce720d)

`rach2/.gdbinit` still said `file kernel`, stale since the build directory was
unified; `vga_putc_raw` guarded control bytes with `c < 31`, so 0x1F was drawn
as a glyph; and `blockprobe` built file names one byte at a time, which ran past
`'9'` into punctuation for the files past the tenth. All three are fixed.

## What the reviews overturned

Every phase took an independent review, and each one found something the work
had missed. The findings that changed the code rather than the prose:

* Phase D: Send-Q reported unsent bytes only; the three new `/proc/net` files
  had no entry in either origin dispatcher, so a descriptor on one of them
  reported another file's identity.
* Phase E: every wait in `ptyprobe` could hang rather than fail, and the pipe
  link parser accepted text that was not a pipe link. The socket and pipe inode
  counters could hand out a live number after a wrap.
* Phase F: a refused bind changed the socket's filter; a rebind could relabel a
  queued datagram.
* Phase G: the raw send built its datagram through the shared transmit buffers
  without the network lock, which an interrupt-context sender can re-enter.
  The claim that `raw_ready` needed that lock was refuted: `wq_wait_event`
  evaluates a predicate with interrupts already off, which is what serializes
  it, and the packet socket has read it the same way since milestone 25.
* Phase H: the refusal was too narrow, and the test killed its machine at the
  moment the diagnostic appeared, so it could not tell a kernel that halted
  from one that printed and carried on.
* Phase I: "once per process" was keyed to a pid, which is reused.
* Phase J: the script took options and ignored them; the test read the boot
  banner off the console rather than out of the command's own output.

## What was not done, and why

* busybox `ping` is not built. The plan asked for it, and the machine can run
  it now, but the applet would shadow `/bin/ping`, the unprivileged in-house
  program that the plan's own list of things not to break protects and that
  several tests read the output of. `traceroute` has no such collision and is
  built, and `rawprobe` covers the socket directly.
* The fan-out test covers an echo reply and a time-exceeded message. An
  unreachable was asked for as well; the network QEMU provides does not produce
  one for its own closed ports, and the delivery path does not branch on the
  message type, so the two kinds exercise the same code.
* `recvfrom` on a packet socket reports the link address of the datagram most
  recently taken, kept per socket. That is what `arping` reads, and it is not
  the same as Linux's per-datagram address on a queue read out of order; no
  program here does that.

## The gate, from a clean tree

`make distclean` and then the whole thing, in this order, every one green:
`make full`, `verify-style`, `verify-host`, `test` (63 of 63 probes),
`packet-test`, `raw-test`, `bootline-test`, `procfiles-test`, `exitline-test`,
`dhcp-test`, `ssh-test`, `selfbuild-test` (the machine built its kernel twice
from clean workspaces, hashed equal, and booted the result through GRUB and the
whole probe suite). `make sealed-test` then rebuilt the sealed artifact at its
contracted 660602880 bytes and operated it over SSH through all eight phases.
The artifact's hash differs from build to build by design: the demo password is
generated per build, so what is contracted is the geometry and the size, and
the hash belongs to the copy that is about to be written.

## Waiting on hardware

The laptop is not powered on for this session, so every claim that needs it
waits: a DHCP lease on real hardware for the packet-socket change, `traceroute`
across a real route rather than one hop of QEMU's, and the sealed drive.
Phase K's disk rewrite requires the owner present to approve a named node,
every time, and that approval cannot be given from here. What did run is the
whole local gate, including `make sealed-test`, which operates the finished
artifact over SSH in eight phases and passed.
