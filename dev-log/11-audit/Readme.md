# Milestone 11: the post-AHCI audit

The AHCI milestone added 927 lines across fifteen files: a 669-line SATA driver,
the block-layer seam that lets two drivers publish `block0`, three `kernel.c`
integration points, and the AHCI test and run wiring. This audit proves that new
code correct, proves the unchanged code unbroken by the ripple, and brings every
comment, Readme, and support file into agreement with the code and the `CLAUDE.md`
style contract. It is the post-AHCI counterpart of `dev-log/09-audit`, run in the
same discipline: every candidate finding is adversarially verified before it
counts, most are refuted, and Codex independently re-verifies each phase.

## The method

Eight risk-ordered phases, each a Workflow fan-out the maintainer reviewed at its
gate: parallel auditors read one concern cluster each, a separate adversarial
verifier tried to refute every candidate finding, and then Codex re-read the
phase independently. The bookend phases (baseline, retirement) ran the gate; the
six middle phases each carried a named exit token. The plan, built with Codex from
a six-dimension survey of the surface, lives in `plan.md`.

The Codex re-verification earned its place. Twice it changed the outcome: in the
HBA phase it promoted a finding the fan-out had refuted (a low ABAR mapped through
the cacheable identity map), and in the command-path phase it found a second
defect the fan-out missed (a device-fault status the completion check ignored).
An audit that had trusted the fan-out alone would have shipped both.

## What it found

Eleven real defects across the eight phases, four of them surfaced by Codex's
independent re-verification, two of those on its final pass over the audit's own
fixes. The pattern worth noting: the highest severity hid where QEMU cannot reach,
on the error paths the green probes never exercise.

* The command path returned false success on two hardware-only error conditions.
  The completion poll watched only the task-file error bit (`PxIS.TFES`), so a
  host-bus or interface fatal error that cleared `PxCI` without setting the task
  file passed the post-loop check and reported a corrupt transfer as a success;
  and the post-loop check tested the task-file `ERR` bit but not `DF` (device
  fault), so a `DF`-only completion also read as success. Both are silent data
  corruption on real hardware, invisible on QEMU. The fix watches the full fatal
  `PxIS` set in the loop and again after it clears, and tests `ERR | DF`. The same
  phase added port recovery (AHCI 6.2.2: stop the engine, clear the latched
  errors, restart it) so a failed command no longer leaves the port wedged until a
  fresh `ahci_init`.
* A low ABAR was mapped through the cacheable identity map. The bring-up mapped the
  ABAR cache-disabled only when it sat above the 32 MiB identity-mapped limit; a
  controller that placed its ABAR low (legal, though not what QEMU does) would have
  its status registers polled through a cacheable mapping, reading stale values.
  The map is now unconditional.
* The debug commands re-probed a live disk. Typing `disk`, `disk write`, or `ahci`
  at the rescue shell re-ran the full bring-up, which quiesces every port, even
  when KRFFS was mounted from the AHCI disk: the re-probe stopped the very port the
  page cache was using, and a partway failure left it stopped under a live mount.
  The three commands now refuse while a disk mount is live.
* The kernel command line could match a torn token. A command line longer than the
  256-byte capture buffer was truncated mid-token, and a partial trailing token
  could spuriously match a flag (`rach2.disk=ahcix` truncated to `rach2.disk=ahci`).
  The capture now drops the torn token.
* Two gate-correctness defects in the support scripts. The source-list check
  printed `OK` and exited zero if `mktemp` failed, so a constrained environment
  could pass the gate without comparing anything; it now fails closed. And the ATA
  and AHCI disk tests shared image filenames, so a `make -j` run of a target and
  its AHCI twin would race on one file; the AHCI variants now use their own images.

The partial-init state a failed probe leaves behind was investigated and judged
benign (an idle started port issues no DMA without a queued command, and the HBA's
MMIO does not overlap the legacy ATA ports), and documented rather than changed.
The DMA buffer trust boundary, the bounds guards, the `--ok` token gate, and the
`!user_mode` window gate were all confirmed clean by both the fan-out and Codex.

The documentation and style phase corrected two comments that contradicted the
code (the self-test no longer claims an IDENTIFY it does not do; the command issuer
no longer says it "sleeps"), brought every AHCI function header to the Doxygen
`/**` form the sibling ATA driver uses, updated the block Readme to the audited
error model, de-bolded the dev-log, documented the AHCI test variants, and removed
a stale include and a dead macro.

Codex's final pass over the cumulative change found three more, two of them in the
audit's own fixes. The new error-recovery helper would have restarted a command
engine that refused to stop; it now fails closed. The driver never validated the
device's logical sector size, though the comment and Readme claimed it did; it now
rejects a non-512-byte sector. The third gave the crash test its own disk image,
closing the last shared-image race. That the final reader caught a bug in the
audit's own fix is the argument for keeping one.

## Verification

The global gate held at every phase and once more at close: `make verify-style`
8/8, `make verify-host`, the module-path `make test` at 28/28, and `make
check-sources`. The ATA disk probes (`test-disk`, `persist-test`, `stress-test`)
and the AHCI disk probes (`test-disk-ahci`, `persist-test-ahci`,
`stress-test-ahci`) pass their tokens, and the SSH and Doom capstones run over the
AHCI-backed disk. The default RAM-module boot and the legacy `if=ide` ATA boot stay
byte-identical: every fix is fail-closed or text-only, and AHCI stays dormant
unless `rach2.disk=ahci` selects it. The vendored trees stayed out of the diff
throughout.
