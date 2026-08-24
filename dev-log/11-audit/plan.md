# Milestone 11: the post-AHCI audit

Status: complete. All eight phases and their checks were completed; eleven defects were fixed and the other candidate findings were refuted. See `Readme.md` for the milestone record.

## The goal

The AHCI milestone added 927 lines across fifteen files: a new 669-line SATA driver (`sys/block/ahci.c`), the block-layer interface that lets two drivers publish `block0` (`block.c`/`block.h`/`ata.c`), three `kernel.c` integration points, two PCI constants, the `sys/block/Readme.md` contract document, and the AHCI test and run support (`harness.py`, `run.sh`, the root `Makefile`). Nothing above `block0` changed, and the default RAM and legacy IDE boots are intended to remain byte-identical. This audit checks the new driver under the polled IF=0 model, checks that the block-layer change did not alter the old boot contract, and brings each comment, Readme, and support file into agreement with the code and the `CLAUDE.md` style contract.

It is the post-AHCI counterpart of `dev-log/09-audit` and uses the same method. An adversarial review must confirm each candidate finding, most candidates are expected to be refuted, and Codex independently checks each phase's findings before they become fixes. The changed code is small enough for eight focused phases rather than ten.

## Audit scope

The AHCI milestone (`e79c781..060e528`) added or changed:

* `sys/block/ahci.c` (NEW, 669 lines): PCI discovery and ABAR mapping, HBA ownership (BIOS handoff, `GHC.AE` on / `GHC.IE` off, the deliberately skipped `GHC.HR`), all-port quiescence, first-present-port initialization, the polled command path (`ahci_run_command`: H2D FIS + single PRDT + `PxCI`/`PxIS`/`PxTFD` poll), `ahci_identify`, `ahci_read`/`ahci_write`/`ahci_flush`, the cache-sync helper, and the `ahci_dump`/`ahci_selftest` debug commands.
* `sys/block/ahci.h` (NEW): the Doxygen contract for the three public entry points.
* `sys/block/block.c`: the `block0` definition (moved out of `ata.c`) and `block_probe` (the `rach2.disk=ahci` selection point with an ATA fallback).
* `sys/block/block.h`: the `block_probe` declaration; `sys/block/ata.c`: lost its `block0` definition, otherwise unchanged.
* `kernel.c`: three `ata_init`->`block_probe` sites (`init_krffs_disk`, the `disk` and `disk write` debug commands), the `ahci` debug command, and the neutralized diagnostics.
* `sys/pci.h`: `PCI_CLASS_MASS_STORAGE 0x01`, `PCI_SUBCLASS_SATA 0x06`.
* `sys/block/Readme.md` (NEW), `dev-log/10-ahci/{plan,Readme}.md`, `dev-log/Readme.md`: the documentation.
* `tools/harness.py` (`--qemu-arg`), `rach2/run.sh` (`DISKBUS=ahci`), the root `Makefile` (`test-disk-ahci`/`persist-test-ahci`/`stress-test-ahci`), and `rach2/Makefile` (`ahci.c` in `C_SRC`): the test and run wiring.

The audit protects these invariants and checks their documentation:

* The I/O path polls and never sleeps. `ahci_read`/`ahci_write` run inside the page-cache window fault at IF=0: no `sti`, `hlt`, wait queue, allocation, or unbounded spin on the path, and every wait is an iteration watchdog that returns `-EIO` on expiry.
* The page-cache window stays the only route from ring 3 to `block0->read`, and it stays strictly `!user_mode`-gated (the milestone-9 fix). No user path reaches the AHCI code directly; the `ahci`/`disk` debug commands stay rescue-gated.
* The default RAM boot and the legacy `if=ide` ATA boot are byte-identical. AHCI remains inactive unless `rach2.disk=ahci` selects it, and `block_probe` then falls back to ATA if no HBA answers.
* Every DMA buffer that reaches the PRDT is identity-mapped and below 4 GiB, so the truncated 32-bit `dba` with `dbau=0` names the right physical page.
* Bus mastering is enabled only after the selected port's DMA bases are programmed, and `GHC.IE` is cleared before it.

