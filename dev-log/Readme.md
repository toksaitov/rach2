# dev-log

The record of how each Rach2 milestone was planned and built, one subdirectory per milestone. Milestone 1 keeps the prompts that drove it; later milestones keep the plan they were built from.

* `01-foundation`: the boot, paging, system calls, shell, audio, and Doom of the first OS.
* `02-networking-and-userland`: the TCP/IP stack, the ported musl libc, BusyBox, the Dropbear SSH server, and the build modernization.
* `03-text-standard`: the text and documentation standard, the refactor that reached it, and the gate that keeps it.
* `04-normalization`: carrying that standard into the rest of the in-house tree (the tests, the krffs host tools, the utils, and the ish shell) and widening the gate to match.
* `05-images`: replacing the one overloaded RAM disk with three purpose-built images (a test image, our simple-tools image, and a real busybox/dropbear/Doom environment), built by a manifest-driven stager, plus a `make test` boot check and a bootable ISO per image.
* `06-review`: a full code, comment, documentation, support-file, and manual-test audit of the whole system, every finding refuted before it counted, the safe ones fixed in scoped commits, and the gate, boots, and capstone re-verified against a recorded baseline.
