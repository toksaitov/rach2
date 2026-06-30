# Milestone 12: the whole-system production-readiness review

Eleven milestones built the OS; this one tries to break it. Where `06-review`,
`09-audit`, and `11-audit` each audited the slice their predecessor had just
added, this review takes the whole system as its subject: the boot path and the
traps, physical memory and paging, the page cache, the block layer and KRFFS, the
system-call ABI, process and signal and job-control lifecycle, the fd and OFD
layer, the TTY and pty line discipline, wait queues and poll and pipes, the entire
network stack from the socket layer down through TCP to the RTL8139, the audio
drivers, the display and the peripherals, the freestanding library and the
CSPRNG, the in-house userland, the KRFFS host tooling, and the trust boundary
itself, consolidated. It is the production-readiness pass: the question is no
longer "does the new code work" but "where does the finished system fail when it
is pushed past the paths the green probes walk."

## The method

Thirty risk-ordered phases (`P0` through `P29`), each run in the discipline the
prior audits settled on and the plan (`plan.md`, built with Codex from a survey of
the whole surface) lays out. A phase fans out parallel auditors, one per concern
cluster; a separate adversarial verifier tries to refute every candidate, defaulting
to refuted when uncertain; and then Codex re-reads the phase independently. Only a
confirmed finding is fixed, each in a scoped one-line commit, and the global gate
(`verify-style` 8/8, `verify-host`, `make test`, the disk and persistence probes,
`check-sources`) holds green at every step. Where a phase licensed it, the fix
extended a test or rewrote the owning Readme in the same commit.

The independent Codex re-verify earned its place in every phase, and twice it was
load-bearing in a way the prior audits had only hinted at. It repeatedly rejected a
phase the fan-out had passed and named a real gap the parallel readers had missed:
the socket and signal error-return paths, an integer overflow, a closing-state
transition. More striking, in the peripherals phase it overturned a refutation. The
fan-out's adversarial verifier had looked at the 8259 bring-up and declared the
early-unmask concern unreachable; Codex, re-auditing the same code, constructed the
reachable case and proved it real. An audit that trusted its own refutations would
have shipped that bug. And in the TCP phase Codex found two regressions in the
review's own retransmit fix, the same lesson the last audit closed on: keep a reader
who did not write the fix.

## What it found

About fifty confirmed defects across the substantive phases, plus a clean bill on
the two phases that matter most for the negatives: the hostile-packet parsing and
the ring-3 trust boundary. The pattern the earlier audits noticed holds and
sharpens here. The severity lives where QEMU and the passing probes never go: on the
hangup path, in the connection-closing states, on the error returns, at the real
hardware edges, and behind the spellings of a path or an option no normal run
types. The network stack, the largest and least-exercised subsystem, held the most.

The highest-value finds, by where they hid:

* TCP, the connection-closing states and the send side. `tcp_rx` consumed a FIN in
  `FIN_WAIT_1`/`FIN_WAIT_2` with no in-order sequence check, so a duplicate or
  out-of-order FIN corrupted `rcv_nxt`; those states delivered no data at all, so a
  peer that kept sending after our close lost its bytes and was mis-acknowledged;
  a segment carrying both data and a FIN advanced `rcv_nxt` over a truncated
  payload, signalling EOF before the data was delivered; a retransmit timer fired
  for a close with unacknowledged data resent only the FIN, never the lost data, so
  the teardown stalled until giveup; and the send path returned `0` on a full ring
  with no backpressure, so a blocking writer livelocked and `POLLOUT` lied. The fixes
  rebuild the closing-state receive path (deliver in order, gate the FIN on its data,
  validate the ACK, re-ACK a retransmission), give the send a real blocking and
  `-EAGAIN` discipline woken when an ACK frees ring space, and surface a connection
  error through `SO_ERROR` and `POLLERR`. Codex's re-verify caught the retransmit
  fix counting queued-but-unsent bytes as in flight and the closing states failing
  to advance the send-ring acknowledgement, both fixed before close. The whole
  rework is verified live: `tcptest` and `sockettest` pass against a host echo
  server over SLIRP, and the SSH capstone runs the real stack end to end.