## Scope and non-goals

In scope are the changed files above, read for logic and correctness, security and robustness, integration and regression, support scripts and checks, documentation, and style. The audit also covers the integration points affected by the block-layer change (`pc_fault`/`pc_set_backing`, `init_krffs_disk`, `cmdline.c`). The vendored trees (`musl`, `busybox`, `dropbear`, `krffs/vendor`) are out of scope and remain untouched. Subsystems that the AHCI milestone did not change are not re-audited because milestone 9 already covered the tree. This audit fixes only findings confirmed by adversarial review. It does not refactor for preference or change a test contract (the probe tokens, 28-probe module set, or byte-identical default boot) without identifying that change first.

## Method, common to every phase

Each phase uses one parallel Workflow audit that `/goal` runs to completion, followed by maintainer review. Codex participates in every phase:

* Run parallel auditors, one per file or group of concerns. Give each auditor the concrete concerns below with their `file:line` anchors. An auditor reads but does not fix.
* Give every candidate finding to a separate adversarial verifier. The verifier reads the surrounding code, callers, and affected invariant, then either refutes the finding or confirms it with a reproduction or precise argument. A finding does not count until it survives this second review.
* Codex independently checks the phase. It reads the confirmed findings against the code and specification, then confirms or refutes each one. It rejects the phase if the parallel audit missed a concern in a changed contract file. Record the Codex result with the phase token.
* Confirmed findings become scoped fixes, one logical change each, committed in the house one-line style with no attribution. A text-only fix changes no behavior and no built artifact. Comments and docs are fixed in the phase that owns them.
* Each phase ends with its required checks and exit token. The maintainer reviews the result before `/goal` proceeds.

Every phase examines security, with a combined security review in Phase 5. Each file receives a style and documentation review, followed by the combined review in Phase 7.

## Phase 1: baseline and map

Scope: establish the passing state used for later comparisons and confirm that the audit map covers every changed contract file. Nothing is fixed here.

Work:

* Record the baseline: `make verify-style` (8/8), `make verify-host`, the module-path `make test` (28/28), the ATA disk probes (`test-disk`, `persist-test`, `stress-test`), the AHCI disk probes (`test-disk-ahci`, `persist-test-ahci`, `stress-test-ahci`), a default RAM boot, an SSH end-to-end test, and a Doom run. Capture the exact tokens and counts.
* Confirm the changed-file map (`git diff --stat e79c781~1..HEAD`, 15 files) is fully covered by Phases 2-7, with no changed contract file unowned.

Verification: every baseline check passes and is recorded; the map accounts for all 15 files. The disk-probe and AHCI-probe tokens remain provisional until Phase 6 checks the `--ok` behavior. `harness.py` returns 0 on token presence alone, so a `FAIL` followed by `OK`, or a hang after `OK`, would appear to pass at this point. Codex reads the changed-file map and rejects the plan if any changed contract file is unowned. Exit token `AUDIT-BASELINE OK`.

## Phase 2: the HBA, PCI, MMIO, and DMA ownership

Scope: `ahci_discover`, `ahci_hba_own`, `ahci_port_stop`/`ahci_quiesce_ports`, `ahci_select_port`, `ahci_port_start`, and the `ahci_init` ordering. These functions contain the main stale-DMA, incorrect-MMIO, and interrupt-leak risks during initialization.

Checks:

