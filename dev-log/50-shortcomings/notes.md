# Milestone 50: notes

## Where the work stands

Phase 1, the fixes, is done for every entry but 11, which is set out below for the owner's decision. Entry 22's rehearsal at the machine's size passed on 2026-09-10 in 3086 s, after the storage path was measured and fixed as entries 25 and 27 record: its first run, on the code before those fixes, saved at 1.3 MB a second and was stopped after 17 minutes with the first of its 1852 ranges unfinished. Phase 2 has not started. Nothing has touched the Lenovo.

## Phase 1: the fixes

Each entry's cause, fix, proof and commit are in `plan.md` beside the entry, since the list is the record the milestone keeps open. What the entries do not say:

* The order was severity first, as the plan asks: the memory refusal (12), the switch at a system call's exit (6), the request deadlines (14), the cache's write order (18), then the device table (19), the guest makefiles (23), the archive (20), the fallback (21), the acceptance's stick (3), the host check (17) and the rebuilt program's image (5). Entries 1, 2 and 16 were closed on the record of what earlier milestones built and the sets since, with the two experiments 2 asked for: a line typed while a foreground job ran arrived whole, and the three install suites' deadlines were doubled since their work was done late and not wrongly.
* Two faults of the tooling were found and fixed on the way, both in the host tests: the bootctl test built its fixture builder once into `/tmp` and kept it across runs, so an option added to the builder was not there, and the ext2 test's fixture lock had no holder in it, so an instance killed with the signal that removes nothing left every later instance waiting ten minutes; the builder is rebuilt when its source is newer, and the lock names its holder and is taken when that process is gone.
* The slow run was treated as a fault of the code, not paid for. The rehearsal at size was stopped at its first stage, the rates of every storage path were measured in a two-minute boot, the causes were found by counting cache gets and disk commands per MiB through `/proc/disk` and by sampling the guest's call chains through QEMU's monitor, and the fixes were measured with the same rows before the rehearsal ran again. Codex reviewed the diff twice on the way, the second round on the kernel's and drivers' paths alone at the owner's direction, and two of its findings were bugs in the diff. Three "after" measurements were made on a stick whose kernel `make` had not rebuilt and reported the old rate as the fix's; the rule is now in CLAUDE.md. `tests/run diskrate` holds the paths under bounds from now on.
* The runs were kept small: each entry's smallest suite, one mutation each, and one `style host` before each commit that touched a host test. The one set that lost time was the first run of entries 20 and 21 together, where the update left the recovery partition mounted while it wrote the root and the raw writer refused the disk; the two installer halves ran again after the fix.

### Entry 11, for the owner

A disk with 4096-byte sectors is still refused by name. The fix is the block path's unit: the partition scanner caches 512-byte sectors and the parser addresses the tables in them, the raw windows, the buffer cache, the request buffers, the disk lines and the storage driver's command sizing all count in 512 bytes, about sixty sites, and the two SATA drivers' refusals cannot be proved either way in the emulator, which presents such a disk only as a USB stick. It is a day of work in the code the sitting depends on, with host tests to match. It was not started in phase 1 because it would have gone into the sitting's kernel untested by a sitting of its own; the owner decides whether it is done before the sitting or after it, with the Lenovo's 512-byte disk unaffected either way.

## Phase 2: the review before the machine

Not started.
