# Milestone 11: the post-AHCI audit

Status: complete. All eight phases executed and gated; eleven real defects fixed
and the rest refuted. See `Readme.md` for the milestone record.

## The goal

The AHCI milestone added 927 lines across fifteen files: a new 669-line SATA
driver (`sys/block/ahci.c`), the block-layer seam that lets two drivers publish
`block0` (`block.c`/`block.h`/`ata.c`), three `kernel.c` integration points, two
PCI constants, the `sys/block/Readme.md` contract doc, and the AHCI test and run
wiring (`harness.py`, `run.sh`, the root `Makefile`). It is contained: nothing
above `block0` changed, and the default RAM and legacy IDE boots are meant to be
byte-identical. This audit proves the new driver correct under the polled IF=0
model, proves the seam did not disturb the old boot contract, and brings every
comment, Readme, and support file into exact agreement with the code and with the
`CLAUDE.md` style contract.

It is the post-AHCI counterpart of `dev-log/09-audit`, run in the same discipline:
every candidate finding is adversarially verified before it counts, most are
expected to be refuted, and Codex independently re-verifies each phase's findings
before they are promoted to fixes. The surface is small, so the audit is eight
focused phases, not ten.

## Blast radius this audit answers to

The AHCI milestone (`e79c781..060e528`) added or changed:

* `sys/block/ahci.c` (NEW, 669 lines): PCI discovery and ABAR map, HBA ownership
  (BIOS handoff, `GHC.AE` on / `GHC.IE` off, the deliberately-skipped `GHC.HR`),
  all-port quiesce, first-present-port bring-up, the polled command path
  (`ahci_run_command`: H2D FIS + single PRDT + `PxCI`/`PxIS`/`PxTFD` poll),
  `ahci_identify`, `ahci_read`/`ahci_write`/`ahci_flush`, the cache-sync helper,
  and the `ahci_dump`/`ahci_selftest` debug surface.
* `sys/block/ahci.h` (NEW): the Doxygen contract for the three public entry points.
* `sys/block/block.c`: the `block0` definition (moved out of `ata.c`) and
  `block_probe` (the `rach2.disk=ahci` selection seam with an ATA fallback).
* `sys/block/block.h`: the `block_probe` declaration; `sys/block/ata.c`: lost its
  `block0` definition, otherwise unchanged.
* `kernel.c`: three `ata_init`->`block_probe` sites (`init_krffs_disk`, the `disk`
  and `disk write` debug commands), the `ahci` debug command, and the neutralized
  diagnostics.
* `sys/pci.h`: `PCI_CLASS_MASS_STORAGE 0x01`, `PCI_SUBCLASS_SATA 0x06`.
* `sys/block/Readme.md` (NEW), `dev-log/10-ahci/{plan,Readme}.md`,
  `dev-log/Readme.md`: the documentation.
* `tools/harness.py` (`--qemu-arg`), `rach2/run.sh` (`DISKBUS=ahci`), the root
  `Makefile` (`test-disk-ahci`/`persist-test-ahci`/`stress-test-ahci`), and
  `rach2/Makefile` (`ahci.c` in `C_SRC`): the test and run wiring.

The invariants the audit must protect, and confirm are documented right:

* The I/O path polls and never sleeps. `ahci_read`/`ahci_write` run inside the
  page-cache window fault at IF=0: no `sti`, `hlt`, wait queue, allocation, or
  unbounded spin on the path, and every wait is an iteration watchdog that returns
  `-EIO` on expiry.
* The page-cache window stays the only route from ring 3 to `block0->read`, and it
  stays strictly `!user_mode`-gated (the milestone-9 fix). No user path reaches the
  AHCI code directly; the `ahci`/`disk` debug commands stay rescue-gated.
* The default RAM boot and the legacy `if=ide` ATA boot are byte-identical: AHCI is
  dormant unless `rach2.disk=ahci` selects it, and `block_probe` then falls back to
  ATA if no HBA answers.
* Every DMA buffer that reaches the PRDT is identity-mapped and below 4 GiB, so the
  truncated 32-bit `dba` with `dbau=0` names the right physical page.