* Bring-up ordering (`ahci.c:574-610` init order, `:288-301`, `:379-405`). The `GHC.AE`-on / `GHC.IE`-off must be a single read-modify-write that cannot momentarily enable IE; `pci_enable_bus_master` must be strictly after `PxCLB`/`PxCLBU`/`PxFB`/`PxFBU` are written and before `PxCMD.ST`; FRE must precede ST; and a quiesce that fails on any port must abort init rather than proceed with a half-quiesced HBA.
* Discovery and ABAR (`ahci.c:235-278`, `pci.c:345-364`, `paging.c:471-512`). The BAR5 checks must reject a 64-bit BAR4 spill, a non-memory BAR5, and a zero BAR5. `map_mmio` runs only when `abar >= HARD_MEMORY_LIMIT`, leaving a possible cacheable-MMIO defect if a controller places the ABAR below that limit. Memory-space decoding must be enabled before `ahci_hba_own` reads `CAP2`/`GHC`; `map_mmio` failure must abort; and `AHCI_ABAR_MAP_SIZE` (0x2000) must cover the highest port register read.
* Port selection (`ahci.c:350-376`). The code selects only on `DET==3 && IPM==1`, although the plan also required `PxTFD` not BSY/DRQ. Decide whether the pre-issue task-file-idle wait in `ahci_run_command` makes the selection check sufficient, and whether requiring `IPM==1` incorrectly rejects a present device in a non-active interface power state on hardware. QEMU reports the active state.
* The deliberate `GHC.HR` skip (`ahci.c:280-287`). Re-verify against the documented QEMU `PxTFD 0x50->0x130` behavior that skipping HR plus the all-port quiesce leaves no stale `PxIS`/`PxSERR`/`PxCI` from a prior owner.
* Partial-init effects after a failed bring-up. A failure after `ahci_port_start` leaves the HBA bus-mastering with the port started, then `block_probe` falls back to ATA. Phase 5 owns the question of whether that leftover state is tolerable; Phase 2 only confirms the ordering that produces it (bus mastering enabled inside `ahci_port_start`).

Verification: trace the source to confirm that no path enables `PxIE`/`GHC.IE` or calls `pci_enable_interrupts`, and that bus mastering starts only after the DMA bases are set. The `ahci` debug command must report `ahci: selftest: MBR signature OK`. Codex checks each ownership and DMA finding, with particular attention to stale-port DMA and the BAR-spill check. Exit token `AUDIT-HBA-DMA OK`.

## Phase 3: the polled command path

Scope: `ahci_run_command`, the packed FIS/PRDT/header overlays, `ahci_identify`, `ahci_read`/`ahci_write`/`ahci_flush`, and `ahci_dma_sync`. A missed status bit can silently corrupt data or stop command processing.

Checks:

