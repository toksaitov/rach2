# dev-log

The record of how each Rach2 milestone was planned and built, one subdirectory per milestone. Milestone 1 keeps the prompts that drove it; later milestones keep the plan they were built from.

* `01-foundation`: the boot, paging, system calls, shell, audio, and Doom of the first OS.
* `02-networking-and-userland`: the TCP/IP stack, the ported musl libc, BusyBox, the Dropbear SSH server, and the build modernization.
* `03-text-standard`: the text and documentation standard, the refactor that reached it, and the gate that keeps it.
* `04-normalization`: carrying that standard into the rest of the in-house tree (the tests, the krffs host tools, the utils, and the ish shell) and widening the gate to match.
* `05-images`: replacing the one overloaded RAM disk with three purpose-built images (a test image, our simple-tools image, and a real busybox/dropbear/Doom environment), built by a manifest-driven stager, plus a `make test` boot check and a bootable ISO per image.
* `06-review`: a full code, comment, documentation, support-file, and manual-test audit of the whole system, every finding refuted before it counted, the safe ones fixed in scoped commits, and the gate, boots, and capstone re-verified against a recorded baseline.
* `07-init`: a persistent userland init (`/sbin/init`) that reaps orphans and respawns the login shell, the kernel orphan-reparenting that fix needs, and a `rach2.rescue` boot flag that turns the kernel debug shell into a deliberate rescue mode instead of the default home.
* `08-disk`: Path B, lifting KRFFS off the RAM-disk module onto a real ATA disk through a kernel page cache and a demand-paged kernel-virtual window (the fault polls the sectors in rather than sleeping, since it runs with interrupts off), with write-back, a real fsync, a dirty-mount flag, and a GRUB boot from the disk.
* `09-audit`: a full four-dimension audit (code, comments, documentation, support files) of the whole tree after the disk milestone, run as ten risk-ordered multi-agent phases that adversarially verified every candidate finding; it found and fixed one real defect (a user read of the page-cache window panicked the kernel, now a clean segfault) and brought the comments and Readmes back into agreement with the disk-backed system.
* `10-ahci`: the modern SATA half of the disk layer, an AHCI driver published behind the same `block0` the ATA driver fills, driven by polled `PxCI` completion so it stays callable from the interrupt-off page-cache fault, selected by a `rach2.disk=ahci` probe seam that leaves the legacy IDE boot byte-for-byte unchanged; the disk probes and the SSH and Doom capstones all run over a SATA disk.