* Bus mastering is enabled only after the selected port's DMA bases are programmed,
  and `GHC.IE` is cleared before it.

## Scope and non-goals

In scope: the milestone surface above, read against five dimensions (logic and
correctness, security and robustness, integration and regression, support scripts
and gates, documentation and style), plus the integration points the seam touches
(`pc_fault`/`pc_set_backing`, `init_krffs_disk`, `cmdline.c`). Out of scope and
off-limits: the vendored trees (`musl`, `busybox`, `dropbear`, `krffs/vendor`) stay
untouched, and the subsystems the AHCI milestone did not touch are not re-audited
(milestone 9 already swept the tree; this audit answers only to the AHCI blast
radius). This audit fixes only what adversarial verification confirms; it does not
refactor for taste, and it does not change a test contract (the probe tokens, the
28-probe module set, the byte-identical default boot) without flagging the change
first.

## Method, common to every phase

Each phase is one Workflow fan-out that `/goal` runs to completion, with the
maintainer reviewing between phases. The shape is fixed, and Codex is woven into
every phase, not only the hard ones:

* Fan out parallel auditors, one per file or concern cluster, each handed the
  concrete concerns below with their `file:line` anchors. An auditor reads; it does
  not fix.
* Every candidate finding is handed to a separate adversarial verifier whose job is
  to refute it: read the surrounding code, the callers, the invariant it supposedly
  breaks, and either kill the finding or promote it with a reproduction or a tight
  argument. A finding does not count until it survives a hostile second read.
* Codex independently re-verifies the phase: it re-reads the promoted findings
  against the code and the spec and either confirms or refutes each, and it rejects
  the phase if the fan-out missed a concern in a changed contract file. This is the
  required Codex step at every phase. Its verdict is recorded with the phase token.
* Confirmed findings become scoped fixes, one logical change each, committed in the
  house one-line style with no attribution. A text-only fix changes no behavior and
  no built artifact. Comments and docs are fixed in the phase that owns them.
* Each phase ends at its gate and its exit token; the maintainer reviews, then
  `/goal` proceeds.

The security lens is applied in every phase and consolidated in Phase 5; the style
and documentation lens is applied as files are read and consolidated in Phase 7.

## Phase 1: baseline and map

Scope: fix the green state every later phase diffs against, and confirm the audit
map covers every changed contract file. Nothing is fixed here.

Work:

* Record the baseline: `make verify-style` (8/8), `make verify-host`, the
  module-path `make test` (28/28), the ATA disk probes (`test-disk`,
  `persist-test`, `stress-test`), the AHCI disk probes (`test-disk-ahci`,
  `persist-test-ahci`, `stress-test-ahci`), a default RAM boot, the SSH capstone,
  and a Doom run. Capture the exact tokens and counts.
* Confirm the changed-file map (`git diff --stat e79c781~1..HEAD`, 15 files) is
  fully covered by Phases 2-7, with no changed contract file unowned.

Gate: every baseline check green and recorded; the map accounts for all 15 files.
The disk-probe and AHCI-probe tokens captured here are provisional until Phase 6
clears the `--ok` gate: `harness.py` returns 0 on token presence alone, so a
`FAIL`-then-`OK` or a hang-after-`OK` would read as green at this point. Codex
re-reads the changed-file map and rejects the plan if any changed contract file is
unowned. Exit token `AUDIT-BASELINE OK`.

## Phase 2: the HBA, PCI, MMIO, and DMA ownership

Scope: `ahci_discover`, `ahci_hba_own`, `ahci_port_stop`/`ahci_quiesce_ports`,
`ahci_select_port`, `ahci_port_start`, and the `ahci_init` ordering. The
highest-risk bring-up code: stale-DMA, bad-MMIO, and interrupt-leak hazards live
here.

Concerns to chase:

* Bring-up ordering (`ahci.c:574-610` init order, `:288-301`, `:379-405`). The
  `GHC.AE`-on / `GHC.IE`-off must be a single read-modify-write that cannot
  momentarily enable IE; `pci_enable_bus_master` must be strictly after
  `PxCLB`/`PxCLBU`/`PxFB`/`PxFBU` are written and before `PxCMD.ST`; FRE must
  precede ST; and a quiesce that fails on any port must abort init rather than
  proceed with a half-quiesced HBA.