* FIS/PRDT/header construction (`ahci.c:417-478`, `:117-144`). Verify every field against the SATA H2D Register FIS and AHCI command-header layout: `fis_type=0x27`, C bit, command byte, the 48-bit LBA spread `lba0..lba5`, `countl/counth`, and `device=0x40`; the header `dw0` with `CFL=5` dwords, the W bit, and `PRDTL=1`; and the PRDT `dbc=(bytes-1)` masked to 22 bits. Confirm that the packed overlays have no padding and that the volatile writes match the values read by HBA DMA.
* Completion logic (`ahci.c:480-507`, `:220-232`). The loop spins on `PxCI` while checking `PxIS.TFES`, then checks `PxTFD.ERR`. `TFES` is not the only fatal status. The host-bus and interface error bits (`PxIS.HBFS`, `HBDS`, `IFS`) are not checked, so a non-task-file DMA fault reaches the watchdog instead of failing immediately. The post-error path also does not clear `PxIS`/`PxSERR` or restart the engine, leaving the next command after a `TFES` error unverified. Decide whether to check the fatal bits and whether an error is terminal or recoverable, then document the choice.
* `bytes` vs `count` coupling (`ahci.c:428-467`, `:539-570`, `:103`). `ahci_run_command` takes `count` (drives the FIS sector count) and `bytes` (drives the PRDT) independently, with no internal assertion that `bytes == count*512`; the `AHCI_MAX_SECTORS_PER_CMD=8192` cap is enforced in `ahci_read`/`ahci_write`, not in `ahci_run_command`. Confirm the only callers always pass a consistent pair, and that the cap exactly matches the single-PRDT 4 MiB reach.
* The descriptor flush span (`ahci.c:199-218`, `:474-478`, `:504-506`). The command-list header is flushed for `sizeof(ahci_cmd_header_t)=16` bytes, but the HBA reads 32 bytes per slot. The upper 16 bytes are zero and unused, so confirm that 16 bytes suffice. The command-table flush covers all 256 bytes, the post-read invalidation must include the trailing partial cache line, and `WBINVD` at IF=0 is safe but expensive.
* IDENTIFY (`ahci.c:510-529`). Words 100-103 are the LBA48 user-addressable count, assembled little-endian, read from a DMA buffer (so no byte-swap, matching the ATA PIO data path); the zero-count rejection fails a non-LBA48 device. Confirm `idbuf` is a valid single-PRDT target.
* No-data path (`ahci.c:439`, `:462-467`, `:531-536`). The PRDT is built only when `bytes > 0`, so FLUSH (`bytes=0`, `PRDTL=0`) and IDENTIFY (`bytes=512, count=0`) are both correct; `ahci_zero` clears the full command table so no stale PRDT leaks; `ahci_write` must propagate a failed `ahci_flush`'s `-EIO`.
* The freestanding 64-bit shifts (`ahci.c` LBA `>>32`/`>>40`). Use `nm`/`objdump` to confirm again that the object built under `-nodefaultlibs` has no libgcc helper (`__lshrdi3`/`__ashrdi3`/`__udivdi3`). Codex confirmed this during the milestone; the audit checks the committed object again.

Verification: `make test-disk-ahci` reports `BLOCKPROBE OK`, `make persist-test-ahci` reports both `PERSISTPROBE OK` tokens, `make stress-test-ahci` reports `STRESSPROBE OK`, and the object inspection passes. Codex checks each confirmed finding against the AHCI layout, ATA command semantics, and generated object. Exit token `AUDIT-COMMAND OK`.

## Phase 4: the block interface and KRFFS integration

Scope: the `block0` move and `block_probe` (`block.c`), the three `kernel.c` call sites and the `ahci`/`disk`/`disk write` debug commands, `pci.h`, and the `init_krffs_disk` mount path. One new driver must not disturb the old boot contract.

Checks:

* `block0` linkage and joint ownership (`block.c:9`, `block.h:33`, `ata.c:276,332`, `ahci.c:572,604`). Exactly one definition, both drivers assigning it; `ahci_init` leaves `block0` untouched on every `-1` path (relying on `block_probe`'s short-circuit plus `ata_init`'s reset); `ata_device`/`ahci_device` are static so a captured `block0` (`_krffs_disk_part.backing`) never dangles.
* `block_probe` policy (`block.c:84-94`, `cmdline.c:24-52`). Whole-token matching so `rach2.disk=ahci` cannot collide with `rach2.fsdisk` (test `rach2.disk=ahcix`, a prefix, a neighbor); the ATA fallback reached only when `ahci_init` returns nonzero; `ahci_init` returns 0 only when `block0` is set.
* Default preservation (`ata.c:271-337`, `run.sh:67-84`). Diff `ata.c` to confirm the only change is the removed `block0` definition; the no-`DISKBUS` `run.sh` path still emits `if=ide,index=0,media=disk` and appends no `rach2.disk=ahci`; the ATAPI/SATA-signature refusal (`ata.c:301-304`) still keeps ATA off an AHCI disk.
* Re-probe over a live mount (`kernel.c:1850,1882,1910-1916`, `ahci.c:331-348`, `kernel.c:1567`). The debug `disk`/`disk write`/`ahci` commands re-run the full bring-up, which quiesces and restarts the very port the page cache may be using under a mounted KRFFS. Trace whether this disturbs an in-flight cache, leaves the port cleanly restarted, or risks a wedge; decide whether the commands should refuse to re-probe a mounted disk, or whether the contract docs should mark re-probe destructive (mirroring the existing `disk write` last-sector note).
* Call sites and messages (`kernel.c:1496-1499,1850-1858,1882-1896,1824,1910-1916`). All three keep the `!= 0 || !block0` guard; the neutralized messages (`disk: block0: ... sectors`, `disk: no boot disk`) obey `component: detail` lowercase and do not imply ATA when AHCI is underneath.
* `pci.h` additions (`pci.h:43-44`, `ahci.c:235-278`). The two constants are correct and consumed by the class/subclass/prog-if filter, do not collide with the audio constants, and AHCI discovery works with `init_pci` never called.
* Partition arithmetic (`kernel.c:1544-1568`). Check the `part_bytes` multiplication and size-versus-window-cap handling after the MBR probe for overflow and for a partition that the host can create but the kernel rejects only during mounting.