* The PS/2 keyboard, before its handler exists. `init_pic` unmasked every IRQ line
  and `init_isr` enabled interrupts before the keyboard handler was installed, so an
  early IRQ 1, a keystroke or a byte the BIOS left in the controller, was
  acknowledged without reading port `0x60` and stranded the controller's output
  buffer, leaving the keyboard dead. This is the finding the fan-out refuted and
  Codex rescued. IRQ 1 now stays masked until its handler is in place; the 8259
  latches the held request, so a buffered byte is delivered the moment it unmasks.

* The line discipline, on hangup. A raw-mode read consulted only `VMIN` and never
  the end-of-file a hangup sets, so a raw pty slave whose master had closed blocked
  forever instead of returning `0`, the exact path a dropbear SSH session takes. A
  pty slave read ignored `O_NONBLOCK`, and a canonical slave whose master had closed
  read EOF once and then blocked rather than reporting persistent end-of-file. A
  control character set to zero (the disable convention) still matched an incoming
  NUL, so disabling an interrupt key turned every NUL into a spurious signal. All now
  read the way a real terminal does.

* The path classifier, behind a spelling. The synthetic `/dev` and `/proc` mounts
  are decided lexically, while KRFFS collapses `..` on resolution, so a name like
  `/x/../proc` classified as a real file and could create a KRFFS node that shadowed
  the synthetic mount; `execve` reached KRFFS without classifying at all. The kernel
  now canonicalizes an absolute path once, before classification, and `execve`
  classifies like every other path syscall.

* The host tooling, against a hostile path or image. `defrag.krffs` ran `fsck`
  through `system()` with the image path concatenated into the shell string, so a
  path with a shell metacharacter ran arbitrary commands; it now forks and execs
  with the path as a distinct argument. The shared node-bounds check accepted a node
  whose header straddled the end of the mapping, and `fsviz` and `fsck` walked a
  corrupt or non-KRFFS image without a magic check or an iteration cap; all now fail
  closed.

The rest spread across the phases: a default handler for the CPU exceptions that had
none; a per-fork kernel-stack leak fixed with a real stack arena; a stale-node
use-after-free across the fd operations; a relocate-on-grow that dropped a KRFFS
node's parent and a mount that never validated the on-disk chain; a `select` that
busy-spun on a hangup-only write descriptor and a timed `poll` that lost its timeout
when another process was runnable; a UDP datagram read out from under the receive
IRQ; a listener that leaked its accept queue and a second `listen` or `connect` that
leaked the old control block; an SB16 stereo desync and an unbounded HDA wall-clock
spin; a `k_itoa` that negated `INT_MIN` in a signed `int` and a `k_strreverse` that
walked off an empty string, each now caught by a host test; an init that respawned a
crash-looping shell at full CPU; and three gate scripts that could report a false
green. Every fix is its own commit, and the larger structural debt the review
surfaced but did not act on (the two oversize source files, a reproducible-build
pin for the Docker-built ISO, a handful of host tests that the 32-bit-pointer host
build cannot run) is recorded for a focused follow-up rather than rushed in at the
tail of a thirty-phase audit.

The two negative phases came back clean and are worth stating as such. Every layer
of the inbound packet path, Ethernet through IP, UDP, ARP, and ICMP, and the RTL8139
receive ring, bounds its reads against the captured length before trusting a field;
no runt, oversized-length, header-underflow, or ring-wrap frame reaches an
out-of-bounds access. And an independent attempt to construct a ring-3 to ring-0
escape found none: the user-copy helpers reject an address-plus-length wrap and a
crossing of the page-cache window, the raw framebuffer and audio hand-offs are
bounded, the page-cache fault is gated on supervisor mode, and the CSPRNG fails
closed. The one change the security phase made was defensive: `vm_prefault_range`
now self-defends against the wrap its callers already reject.

## Verification

The global gate held at every phase and once more at close: `make verify-style`
8/8, `make verify-host` over fifteen host seam tests, `make test` at thirty-five
boot probes, `make check-sources`, and the IDE and AHCI disk, persistence, crash,
and stress probes. The trust-boundary and parsing fixes are revert-verified, the
test reintroducing the bug confirmed to fail before the fix. The network rework is
verified live against a host echo server, not just by reasoning. And the capstone
matrix runs the finished system end to end: the SSH server authenticates a public
key and runs busybox `ash` over a pty to a clean disconnect, busybox is the live
login shell, and Doom loads its WAD and reaches its refresh daemon and play loop.
The vendored musl, BusyBox, Dropbear, and Doom trees stayed out of the diff
throughout.