* Discovery and ABAR (`ahci.c:235-278`, `pci.c:345-364`, `paging.c:471-512`). The
  BAR5 guards must reject a 64-bit BAR4 spill, a non-memory BAR5, and a zero BAR5;
  `map_mmio` runs only when `abar >= HARD_MEMORY_LIMIT`, leaving a latent
  cacheable-MMIO risk if a real controller put the ABAR low; memory-space decoding
  must be enabled before `ahci_hba_own` reads `CAP2`/`GHC`; `map_mmio` failure must
  abort; `AHCI_ABAR_MAP_SIZE` (0x2000) must over-cover the highest port register
  read.
* Port selection (`ahci.c:350-376`). The code selects on `DET==3 && IPM==1` only,
  but the plan specified also requiring `PxTFD` not BSY/DRQ; decide whether the
  pre-issue task-file-idle wait in `ahci_run_command` makes the selection check
  sufficient, and whether `IPM==1`-only wrongly rejects a present device in a
  non-active interface power state on real hardware (QEMU reports active).
* The deliberate `GHC.HR` skip (`ahci.c:280-287`). Re-verify against the documented
  QEMU `PxTFD 0x50->0x130` behavior that skipping HR plus the all-port quiesce
  leaves no stale `PxIS`/`PxSERR`/`PxCI` from a prior owner.
* Partial-init effects after a failed bring-up. A failure after `ahci_port_start`
  leaves the HBA bus-mastering with the port started, then `block_probe` falls back
  to ATA. Phase 5 owns the question of whether that leftover state is tolerable;
  Phase 2 only confirms the ordering that produces it (bus mastering enabled inside
  `ahci_port_start`).

Gate: a source trace proving no `PxIE`/`GHC.IE` enable and no `pci_enable_interrupts`
on any path, bus mastering only after the DMA bases are set, and a clean
`ahci: selftest: MBR signature OK` from the `ahci` debug command. Codex re-verifies
each ownership/DMA finding, pressing hardest on stale-port DMA and the BAR-spill
guard. Exit token `AUDIT-HBA-DMA OK`.

## Phase 3: the polled command path

Scope: `ahci_run_command`, the packed FIS/PRDT/header overlays, `ahci_identify`,
`ahci_read`/`ahci_write`/`ahci_flush`, and `ahci_dma_sync`. The highest
data-corruption and hang risk: a missed status bit silently corrupts or wedges.

Concerns to chase:

* FIS/PRDT/header construction (`ahci.c:417-478`, `:117-144`). Verify every field
  against the SATA H2D Register FIS and AHCI command-header layout: `fis_type=0x27`,
  C bit, command byte, the 48-bit LBA spread `lba0..lba5`, `countl/counth`,
  `device=0x40`; the header `dw0` with `CFL=5` dwords, the W bit, `PRDTL=1`; the
  PRDT `dbc=(bytes-1)` masked to 22 bits. Confirm the packed overlays carry no
  padding and that the volatile writes match what the HBA DMA-reads.
* Completion logic (`ahci.c:480-507`, `:220-232`). The loop spins on `PxCI` while
  checking `PxIS.TFES`, then checks `PxTFD.ERR` after. Two real concerns: `TFES` is
  not the only fatal status. The host-bus/interface error bits (`PxIS.HBFS`,
  `HBDS`, `IFS`) are not watched, so a non-taskfile DMA fault spins to the watchdog
  instead of failing fast; and the post-error path does not clear `PxIS`/`PxSERR` or
  restart the engine, so whether the next command can issue after a `TFES` error is
  unverified. Decide whether to watch the fatal bits and whether an error is
  terminal or recoverable, and document the choice.
* `bytes` vs `count` coupling (`ahci.c:428-467`, `:539-570`, `:103`).
  `ahci_run_command` takes `count` (drives the FIS sector count) and `bytes` (drives
  the PRDT) independently, with no internal assertion that `bytes == count*512`; the
  `AHCI_MAX_SECTORS_PER_CMD=8192` cap is enforced in `ahci_read`/`ahci_write`, not
  in `ahci_run_command`. Confirm the only callers always pass a consistent pair, and
  that the cap exactly matches the single-PRDT 4 MiB reach.