Verification: the default RAM boot is byte-identical, `make test` passes 28/28, and both ATA and AHCI disk probes pass. Codex checks the integration findings, with particular attention to the debug re-probe over a mounted disk. Exit token:

```
AUDIT-BLOCK-SEAM OK
```

## Phase 5: failure modes, bounds, and security

Scope: examine the AHCI driver's robustness and security boundary together. The success probes exercise completed commands; this phase examines failure paths and the trust boundary that no probe covers.

Checks:

* The range-check boundaries (`ahci.c:542-545,558-561`, compared with `ata.c:182-186`, `block.c:24-27`). Verify that `count==0 || lba > sector_count || count > sector_count - lba || count > cap` avoids underflow for every boundary: `lba==sector_count`, `count==sector_count-lba` (an exact final read), `count==cap`, and a very large device-reported `sector_count`. The `>` rather than `>=` comparison is required and must match the ATA and partition implementations.
* The DMA-buffer trust boundary (`ahci.c:428-508,539-570,513-529`). The PRDT `dba` is a truncated 32-bit address with `dbau=0`; nothing checks whether the buffer is below `HARD_MEMORY_LIMIT`, identity-mapped, contiguous, or non-NULL. Enumerate every caller: page-cache frames from `alloc_kernel_frame`, the `init_krffs_disk` MBR and partition buffers, debug-command buffers, `idbuf`, and the selftest buffer. Verify that each caller meets the contract, then decide whether the interface should assert the condition or a documentation contract is sufficient.
* Width truncation (`ahci.c:456-457,103,466`). `countl/counth` take only the low 16 bits of the `uint32_t count`. The 8192 cap prevents wrapping and keeps `count*512` from overflowing. The `bytes>0` condition prevents `(bytes-1)` from underflowing. Confirm that the cap applies to every route with a nonzero `count`.
* User reachability (`paging.c:434-444`, `pagecache.c:246-294`, `kernel.c:1910-1916,1847-1909`). Confirm that the `pc_fault` window branch remains restricted by `!user_mode`, as required by milestone 9; no syscall or VFS path calls `block0` directly; and the `ahci`/`disk` debug commands remain reachable only through the kernel debug shell. That shell is entered with `rach2.rescue` or an init-less test image (`kernel.c:1255,2629`), never during a normal `os-real` init boot.
* cmdline robustness (`block.c:84-94`, `cmdline.c:10-22`). A truncated or absent flag degrades to `ata_init`, never to a NULL `block0`; the fallback cannot leave `block0` half-set.
* Watchdog adequacy (`ahci.c:64-67,220-232,480-499`). Every wait loop has a strictly decreasing bound and returns `-EIO` on expiration. `AHCI_SPIN_LIMIT` must allow the longest valid single command but remain finite, so a wedged port returns `-EIO` instead of hanging the machine during a fault. Every expiration path must return `-EIO` instead of success, causing the fault handler to panic.
* Partial-init fallback state (`ahci.c:574-610`, the failure after `ahci_port_start` at `:592` before `block0` is published at `:604`; `block.c:90`). `ahci_init` publishes `block0` only on full success, so a mid-sequence failure returns -1 and `block_probe` falls back to `ata_init`. But the side effects are not undone: `GHC.IE` is cleared, every port is stopped, and the selected port may be bus-mastering and started before `ahci_identify` fails; `ahci_base`/`ahci_port` reset only on the next `ahci_init` entry, not on failure exit. Decide whether an HBA left reconfigured and bus-mastering while the system runs on ATA is tolerable, or whether a failed `ahci_init` must quiesce before returning.
* Negative boots, by case with the owning diagnostic. No boot disk (`kernel.c:1496`), an MBR read failure or a bad signature (`kernel.c:1500,1504`), no KRFFS partition among the four entries (`kernel.c:1539`), and a partition whose alignment, range, or window fit is bad (`kernel.c:1549,1555,1560`) must each fail with its `KRFFS:` diagnostic and fall through cleanly, not hang or silently mis-mount.

