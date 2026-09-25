# Entry 49: ext2 write batches

The finding is real. The starting tree stores the inode once per write, so the duplicate store described in the original finding was already removed. It still drains each write call. This change implements the mount-wide batch specified in [ext2-write-batch.md](ext2-write-batch.md). It changes no KRFFS write path and makes no claim about entry 53 or installer growth.

## Implementation

`ext2_write` collects allocated overwrites and contiguous appends. Each writable mount reserves 256 data images, 32 metadata images and sixteen inode records. The byte limit is 1 MiB per generation, including repeated overwrites. An admission plans one block, reserves its metadata, and prepares its complete data image before a commit that performs no I/O and cannot allocate. Failed admission discards only new reservations. Whole-call space and range checks precede acceptance.

Both data and metadata remain private until drain. Keeping data private is stricter than the design's permission to evict data early. It removes cache residency from the ownership rule without increasing the reserved storage. Reads consult these images before the cache. Held inode references retain visible size, pointers and times across close or failure; the committed size advances only after inode publication succeeds.

A drain holds the mount namespace lock through allocation metadata, data and zeroing, indirect blocks from leaves to roots, the large-file flag, and inode-table blocks. Each nonempty stage completes cache sync and a media flush before the next. Only eligible images enter the cache. ATA and AHCI keep their write-through flushes. Retained images replay a failed stage through the existing dirty-buffer sync path; ordinary direct cache writes still reject a failed device. No new cache retry interface is needed.

Attribute and namespace changes drain under the same lock before storing metadata. Truncate and final inode reclamation drain before freeing blocks. Mounted descriptions take the mount lock before their description lock, including when selecting an append offset. Loop attachment syncs and rechecks its backing file under the mount lock before publication; each loop write still syncs the backing file.

Final open-description release drains the mount. Shared descriptor references defer this boundary until their last close; independently opened descriptions have separate close boundaries. The first writeback or flush error remains on the mount until teardown. A successful retry permits unmount but never clears the error reported by fsync. Device loss records an error even with no dirty cache buffer and forbids retry. An abandoned prepared unmount clears the valid bit.

The owning [ext2 contract](../../rach2/sys/fs/ext2.Readme.md), VFS and block Readmes, and `open(2)`, `write(2)` and `sync(2)` describe these rules.

## Command-count proof

`sh tests/system/ext2_batch_mutate.sh HEAD` builds the probe and kernel in this worktree, boots a private fixture, mutates the drain condition to finish every call, then restores, touches and rebuilds the source. Its trap stops the test process group before restoration. Kernel checksums identify each artifact, and the restored kernel matches the fixed kernel. No time is an assertion.

The probe reads `/proc/disk`. Sixteen 4 KiB rewrites through one open description collect zero commands and zero flushes; fsync completes them with four commands and two flushes. Two 4 KiB direct-block appends collect zero commands and zero flushes; fsync completes them with six commands and three flushes, within the entry's ten-command bound. The starting revision costs 64 commands and 32 flushes for the rewrites, and sixteen commands and eight flushes for the two appends; both new rows fail against that kernel. The per-call mutation of the new path costs 64 commands and 32 flushes for the rewrites, and twelve commands and six flushes for the appends. Each mode fails its own command-count assertion. Restoration passes both modes. `e2fsck -fn` accepts each successful image; `debugfs` reads the expected bytes and file lengths.

The fixed output is:

```text
ext2-cost: result: rewrite collected commands=0 flushes=0 committed commands=4 flushes=2
ext2-cost: result: append collected commands=0 flushes=0 committed commands=6 flushes=3
EXT2PROBE OK (cost)
```

The count row is also part of `tests/run ext2`; its older rows are retained.

## Safety proof

`sh tests/host/ext2_batch_mutate.sh` builds the ext2 test alone with the host runner's ASan and UBSan flags. Mutations run in private source copies. Every fault is compiled, required to fail its named assertion, then restored, touched, rebuilt and required to pass. The trap stops the child process group before restoration. The script records sources, binaries, checksums and complete output under `build/tests/ext2-batch-proof.*`.