* The descriptor flush span (`ahci.c:199-218`, `:474-478`, `:504-506`). The
  command-list header is flushed for `sizeof(ahci_cmd_header_t)=16` bytes, but the
  HBA reads 32 bytes per slot (the upper 16 are zero and unused, so confirm 16
  suffices); the command-table flush covers the full 256 bytes; the post-read
  invalidate must reach the trailing partial cache line; `WBINVD` at IF=0 is safe
  but heavy.
* IDENTIFY (`ahci.c:510-529`). Words 100-103 are the LBA48 user-addressable count,
  assembled little-endian, read from a DMA buffer (so no byte-swap, matching the ATA
  PIO data path); the zero-count rejection fails a non-LBA48 device. Confirm `idbuf`
  is a valid single-PRDT target.
* No-data path (`ahci.c:439`, `:462-467`, `:531-536`). The PRDT is built only when
  `bytes > 0`, so FLUSH (`bytes=0`, `PRDTL=0`) and IDENTIFY (`bytes=512, count=0`)
  are both correct; `ahci_zero` clears the full command table so no stale PRDT
  leaks; `ahci_write` must propagate a failed `ahci_flush`'s `-EIO`.
* The freestanding 64-bit shifts (`ahci.c` LBA `>>32`/`>>40`). Re-confirm by
  `nm`/`objdump` that no libgcc helper (`__lshrdi3`/`__ashrdi3`/`__udivdi3`) appears
  in the object under `-nodefaultlibs` (Codex confirmed this for the milestone; the
  audit re-checks the committed object).

Gate: `make test-disk-ahci` (`BLOCKPROBE OK`), `make persist-test-ahci` (both
`PERSISTPROBE OK` tokens), `make stress-test-ahci` (`STRESSPROBE OK`), and the
object inspection clean. Codex re-verifies each promoted finding against the AHCI
layout, ATA command semantics, and the generated object. Exit token
`AUDIT-COMMAND OK`.

## Phase 4: the block seam and KRFFS integration

Scope: the `block0` move and `block_probe` (`block.c`), the three `kernel.c` call
sites and the `ahci`/`disk`/`disk write` debug commands, `pci.h`, and the
`init_krffs_disk` mount path. One new driver must not disturb the old boot
contract.

Concerns to chase:

* `block0` linkage and joint ownership (`block.c:9`, `block.h:33`, `ata.c:276,332`,
  `ahci.c:572,604`). Exactly one definition, both drivers assigning it; `ahci_init`
  leaves `block0` untouched on every `-1` path (relying on `block_probe`'s
  short-circuit plus `ata_init`'s reset); `ata_device`/`ahci_device` are static so a
  captured `block0` (`_krffs_disk_part.backing`) never dangles.
* `block_probe` policy (`block.c:84-94`, `cmdline.c:24-52`). Whole-token matching so
  `rach2.disk=ahci` cannot collide with `rach2.fsdisk` (test `rach2.disk=ahcix`, a
  prefix, a neighbor); the ATA fallback reached only when `ahci_init` returns
  nonzero; `ahci_init` returns 0 only when `block0` is set.
* Default preservation (`ata.c:271-337`, `run.sh:67-84`). Diff `ata.c` to confirm
  the only change is the removed `block0` definition; the no-`DISKBUS` `run.sh` path
  still emits `if=ide,index=0,media=disk` and appends no `rach2.disk=ahci`; the
  ATAPI/SATA-signature refusal (`ata.c:301-304`) still keeps ATA off an AHCI disk.
* Re-probe over a live mount (`kernel.c:1850,1882,1910-1916`, `ahci.c:331-348`,
  `kernel.c:1567`). The debug `disk`/`disk write`/`ahci` commands re-run the full
  bring-up, which quiesces and restarts the very port the page cache may be using
  under a mounted KRFFS. Trace whether this disturbs an in-flight cache, leaves the
  port cleanly restarted, or risks a wedge; decide whether the commands should
  refuse to re-probe a mounted disk, or whether the contract docs should mark
  re-probe destructive (mirroring the existing `disk write` last-sector note).
* Call sites and messages (`kernel.c:1496-1499,1850-1858,1882-1896,1824,1910-1916`).
  All three keep the `!= 0 || !block0` guard; the neutralized messages
  (`disk: block0: ... sectors`, `disk: no boot disk`) obey `component: detail`
  lowercase and do not imply ATA when AHCI is underneath.
* `pci.h` additions (`pci.h:43-44`, `ahci.c:235-278`). The two constants are correct
  and consumed by the class/subclass/prog-if filter, do not collide with the audio
  constants, and AHCI discovery works with `init_pci` never called.
* Partition arithmetic (`kernel.c:1544-1568`). The `part_bytes` multiply and the
  size-vs-window-cap handling after the MBR probe, for overflow or a partition the
  host can mint but the kernel rejects only at mount.

Gate: default RAM boot byte-identical, `make test` 28/28, the ATA disk probes green
(default preservation), the AHCI disk probes green. Codex re-verifies the
integration findings, pressing hardest on the debug re-probe over a mounted disk.
Exit token `AUDIT-BLOCK-SEAM OK`.

## Phase 5: failure modes, bounds, and security

Scope: the AHCI driver's robustness and the security boundary, consolidated. The
success probes exercise the happy path; this phase reasons the failure paths and the
trust boundary that no probe covers.

Concerns to chase:

* The range guard corners (`ahci.c:542-545,558-561`, vs `ata.c:182-186`,
  `block.c:24-27`). Prove `count==0 || lba > sector_count || count > sector_count -
  lba || count > cap` is underflow-safe for every corner: `lba==sector_count`,
  `count==sector_count-lba` (exact-fit last read), `count==cap`, and a
  device-reported huge `sector_count`. The `>` (not `>=`) is load-bearing and must
  match the ATA and partition siblings.
* The DMA buffer trust boundary (`ahci.c:428-508,539-570,513-529`). The PRDT `dba`
  is a truncated 32-bit address with `dbau=0`; nothing checks the buffer is below
  `HARD_MEMORY_LIMIT`, identity-mapped, contiguous, or non-NULL. Enumerate every
  caller (the page-cache frames from `alloc_kernel_frame`, the `init_krffs_disk`
  MBR/partition buffers, the debug-command buffers, `idbuf`, the selftest buffer)
  and prove each satisfies the contract; decide whether the seam should assert it or
  whether the doc-comment contract is adequate.
* Width truncation (`ahci.c:456-457,103,466`). `countl/counth` take only the low 16
  bits of the `uint32_t count`; the 8192 cap keeps the wrap unreachable and keeps
  `count*512` from overflowing; the `(bytes-1)` underflow is dead behind the
  `bytes>0` gate. Confirm the cap is the only route to a nonzero `count`.
* User-reachability (`paging.c:434-444`, `pagecache.c:246-294`,
  `kernel.c:1910-1916,1847-1909`). Confirm the `pc_fault` window branch stays
  strictly `!user_mode`-gated (the milestone-9 invariant), that no syscall/VFS path
  calls `block0` directly, and that the `ahci`/`disk` debug commands stay reachable
  only through the kernel debug shell, which is entered on `rach2.rescue` or an
  init-less test image (`kernel.c:1255,2629`), never on a normal `os-real` init
  boot.
* cmdline robustness (`block.c:84-94`, `cmdline.c:10-22`). A truncated or absent
  flag degrades to `ata_init`, never to a NULL `block0`; the fallback cannot leave
  `block0` half-set.
* Watchdog adequacy (`ahci.c:64-67,220-232,480-499`). Every wait loop has a strictly
  decrementing bound that returns `-EIO`; `AHCI_SPIN_LIMIT` is large enough for the
  worst legitimate single-command latency yet finite so a wedged port returns `-EIO`
  rather than hanging the machine inside a fault; every expiry path returns `-EIO`,
  not a silent success, so the fault handler reaches its fail-closed panic.
* Partial-init fallback state (`ahci.c:574-610`, the failure after `ahci_port_start`
  at `:592` before `block0` is published at `:604`; `block.c:90`). `ahci_init`
  publishes `block0` only on full success, so a mid-sequence failure returns -1 and
  `block_probe` falls back to `ata_init`. But the side effects are not undone:
  `GHC.IE` is cleared, every port is stopped, and the selected port may be
  bus-mastering and started before `ahci_identify` fails; `ahci_base`/`ahci_port`
  reset only on the next `ahci_init` entry, not on failure exit. Decide whether an
  HBA left reconfigured and bus-mastering while the system runs on ATA is tolerable,
  or whether a failed `ahci_init` must quiesce before returning.
* Negative boots, by case with the owning diagnostic. No boot disk
  (`kernel.c:1496`), an MBR read failure or a bad signature (`kernel.c:1500,1504`),
  no KRFFS partition among the four entries (`kernel.c:1539`), and a partition whose
  alignment, range, or window fit is bad (`kernel.c:1549,1555,1560`) must each fail
  with its `KRFFS:` diagnostic and fall through cleanly, not hang or silently
  mis-mount.

Gate: a source trace for every failure path, and an actual negative boot for each of
the enumerated MBR/partition cases above, each producing its named `KRFFS:`
diagnostic and a clean fall-through. No robustness finding counts without a trace and
either a reproduction or a tight proof. Codex walks the callers and the
subsequent-command state to kill each failure-mode finding before it counts. Exit
token `AUDIT-FAIL-CLOSED OK`.

## Phase 6: support scripts and gates

Scope: `tools/harness.py`, `rach2/run.sh`, the root `Makefile` AHCI targets,
`rach2/Makefile`, `check_sources.sh`, and the disk manifests. A false gate makes a
green milestone a lie.

Concerns to chase:

* The `--qemu-arg` passthrough (`harness.py:45-49,67-78`). The `--qemu-arg=-device`
  (=value) form is load-bearing (the bare two-token form mis-parses a leading-dash
  value); `cmd += a.qemu_arg` sits after the NIC `-device` and the `-drive` loop so
  `ide-hd` can bind `drv0` and the NIC stays wired; the one-line docstring Usage
  omits the repeatable flags (acceptable, note it).
* The `run.sh` `DISKBUS=ahci` rewrite (`run.sh:62-91`). Re-run the actual gate
  (`shellcheck -x -e SC1091 -e SC2034 -e SC2086 rach2/run.sh`) clean; the legacy
  plain-`DISK=` ATA branch is byte-identical to the pre-milestone script; the drive
  trio ids (`drv0`, `ahci0`, `ahci0.0`) match the Makefile `AHCI_ATTACH`; the
  mount-vs-probe asymmetry (`DISKBUS=ahci` auto-mounts via `rach2.fsdisk`; plain
  `DISK=` probes only) is intentional and the header comment says so honestly.
* The Makefile AHCI targets (`Makefile:234-270`). The three targets use the same
  `64 48` geometry, manifests, timeouts, and tokens as their ATA twins; the
  `AHCI_ATTACH` ids match `run.sh`; `.PHONY` and `help` list all three; the disk
  geometry is triple-sourced (`mkdisk.sh`, `crash_test.sh`, the `64 48` literals) --
  flag the fragility even though it predates this milestone.
* The `--ok` token contract (`harness.py:184-191`). A token-only pass sees the first
  `OK` and exits: a `FAIL` printed before it, or a hang after it, is invisible.
  Confirm each AHCI probe's OK token is unique and printed only on genuine success,
  and that a `FAIL`-then-`OK` or `OK`-then-hang cannot false-pass; decide whether the
  AHCI targets should also assert the absence of the `FAIL` token.
* Shared disk-image filenames (`Makefile:190,203,225`). `test-disk`, `crash-test`,
  and `test-disk-ahci` share `disk-test.disk`; the persist and stress pairs share
  one file each. Each target re-stages first, so sequential runs are safe; decide
  whether `make -j` or interleaved ATA/AHCI runs warrant per-bus image names.
* The `-cpu max` divergence (`harness.py` vs `runtests.py:52`, `run.sh:46`). The
  AHCI targets run under `harness.py`, which passes no `-cpu`, while the rest force
  `-cpu max` for entropy; confirm the disk probes and the `rach2.fsdisk` mount draw
  no randomness so the divergence is harmless, and decide whether the harness should
  default to `-cpu max` for parity.
* Build wiring (`rach2/Makefile:31`, `check_sources.sh`, `pci.h:43-44`). `ahci.c` is
  in `C_SRC` once and covered by `check-sources`; the new pci constants are
  consumed, not dead; `make check-sources` OK.

Gate: `shellcheck` clean over the run scripts, `make check-sources` OK, every AHCI
target re-runs green with its log preserved, and the false-pass cases reasoned
through. Codex replays the command construction and the token false-pass cases.
Exit token `AUDIT-SUPPORT OK`.

## Phase 7: documentation, comments, and style

Scope: `sys/block/Readme.md`, `ahci.h`, the in-body and header comments of
`ahci.c`/`block.c`, `dev-log/10-ahci/{plan,Readme}.md`, `dev-log/Readme.md`,
`tests/Readme.md`, all against the code and the `CLAUDE.md` Writing/Comments
contract. Voice is part of the review and no checker enforces it.

Concerns to chase:

* The two confirmed code-vs-doc contradictions. `ahci.h:36-39` says `ahci_selftest`
  runs IDENTIFY, but the body (`ahci.c:650-669`) only reads LBA 0 (IDENTIFY ran in
  `ahci_init`); rewrite the summary to match. `ahci.c:423-424` reads "it sleeps and
  allocates nothing," which inverts the load-bearing no-sleep contract; recast to
  "it neither sleeps nor allocates." Sweep for the same garbled-conjunction pattern.
* Function-header comment style (`ahci.c`, all 19 definitions, vs `ata.c` and
  `block.c` which use `/**` throughout). `CLAUDE.md` mandates a Doxygen `/**` header;
  `ahci.c` uses plain `/*` on every function, including the two paragraph-form
  headers at `:280` and `:417` that are Doxygen-shaped but for the opening token.
  Decide the rule (promote helpers to `/**`, or accept `/*` for trivial one-liners
  with the public contract in `ahci.h`) and apply it uniformly so `ahci.c` reads like
  `ata.c`.
* In-body comments (`ahci.c:249,272-273,401` and others). Classify each as a one-line
  hardware/ABI fact stated as documentation (keep) or a reasoning trace / restatement
  of the code (remove or fold into the header); `ata.c`'s near-empty bodies are the
  bar. `:249` ("persistent storage for the next skip") and `:401` ("Start: FRE first,
  then ST") are the borderline cases.
* Over-80 lines. Comment overflows are a direct contract miss (`ahci.c:116,552`,
  `ahci.h:11,12,27,36`, `block.c:88`); reflow them. For the code overflows
  (`ahci.c:515,518,645,646`, the 87-col `serial_printf` at `:645`), the block-layer
  norm is 80; wrap to match `ata.c`/`block.c` rather than leave the outliers.
* Diagnostics (`ahci.c` serial lines, `kernel.c:1824,1912`). All lowercase
  `ahci: detail`, matching `ata.c`, not drifting toward the `hda.c` `HDA:`
  Title-Case debt; decide whether the `OK`/`MISMATCH` status tokens are intentional
  test contracts.
* `block/Readme.md` factual fidelity (`:33-50` vs `ahci.c`). Walk each register name,
  opcode (`0x25`/`0x35`/`0xEA`/`0xEC`), constant (the 8192 cap, words 100-103), and
  the `discover->hba_own->quiesce->select->start->identify` order against the code;
  flag any drift. Confirm the "512-byte logical sectors, asserted" claim does not
  overstate what `ahci_identify` actually enforces.
* Voice and style. The dev-log retrospective opens five bullets with bold leads
  (`dev-log/10-ahci/Readme.md:9-17`) against "do not lean on bold"; recast them. Scan
  `block/Readme.md` for any marketing phrasing that asserts merit without a technical
  reason. Confirm the dev-log stays historical (internal consistency, not currency)
  while the Readmes state the current contract.
* `tests/Readme.md` (`:54` and `:106`, both ATA-only). Add the AHCI probe variants
  (`test-disk-ahci`/`persist-test-ahci`/`stress-test-ahci`) and the `--qemu-arg`
  harness form at both anchors.
* Dead code and includes (`ahci.c`). No defined-but-unused macro or `#if`-guarded
  dead block (beyond what `-Werror` already forbids); decide whether `ahci.c` should
  include `lib/kdef.h` directly for `bool` rather than via the transitive `pci.h`.

Gate: `make verify-style` (8/8), and a re-read confirming every edited comment and
Readme describes the code exactly and every contract claim has a code anchor. No
behavior change. Codex reviews the voice and comment accuracy, the part no checker
catches. Exit token `AUDIT-DOCS-STYLE OK`.

## Phase 8: whole-system retirement

Scope: nothing new is read; this is the end-to-end exercise of every contract the
earlier phases reasoned about, plus the audit record and retirement.

Method: run the full gate together: `make verify-style`, `make verify-host`, the
module-path `make test` (28/28), the ATA disk probes, the AHCI disk probes, a
default RAM boot byte-identical to the Phase 1 baseline, and the AHCI KRFFS boot with
its capstones: boot `os-real` from the SATA disk
(`DISK=build/images/os-real.disk DISKBUS=ahci ./run.sh`, confirmed on the AHCI path
by the `ahci: port 0: ... sectors` boot line and the absence of any `ata:` probe),
log in over `ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost` to busybox ash,
run Doom reading `/doom1.wad` off the AHCI mount, and confirm fsck clean after a
clean shutdown. Confirm every candidate finding is either refuted with a trace or
fixed in a scoped commit (or explicitly deferred with a rationale), the vendored
trees are out of the diff, and the audit record is written to `dev-log/11-audit/`
with `dev-log/Readme.md` updated.

Gate: every item green; any regression reopens the owning phase. Codex does a final
pass over the fixed findings and the retirement record. Exit token
`AUDIT-COMPLETE OK`; the milestone is retired.

## Verification

The global gate, run at the end of every behavior-touching phase and once more as a
whole after Phase 8:

* `make verify-style` (8/8) and `make verify-host`.
* the module-path `make test` (28/28), and a default RAM boot byte-identical to the
  baseline.
* the ATA disk probes (`test-disk`, `persist-test`, `stress-test`) and the AHCI disk
  probes (`test-disk-ahci`, `persist-test-ahci`, `stress-test-ahci`).
* the SSH capstone and Doom over the AHCI-backed `os-real` disk (`DISKBUS=ahci`),
  confirmed on the AHCI path by the `ahci: port 0:` boot line, not an `ata:` probe.

The vendored trees stay untouched throughout; a finding that would edit them is out
of scope. The probe tokens, the 28-probe module set, and the byte-identical default
boot are test contracts and are not changed without flagging the change first. Every
phase's Codex re-verification verdict is recorded with its exit token.

## What stays deferred

* The carried-forward backlog from milestone 9: the over-80 kernel comment wraps
  outside the AHCI surface, the tree-wide Title-Case `HDA:` diagnostics, and any
  cleanup that is taste rather than a confirmed defect.
* The real-hardware AHCI path (`GHC.HR` plus a full per-port COMRESET, a low
  cacheable ABAR, a non-active interface power state): untestable under QEMU
  `ich9-ahci` and documented as a known scope limit, not fixed here unless a
  confirmed defect forces it.
* A linter/type gate for `harness.py` is noted, not added.
* Multi-PRDT / non-page-aligned buffers and counts beyond the single-PRDT cap stay
  the documented single-PRDT limit; this audit confirms the cap, it does not lift it.
* P3 cosmetic findings that survive verification but change no behavior may be
  batched into a single cleanup commit rather than blocking a phase gate.

## Baseline

The green state to diff against is `HEAD` at `060e528` ("Document the AHCI block
driver and retire the milestone."), the tip of milestone 10. Phase 1 records it:
`make verify-style` 8/8, `make verify-host`, `make test` 28/28, the ATA and AHCI disk
probes green, a default RAM boot, the SSH capstone, and a Doom run. Every later phase
diffs against this recorded state, so a regression is attributable to the phase that
introduced it, not to the milestone under audit.