Verification: trace every failure path and perform a negative boot for each MBR and partition case above. Each boot must produce its named `KRFFS:` diagnostic and continue cleanly. A robustness finding requires a trace and either a reproduction or a precise proof. Codex examines the callers and the state of the following command to refute each unsupported failure-mode finding. Exit token `AUDIT-FAIL-CLOSED OK`.

## Phase 6: support scripts and checks

Scope: `tools/harness.py`, `rach2/run.sh`, the root `Makefile` AHCI targets, `rach2/Makefile`, `check_sources.sh`, and the disk manifests. A faulty check can accept an invalid milestone.

Checks:

* The `--qemu-arg` passthrough (`harness.py:45-49,67-78`). The `--qemu-arg=-device` (=value) form is required because the bare two-token form misparses a value that begins with a dash. `cmd += a.qemu_arg` follows the NIC `-device` and the `-drive` loop, allowing `ide-hd` to bind `drv0` while retaining the NIC. The one-line Usage docstring omits the repeatable flags; record that accepted omission.
* The `run.sh` `DISKBUS=ahci` rewrite (`run.sh:62-91`). Run the required check (`shellcheck -x -e SC1091 -e SC2034 -e SC2086 rach2/run.sh`). The legacy plain-`DISK=` ATA branch must remain byte-identical to the pre-milestone script; the drive identifiers (`drv0`, `ahci0`, `ahci0.0`) must match the Makefile `AHCI_ATTACH`; and the header comment must accurately describe the intentional mount-versus-probe difference. `DISKBUS=ahci` mounts automatically through `rach2.fsdisk`, while plain `DISK=` only probes.
* The Makefile AHCI targets (`Makefile:234-270`). The three targets use the same `64 48` geometry, manifests, timeouts, and tokens as their ATA counterparts; the `AHCI_ATTACH` identifiers match `run.sh`; and `.PHONY` and `help` list all three. The disk geometry is repeated in `mkdisk.sh`, `crash_test.sh`, and the `64 48` literals. Record this maintenance risk even though it predates the milestone.
* The `--ok` token contract (`harness.py:184-191`). A token-only pass sees the first `OK` and exits: a `FAIL` printed before it, or a hang after it, is invisible. Confirm each AHCI probe's OK token is unique and printed only on genuine success, and that a `FAIL`-then-`OK` or `OK`-then-hang cannot false-pass; decide whether the AHCI targets should also assert the absence of the `FAIL` token.
* Shared disk-image filenames (`Makefile:190,203,225`). `test-disk`, `crash-test`, and `test-disk-ahci` share `disk-test.disk`; the persist and stress pairs share one file each. Each target re-stages first, so sequential runs are safe; decide whether `make -j` or interleaved ATA/AHCI runs warrant per-bus image names.
* The `-cpu max` divergence (`harness.py` vs `runtests.py:52`, `run.sh:46`). The AHCI targets run under `harness.py`, which passes no `-cpu`, while the rest force `-cpu max` for entropy; confirm the disk probes and the `rach2.fsdisk` mount draw no randomness so the divergence is harmless, and decide whether the harness should default to `-cpu max` for parity.
* Build wiring (`rach2/Makefile:31`, `check_sources.sh`, `pci.h:43-44`). `ahci.c` is in `C_SRC` once and covered by `check-sources`; the new pci constants are consumed, not dead; `make check-sources` OK.