* Data and zeroing: publication replay covers direct-to-single and single-to-double transitions, a new double-indirect leaf, partial EOF and a hole below durable EOF. Free blocks contain nonzero bytes. Cross-call pointer rows run with sixteen and 512 cache buffers. Poisoning fresh data instead of zeroing it fails the hole-publication check.
* Other metadata operations: rows place setattr, rename and truncate after pending calls. A queued writer at the attribute lock handoff cannot enter before the store. Two pending inodes sharing a table drain before a third inode changes mode and atime. A drain failure leaves the attribute operation unstarted. Removing the lock across drain and store fails this check.
* Block reuse: a filled volume forces reuse of a truncated file's data and indirect blocks. Pending rewrites and tail zeroing finish before freeing; cache reclaim and later sync preserve the replacement file's bytes. Range and space refusals leave earlier accepted calls pending.
* Close: the linked descriptor implementation exercises shared references, a copied descriptor table and an independent description. Removing the final-release drain fails the close boundary check. A failed final close followed by reopening reports the error at two fsync calls. Both unlinked reference-release orders reclaim the inode and its data; immediate inode reuse starts empty.
* Eviction and teardown: private images survive small-cache eviction. Pause callbacks queue a writer and attribute operation at each nonempty drain stage and dispatch them only after mount unlock. Removing fsync's mount lock fails the ownership assertion. Failed unmount preparation and abandoned preparation leave the valid bit clear. Device loss prevents retry and teardown writes.
* Errors: write and flush failures preserve accepted bytes and a frozen generation. Twenty-four write failures cover two-inode generations across the direct-to-single and single-to-double boundaries, including separate inode-table blocks. Flush rows cover allocation, data, pointer levels, the large-file flag and inode publication. Successful retry, close and reopening retain the first error. Suppressing historical fsync errors fails the retry assertion.
* Media order: the native replay maintains a separate durable image and tests individual writes and reverse prefixes between successful flushes. Omitting stage flushes fails publication checks. `sh tests/host/ata_durable_mutate.sh` separately removes the ATA flush command and its completion wait; both fail returned-write durability against separate volatile and durable images. Restored driver sources pass.

Separate mutations check the byte, distinct-data, metadata and inode bounds. The complete ext2 host test passes its original 327 interruption stops and the added rows. The focused buffer-cache test reports `bcache_hosttest: OK`. The descriptor test reports `fd_hosttest: pass: stdio allocation and description ordering`; its mutation script also checks omission of non-KRFFS mount locks. The existing guest `loopdevprobe ext2back` passes attachment, writes, hard-link alias refusal and detach with an ext2 backing file. An ext2 image backed by another ext2 image was not tested. Triple-indirect publication, sector tears and exhaustive concurrent scheduling are outside the added rows.

## Limits and acceptance

The native device models assume atomic requests. They do not enumerate sector tears or every possible subset of pending writes. Scheduling callbacks prove lock ownership and deferred mutation at selected handoffs; they do not execute two kernel processes on the real wait queue. The ATA model covers PIO and DMA with LBA28 and LBA48; it does not model hardware interrupts or AHCI durable media.

`make rach2` succeeds under the kernel's `-ansi -Werror` build. The focused QEMU count proof passes. Both `tests/run ext2` and `tests/run queue` were attempted in this worktree and refused before building:

```text
hostcheck: refused: a TCP socket bound on the loopback address
hostcheck: refused: a UDP socket bound on the loopback address
hostcheck: refused: a Unix socket bound in the work directory
run: result: refused: this host cannot run the set
```

No other checkout was used. The unchanged `tests/system/suites/queue.sh` also passes when called directly with its required probes and a harness wrapper selecting an isolated network. Its six rows cover slow and fast AHCI readers, root faults beside queued I/O, late answers, deadline boundaries, quarantine release and pool exhaustion. This does not count as a successful `tests/run queue` invocation. Full named-suite acceptance and physical-machine command traces and rates remain unproved. Entry 49 has its implementation and focused proofs; its full acceptance is pending those runs. No commit was made. Both came after this record: the batch was committed as `aecb6f1b` on 2026-09-21, `tests/run ext2 queue diskrate default selfbuild-ext2 host` passed on the testing host, and every full set since has passed `queue`, as entry 49 of the plan records. The machine's own counts are in the last section.

## Artifacts

The starting revision is `5fe360c18d4862e720e1fbbaa7fdf038f2dcca3e`. The QEMU proof artifacts are under `build/ext2-batch-proof.4ofobb`; the fixed and restored kernels have cksum `3513161467`, length `2087848`. The ext2 native mutation artifacts are under `build/tests/ext2-batch-proof.d3sLkK`, and the ATA artifacts under `build/tests/ata-durable-proof.wMcx6u`. Queue serial logs are under `build/tests/ext2-queue/disks`; the loop backing row is under `build/tests/ext2-loop`.

The coordinated write-path changes were implemented locally because admission, cache ordering and mount lifetime share one contract. A second Codex agent supplied bounded refactoring and test work and reviewed the six production files. That review found no concrete correctness defect. It is not a substitute for the blocked acceptance runs.

## On the Lenovo

The machine's command and flush counts for 2000 calls of 4 KiB, appended and rewritten, are what `sh tests/system/manual_probes.sh KEY ADDRESS writes DIR` reads from `/proc/disk`. They were taken on 2026-09-25 on the final kernel, three runs through one open file on the installed ext2 root, each ended by one `fsync`: 2000 appended calls cost 124, 120 and 120 disk commands and 62, 60 and 60 flushes, and 2000 rewrites in place 54 commands and 27 flushes in each run. The machine's clock gave each run 11 to 15 centiseconds.