Verification: `shellcheck` passes for the run scripts, `make check-sources` reports OK, every AHCI target passes again with its log preserved, and each false-pass case is analyzed. Codex reconstructs the command and checks the token false-pass cases. Exit token `AUDIT-SUPPORT OK`.

## Phase 7: documentation, comments, and style

Scope: `sys/block/Readme.md`, `ahci.h`, the in-body and header comments of `ahci.c`/`block.c`, `dev-log/10-ahci/{plan,Readme}.md`, `dev-log/Readme.md`, `tests/Readme.md`, all against the code and the `CLAUDE.md` Writing/Comments contract. Voice is part of the review and no checker enforces it.

Checks:

* The two confirmed contradictions between code and documentation. `ahci.h:36-39` says `ahci_selftest` runs IDENTIFY, but the body (`ahci.c:650-669`) only reads LBA 0; IDENTIFY ran in `ahci_init`. Rewrite the summary to match. `ahci.c:423-424` reads "it sleeps and allocates nothing," which contradicts the required no-sleep contract. Change it to "it neither sleeps nor allocates." Check for the same incorrect conjunction elsewhere.
* Function-header comment style (`ahci.c`, all 19 definitions, compared with `ata.c` and `block.c`, which use `/**` throughout). `CLAUDE.md` requires a Doxygen `/**` header, but `ahci.c` uses plain `/*` on every function. This includes the paragraph-form headers at `:280` and `:417`, which differ from Doxygen only in the opening token. Decide whether helpers receive `/**` or trivial one-line functions may use `/*` with the public contract in `ahci.h`, then apply that decision consistently so `ahci.c` matches `ata.c`.
* In-body comments (`ahci.c:249,272-273,401` and others). Keep a one-line hardware or ABI fact stated as documentation. Remove a reasoning trace or restatement of the code, or move it into the function header. Use the sparse bodies in `ata.c` as the comparison. Review `:249` ("persistent storage for the next skip") and `:401` ("Start: FRE first, then ST") carefully.
* Lines longer than 80 columns. Reflow comments that violate the contract (`ahci.c:116,552`, `ahci.h:11,12,27,36`, `block.c:88`). Wrap the long code lines (`ahci.c:515,518,645,646`, including the 87-column `serial_printf` at `:645`) to match the 80-column convention in `ata.c` and `block.c`.
* Diagnostics (`ahci.c` serial lines, `kernel.c:1824,1912`). Keep lowercase `ahci: detail`, matching `ata.c`, rather than copying the inconsistent `HDA:` capitalization in `hda.c`. Decide whether the `OK`/`MISMATCH` status tokens are intentional test contracts.
* `block/Readme.md` factual accuracy (`:33-50` compared with `ahci.c`). Compare each register name, opcode (`0x25`/`0x35`/`0xEA`/`0xEC`), constant (the 8192 cap, words 100-103), and the `discover->hba_own->quiesce->select->start->identify` order with the code, and record any mismatch. Confirm that the "512-byte logical sectors, asserted" claim does not overstate what `ahci_identify` enforces.
* Voice and style. The dev-log retrospective begins five bullets with bold labels (`dev-log/10-ahci/Readme.md:9-17`), contrary to "do not lean on bold"; rewrite them. Check `block/Readme.md` for claims of quality without a technical reason. Keep the dev-log historical and internally consistent while the Readmes state the current contract.
* `tests/Readme.md` (`:54` and `:106`, both ATA-only). Add the AHCI probe variants (`test-disk-ahci`/`persist-test-ahci`/`stress-test-ahci`) and the `--qemu-arg` harness form at both anchors.
* Dead code and includes (`ahci.c`). No defined-but-unused macro or `#if`-guarded dead block (beyond what `-Werror` already forbids); decide whether `ahci.c` should include `lib/kdef.h` directly for `bool` rather than via the transitive `pci.h`.

Verification: `make verify-style` passes 8/8. Read each edited comment and Readme again to confirm that it describes the code exactly and that each contract claim has a code anchor. This phase changes no behavior. Codex reviews prose and comment accuracy, which the automated check does not decide. Exit token `AUDIT-DOCS-STYLE OK`.

## Phase 8: whole-system completion

Scope: read nothing new. Exercise every contract examined by the earlier phases, then complete the audit record.

Method: run all required checks together: `make verify-style`, `make verify-host`, the module-path `make test` (28/28), the ATA disk probes, the AHCI disk probes, a default RAM boot byte-identical to the Phase 1 baseline, and an AHCI KRFFS boot with its end-to-end tests. Boot `os-real` from the SATA disk with `DISK=build/images/os-real.disk DISKBUS=ahci ./run.sh`. Confirm the AHCI path from the `ahci: port 0: ... sectors` boot line and the absence of an `ata:` probe. Log in to BusyBox ash through `ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost`, run Doom while it reads `/doom1.wad` from the AHCI mount, and confirm that fsck reports a clean file system after a clean shutdown. Every candidate finding must be refuted with a trace, fixed in a scoped commit, or explicitly deferred with a reason. Confirm that vendored trees remain outside the diff, write the audit record under `dev-log/11-audit/`, and update `dev-log/Readme.md`.

Verification: every item passes. A regression reopens the phase that owns it. Codex performs a final review of the fixed findings and completion record. Exit token `AUDIT-COMPLETE OK`; the milestone is complete.

## Verification

Run the complete check set at the end of every phase that changes behavior and once more after Phase 8:

* `make verify-style` (8/8) and `make verify-host`.
* the module-path `make test` (28/28), and a default RAM boot byte-identical to the baseline.
* the ATA disk probes (`test-disk`, `persist-test`, `stress-test`) and the AHCI disk probes (`test-disk-ahci`, `persist-test-ahci`, `stress-test-ahci`).
* the SSH end-to-end test and Doom over the AHCI-backed `os-real` disk (`DISKBUS=ahci`), confirmed on the AHCI path by the `ahci: port 0:` boot line rather than an `ata:` probe.

The vendored trees stay untouched throughout; a finding that would edit them is out of scope. The probe tokens, the 28-probe module set, and the byte-identical default boot are test contracts and are not changed without documenting the change first. Every phase's Codex review result is recorded with its exit token.

## What stays deferred

* The backlog from milestone 9: kernel comments longer than 80 columns outside the AHCI files, the tree-wide Title-Case `HDA:` diagnostics, and cleanup based on preference rather than a confirmed defect.
* The hardware AHCI path (`GHC.HR` plus a full per-port COMRESET, a low cacheable ABAR, and a non-active interface power state): untestable under QEMU `ich9-ahci` and documented as a known scope limit. It is not fixed here unless a confirmed defect requires a change.
* A linter or type check for `harness.py` is recorded but not added.
* Multi-PRDT / non-page-aligned buffers and counts beyond the single-PRDT cap stay the documented single-PRDT limit; this audit confirms the cap, it does not lift it.
* P3 cosmetic findings that survive verification but change no behavior may share one cleanup commit rather than blocking a phase.

## Baseline

The comparison baseline is `HEAD` at `060e528` ("Document the AHCI block driver and retire the milestone."), the tip of milestone 10. Phase 1 records `make verify-style` at 8/8, `make verify-host`, `make test` at 28/28, passing ATA and AHCI disk probes, a default RAM boot, an SSH end-to-end test, and a Doom run. Every later phase compares its result with this recorded state, so a regression belongs to the phase that introduced it rather than the milestone under audit.
