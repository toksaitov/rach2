# Milestone 25: the sealed disk

The plan in `plan.md` was written before implementation and received four rounds of adversarial review. This record describes the implementation, its evidence, the claims refuted during the work, and the remaining hardware checks.

The goal was to write the laptop disk once and leave it installed. Subsequent kernels, boot menus, GRUB images, and complete recovery images would arrive over SSH.

## Phase A: the receiver window update

Milestone 24 measured bulk transfer at 0.9 KB/s on the laptop and 1.1 KB/s in QEMU. The similar rates correctly located the defect in the TCP stack rather than the new driver or the 100 Hz poll. Its record called the result "a property of the TCP stack", which was too broad. One defect affected one direction and required a change to one function.

A TCP receiver must report both decreases and increases in its window. This stack reported only decreases. The inbound paths in `tcp.c` recompute `rcv_wnd` whenever a segment arrives, so they announced a filling buffer immediately. The syscall paths `socket_tcp_recv` and `socket_tcp_try_recv` advanced `recv_head` without announcing the newly available space. A peer that had filled the window therefore waited for its persist timer before probing again. That timer determined the transfer rate.

The fix is `tcp_window_update`, which both receive paths call after advancing `recv_head`. It acquires `net_lock` and keeps it from the TCB snapshot through transmission because IRQ-side `tcp_close` can detach the TCB. It sends at most one bare ACK and only in ESTABLISHED; the closing states have no reader that can make room. The pure function in `rach2/sys/network/tcp/rcvwnd.c` decides whether to send. It has no state and performs no transmission. It waits until the window has grown by one maximum segment size or half the buffer, whichever is smaller, as required by RFC 1122 4.2.3.3. Earlier announcements could spend one segment per byte read and solicit equally small segments in reply.

Measured host to guest over SSH into `/dev/null`, same image, same QEMU invocation, with the call removed and restored:

| Bytes   | Without | With    | Rate without | Rate with |
|---------|---------|---------|--------------|-----------|
| 4096    |         | 0.361 s |              | 11.4 KB/s |
| 65536   | 60.179s | 0.291 s | 1089 B/s     | 225 KB/s  |
| 1048576 |         | 0.408 s |              | 2.57 MB/s |

The 64 KiB result without the update reproduces milestone 24's measurement to three digits. This identified the two measurements as the same defect.

Proof: `tests/host/rcvwnd_hosttest.c` exercises ring and segment sizes the running kernel never uses. Geometry determines which half of the rule applies: at 4 KiB, one segment is smaller than half the buffer; at 8 bytes, half the buffer is smaller. Four mutations failed: never announcing, announcing every byte, using two segments instead of one, and dropping the 16-bit clamp. The measurement supplies the wiring evidence, and the column without the update is the phase's failing case.

### What the review changed

An independent review raised five findings. Three were confirmed and fixed in the same commit. The threshold was two maximum segment sizes, not the RFC 1122 rule of one segment or half the buffer, whichever is smaller. The old threshold suppressed valid updates. The returned window was cast to 16 bits without a clamp, so a ring of 65536 would have announced zero. This function interprets zero as no update, so a fully drained buffer would have announced nothing. The new function also separated `tcp_is_connected`'s documentation block from `tcp_is_connected`; the block was restored to the correct function.

### What is deferred, and why

The rule governs updates triggered by a reader, not every window the stack advertises. The inbound paths still recompute `rcv_wnd` from free space whenever they owe an ACK, so a segment arriving after a small read can advertise a window below the threshold. This behavior predates the milestone, and the update path usually hides it: a reader that drains normally announces a full-sized window before the peer needs to probe. A correction would keep the advertised right edge fixed while accepting data instead of recomputing it from free space. That change affects the receive state machine, which phase A deliberately left alone, and requires a stateful test.

The host test covers the rule but not its call sites; deleting both calls leaves the test passing. Manual measurements supplied the wiring evidence, once with both calls present and once with them removed. Every result is for host-to-guest transfer. The reverse direction was never slow and was not measured again. An automated check would require a packet recorder around a live connection and was deferred until another test also needs one.

## Phase B: the tool's new name and shape

`install-kernel` became `bootctl`, following the name of systemd's tool for managing boot-partition kernels and defaults. The shared name gives students a term they will encounter on Linux systems. This project's verbs remain different because its kernel diagnostics, Readmes, and harness already use trial and commit:

    bootctl status         what is installed, committed, and booting next
    bootctl kernel FILE    stage a kernel into the candidate slot
    bootctl commit         make the booted trial the saved default

No alias is installed. `--commit`, `--where`, and a bare kernel path are refused and tested. An undocumented compatibility form would impose permanent maintenance without helping users.

Three boundaries limited the change to twenty-one files. The `bootslot` library retained its name because it identifies the object being managed, and a host test matches two dozen of its error substrings literally. `dev-log/` retained its historical names. `make install-test` retained its name because its loop still installs a kernel.

`status` is the phase's only new behavior. The sealed disk will not be opened, so the tool reports the three files' sector extents, the environment block, whether a trial is armed or consumed, and the slot used for the current boot. It does not report the GRUB core image because the required device does not exist until phase C.

Two additional changes addressed hazards predicted by the plan.

`prune-stale` now sweeps `build/utils/sbin` as well as the top level of `build/utils`. The old code assumed that staging each sbin file by name made a sweep unnecessary. A manifest that retained the old name would continue staging the stale binary, while `check_setid.py` compares the declared source with its table without stat-ing the host file. The first build after the rename left exactly that artifact, demonstrating the defect.

The harness's `awk` pattern was anchored. `tools/install_test.sh` read the candidate's first sector with `/candidate/{print $5}`. This was unambiguous when `--where` printed three lines, but `status` prints six lines and three contain the word. The shipped order happened to work because the extra matches produced an empty fifth field and the shell removed trailing newlines. A non-extent line placed first produced a leading newline and gave `dd` a malformed seek. The new pattern matches the extent line's format and does not depend on order.

Proof: `make install-test` passed end to end, running the loop twice on one disk. This verified that trial, commit, fallback, and repair survived the rename. `make selfbuild-test` passed after building `bootctl` from `guest.mk` on Rach2, which verified that the guest list changed with the host list. `tests/host/bootverb_hosttest.c` covers the parser.

Failing cases: pointing the harness at the old `install-kernel: committed` string fails the run, proving that the assertion reads the tool's output. Two parser mutations fail the host test: accepting `--commit` and tolerating a surplus operand. The anchored `awk` produces no result for a reworded extent line instead of returning the wrong sector.

### What the review changed

The review produced six confirmed findings. Two affected new code, and two affected existing code touched during the rename.

`status` described every empty environment value as a consumed trial. Only an empty `next_entry` has that meaning; an empty `saved_entry` says nothing about a trial. The caller now requests the annotation explicitly instead of having the printer assume it.

A known verb with the wrong number of operands was reported as unknown. For example, `bootctl kernel` without a path printed "not a command this tool takes: 'kernel'". The exit status was correct, but the diagnostic was not. `bootverb_known` now distinguishes the cases, and an arity error prints only the usage line, as before the rename.

`bootslot_commit` matched the armed-trial marker as a bare 21-byte string without a leading newline. A preserved line ending in `next_entry=candidate` could therefore look like this machine's armed trial and block a valid commit. Such a line can exist because the writer deliberately preserves unrecognised lines. The pattern is now anchored like the consumed marker, and both bounds are checked independently so neither shortens the other's search. A fixture containing `x_next_entry=candidate` beside a real consumed marker verifies the correction. The defect predates this milestone; the rename exposed it by adding a second reader for the block.

The parser test checked that `kernel` produced an operand but not which operand. A parser returning `argv[1]` instead of `argv[2]` passed and would make `bootctl kernel FILE` open a file named `kernel`. The test now checks the value, and the mutation fails.

The remaining two findings were smaller. `utils/Readme.md` still described `--commit`, used the host test's old filename, and said pruning covered only the top level. The new parser test also contained section labels inside its body, contrary to the project's comment rule. Both were corrected.

A second review round found one more defect and refuted none. `bootverb_parse` and `bootverb_known` had separate verb lists. Adding a verb to only one list would compile but make the diagnostic report an accepted verb as unknown. They now share one catalog. A test traverses it and requires every accepted verb to be known to the diagnostic.

One finding from that round was refuted. The new fixture in `tests/host/bootctl_hosttest.c` begins with a comment introducing the case, although the comment rule generally rejects such comments. The file consists of eighteen similarly introduced cases, and its scenarios provide the test structure. Keeping the local convention was appropriate; changing it would be a separate cleanup rather than part of the rename.

## Phase C step 1: the geometry

The build now produces the sealed disk layout described by the plan. `make rach2-sealed-disk` writes it, and the built image was checked against every value in the plan: 660602880 bytes, 1290240 sectors, 630 MiB; partition 1 at LBA 2048 for 131072 sectors, type 0x0c; partition 2 at 133120 for 1024000; partition 3 at 1157120 for 131072; entry 4 empty; and 4096 bytes per cluster in the boot partition.

`tools/mkdisk_grub.sh` gained three parameters: the boot-partition size, mformat's sectors per cluster, and a third partition that is absent when its size is zero. Their defaults preserve every existing caller. The two geometry parameters depend on each other. `bootslot_open` refuses a volume with a FAT larger than 512 sectors or more than 65536 clusters. A 64 MiB partition with one sector per cluster has about 130500 clusters and a 1020-sector FAT; with eight sectors per cluster, it has 16348 clusters and a 128-sector FAT.

Three corrections were required before the generated file had the intended geometry.

The builder's chain verifier compared cluster counts with sector counts. This happened to work while mformat selected one sector per cluster. It now reads the cluster size from the formatted volume and performs the required division. The report prints the value so a changed geometry is visible in the log.

The same verifier parsed `mshowfat` output with a pattern that matched only a cluster range, `<a-b>`. A file that occupies one cluster prints as `<a>`. This did not occur with 512-byte clusters but occurred immediately for `grub.cfg` with 4096-byte clusters. The parser now accepts both forms.

`krffs/mkdisk.sh` refused partitions larger than 256 MiB based on the kernel's mount window. Milestone 24 increased that window to 512 MiB when it moved above the MMIO region used by real firmware, but this limit was not updated. The correction records the reason for the new value. This path did not block the sealed image, which uses `mkimage.sh`, but its documented limit was wrong.

The host fixture assumed one sector per cluster in fifteen places, so it never exercised the library's cluster arithmetic. With that geometry, multiplication by `cluster_sectors` is the identity, and removing the multiplication would not fail any existing test. `mkfatfixture.py` now accepts the geometry as a parameter. `--cluster8` builds the same files with 4096-byte clusters, while every existing option still produces a byte-identical image. A new case opens this volume, requires the library to report eight sectors per cluster and 512 clusters for the 2 MiB slot instead of 4096, installs a kernel, and refuses a commit.

Proof: the sealed image built, and its partition table matched the plan field by field. The current kernel booted the image, mounted the 500 MiB root, ignored partition 3, and reached a login. `make install-test` passed with the unchanged default geometry, and its log reported 512 bytes per cluster.

Failing cases: removing `* cluster_sectors` from the library's cluster-to-sector conversion now fails the host test, although it passed before this fixture existed. Computing the volume's cluster count as a sector count also fails.

### What the review changed

The review produced four confirmed findings.

Partition 3 had a type byte but no file system. `parted`'s filesystem argument selects the MBR type without creating a file system, so the entry described 64 MiB of zeros as FAT32. GRUB could not read the partition needed by the recovery entry. The build now formats it and creates the two directories that will contain its files. Phase G still owns those contents.

The image was not reproducible. Two builds from the same tree differed in the random FAT volume serial and in the creation and access dates that mcopy copied from the clock into directory entries. The serial is now fixed, and `SOURCE_DATE_EPOCH`, which mtools honors, fixes the dates. Two builds now produce byte-identical images. Two inputs remain deliberately variable and are recorded as inputs: the container uses the floating `ubuntu:24.04` tag, and each tree generates its own SSH host key and password salt.

The new cluster case checked cluster-to-sector conversion but not the volume's cluster count. A version that counted clusters as sectors passed because the resulting value exceeded the volume and therefore constrained nothing; every fixture chain ended well below it. The test now checks the count directly. A second fixture with the same geometry extends a chain past the volume so the count is also exercised as a bound.

The owning documentation still described a two-partition disk and omitted all three new parameters.

## Phase C step 2: the level-one devices

Two devices provide level-one disk authority: `/dev/bootpart` for the boot partition and `/dev/grubcore` for the post-MBR region containing GRUB's core image. Every image, including the everyday image, may write both because kernel and boot-loader updates are ordinary operations.

The previous discovery order prevented recovery from using this authority. `/dev/bootpart` was published only after a successful KRFFS mount. A machine booting a RAM image without mounting the disk therefore could not repair the boot partition that had prevented an ordinary boot. Discovery now precedes root selection: the kernel probes the disk once, reads and validates sector 0 once, publishes both devices, and then selects the root source.

Validating entry 0 required a second predicate because the original predicate needs a mounted root. `mbr_bootpart_pick` compares entry 0 with the entry containing the root, but a RAM-root boot has no such entry. `mbr_bootpart_ok` instead checks the candidate against the complete table: it must have a FAT type, be non-empty and contained within the disk, start at or after the post-MBR region, and share no sector with another non-empty entry. `mbr_bootpart_pick` now adds its two root-specific constraints to that common validation, preventing the rules from diverging. Discovery cannot know which entry will contain the root. If FAT entry 0 later proves to be the root itself, the mount removes the device through `devfs_bootpart_withdraw`.

The post-MBR check exposed a defect in the project's test fixture. `tools/mkbootfixture.py` placed its FAT boot partition at LBA 64, inside the 1 MiB region used by the boot loader. No earlier interface exposed those sectors, but publishing `/dev/grubcore` would have overlapped the boot partition. The kernel now refuses to publish the window for such a disk, and the fixture's boot partition moved to free space after the KRFFS partition.

`/dev/grubcore` has minor 3 and mode 0600 and covers LBA 1 through 2047. Its extent is a layout constant, not a value read from the partition table, so a damaged table cannot enlarge it. Sector 0 is excluded because it contains the partition table. Since the disk writes complete sectors, changing the eight boot-code bytes that locate the core image would also rewrite the adjacent table.

Both devices use the same implementation. `raw_window_read` and `raw_window_write` replaced the bootpart-specific functions, so the new device uses the path already tested by the original one. The write handler also checks the effective uid at write time instead of relying on the credentials used at open. The mode rejects a non-root open, but uid 0 passes the credential check regardless of the mode. Without the write-time check, a process could open the device as root, drop privilege, and continue writing through the descriptor. `block_ready` caches the probe result, so the three callers that previously reset and identified the controller independently now use one result.

Proof: `make bootpart-test` gained a third boot that uses a RAM image with the disk attached and still requires both devices. This directly tests the milestone requirement. It exercises the post-MBR window end to end: the exact extent, a read at the beginning, exactly 0 at the end, `-ENOSPC` at and across the end, `-EINVAL` for an unaligned offset and a partial-sector count, and `-EACCES` for an unprivileged open. `mbr_hosttest` covers the new validation and gained a labelled macro so each failure is identified.

Failing cases: returning from discovery before publication fails the RAM-root boot. Removing the write-time uid check permits the write after privilege has been dropped and fails the test. Extending the post-MBR window beyond 2047 fails its extent and end checks. In `mbr_hosttest`, removing the region check or comparing entry 0 only with entry 1 also fails.

### What the review changed

The review produced six confirmed findings. The first could have destroyed a file system.

`/dev/grubcore` was published before sector 0 was read, with only a requirement that the disk contain 2048 sectors. On a disk with an unreadable or unsigned table, or a partition extending into the post-MBR region, the window could grant write access to file-system sectors. The repository's own fixture had a partition at LBA 64. Publication now waits until the table has been read and its signature accepted. `mbr_gap_clear` withholds the window if any non-empty entry reaches LBA 1 through 2047.

Discovery and mount each read and parsed sector 0, contradicting the record's claim of one read and allowing the results to differ. Discovery now retains the parsed table for the mount.

`block_ready` cached failures as well as success, preventing rescue-shell retries after a controller failed to respond during boot. It now returns the existing `block0` when a disk has been found, avoiding a second reset of a working controller, and retries when no disk is present.

The rescue shell's `ahci` command reinitializes the controller and clears `block0` and the HBA globals. Before this step, a boot without a mounted disk had no raw window. After early publication, such a window could remain attached to a driver without a port. The command now removes all three raw windows, including `/dev/disk0`, before reinitialization and republishes them after either success or failure.

The final two findings concerned documentation. The devfs and block Readmes described the old contract, and three comments no longer matched their declarations, including a claim that the boot partition is 16 MiB.

### A disputed scope decision

The plan requires MBR validation against build-time geometry: the exact types and bounds of entries 0, 1, and 2. The implementation does not provide that check. Pinning those values would make every image except the sealed disk fail, causing `install-test`, `test-disk`, and other images to lose `/dev/bootpart`. The implemented validation applies to any disk: the post-MBR region must be clear, entries must be contained, and entries must not overlap. The review agreed that the region check was necessary for safety and raised it as a finding, but did not accept deferring the exact geometry check. The sealed kernel build introduced in step 4 is the proper place to pin the sealed layout. This deviation is recorded explicitly.

## Phase C step 3: offsets wide enough for a real drive

Recovery must address the far end of a 250 GB disk during a reflash, but the kernel could not do so. Open-file offsets were 32 bits, `_llseek` refused values above 4 GiB, and `/dev/disk0` was limited to 8388607 sectors for that reason, as its comment stated.

The plan identified the shared descriptor offset as the main risk to unrelated code. That field was left unchanged. The open-file description gained a separate 64-bit `raw_offset`, used only by the three raw disk windows. Files, procfs, pipes, and terminals retain their 32-bit offset. Placing the new field in the open-file description keeps the offset shared across `dup` and `fork`, as POSIX requires.

Two size queries replaced the previous pair. `devfs_fd_bootpart_size` and `devfs_fd_disk_size` served three uses, including the boot log, which is not a disk window and uses the ordinary offset. `devfs_fd_raw_size` now returns 64 bits for the three raw windows, while `devfs_fd_log_size` returns 32 for the log. Both seek syscalls select the applicable query. `lseek` refuses an unrepresentable result instead of truncating it, leaving `_llseek` as the required interface for the far end of a large disk.

The wider offset allowed the window limit to increase from 8388607 sectors to 2^32 sectors, or 2 TiB. This is a policy limit of the window. ATA stops earlier at its 28-bit LBA, while AHCI programs a 48-bit LBA. `/proc/disk` prints the drive's actual size beside the window size, making the difference visible for a larger disk.

Proof: a fourth `make bootpart-test` boot uses a sparse 260 GB image from `tools/mkwidefixture.py`, attached through AHCI because ATA's 28-bit LBA stops at 128 GiB. It seeks to the end and requires the complete drive size; reads and compares markers at 5 GiB and 258 GiB, the latter requiring 38 offset bits; crosses the 4 GiB boundary in both directions with `SEEK_CUR` and refuses a seek before the beginning; duplicates a descriptor positioned at 258 GiB and reads through the copy, proving that the wide offset belongs to the open-file description; and requires 32-bit `lseek` to report overflow for an end it cannot represent. Before this change, the window ended at 4 GiB. All 62 probes in `make test`, plus `verify-host`, `bootpart-test`, `diskdiag-test`, `test-disk`, and `persist-test`, continued to pass. These results show that the change remained confined to raw disk windows.

Failing cases: making `/dev/disk0` read through the old 32-bit offset fails at 5 GiB. Retaining only 33 bits of the resolved offset passes at 5 GiB but fails at 258 GiB, which is why the fixture is 260 GB. The old 4 GiB byte limit also fails the host test.

### What the review changed

The review produced seven confirmed findings.

Both seek paths added the displacement to the base as a signed 64-bit value and then checked whether the result was negative. This does not establish a bound: adding one to a descriptor at the top of the signed range is undefined, as is negating the most negative displacement. A shared resolver now uses unsigned arithmetic, obtains the displacement magnitude without negation, and checks both directions before addition.

A valid seek result that the 32-bit call could not return produced `-EINVAL`. It now produces `-EOVERFLOW`, and the probe requires that result.

Raw identity depended on whether the window was currently published. Removing a window during controller reprobe therefore changed an open descriptor from a seekable disk window into an unseekable generic device. `devfs_fd_is_raw` now identifies the descriptor from the operations selected at open, and seeking returns `-ENXIO` while the backing device is unavailable.

The increased window limit used the largest 32-bit LBA as a sector count, leaving the final addressable sector unreachable. It now uses a count of 2^32.

The initial wide-offset test was insufficient. Its 6 GiB fixture passed with only 33 offset bits and did not exercise `SEEK_CUR` or sharing. The fixture is now 260 GB, larger than the disk being sealed, so the far marker requires 38 bits. It uses AHCI because ATA's 28-bit LBA stops at 128 GiB. The boot crosses the 4 GiB boundary in both directions with `SEEK_CUR`, refuses a seek before the beginning, and duplicates a descriptor positioned at 258 GiB to prove that the open-file description owns the shared wide offset.

The final two findings concerned prose. The filesystem and block Readmes still described one offset and the old limit, and several comments no longer matched the implementation.

## Phase C step 4: who may write the whole disk

The implementation now enforces both planned levels of disk authority. Every image may write level one, the boot partition and the boot loader's post-MBR region, because installing a kernel and updating GRUB are ordinary operations on a sealed machine. Level two permits writing any sector through `/dev/disk0`. Recovery needs that access to replace a damaged root, but an everyday boot does not; an error there can destroy the complete installation instead of one file.

A build switch selects the authority. The everyday kernel compiles a write handler that always returns `-EPERM`; only `-DRACH2_RECOVERY` includes the writable implementation. A command-line token would allow anyone at the GRUB prompt to enable the operation. The project had already rejected that design for the read-only disk policy, and whole-disk writes require at least the same protection.

`RACH2_DISK_READONLY` could not serve this purpose. That switch is enforced below devfs in the drivers' command allowlists, so it also prevents boot-partition writes that both builds need. The new switch controls only whether devfs provides a writable whole-disk node. `rach2/sys/block/recovery.c` contains a banner identifying the build, `tools/check_recovery.py` checks both forms, and `make rach2-recovery` checks both kernels before a mislabeled artifact can reach a test.

The write path also gained batching. Every raw transfer in either direction through any of the three windows uses one 32 KiB buffer in `.bss` and processes sixty-four sectors at a time. Both drivers can issue that as one command: ATA permits 256-sector chunks, and AHCI refuses counts above 8192. The linker places `.bss` below the 32 MiB identity-mapped limit, so the buffer's virtual address is also the physical address required by the AHCI PRDT. One buffer can serve every window because a syscall is not preempted. Batching makes a whole-disk reflash practical. At one sector per call, a 500 MiB write requires about a million driver calls and, because both drivers flush after every write command, about a million cache flushes. Sixty-four-sector batches reduce that to sixteen thousand calls.

Proof: `make authority-test` boots both kernels against the same sparse image; only the kernel differs. The everyday build must return `-EPERM`, after which the target sector must still contain zero. The recovery build must write beyond 2 GiB, read the sector back byte for byte, and complete a 200-sector write.

The return value alone cannot prove batching. A 200-sector write returns the same result after four driver calls or two hundred. `/proc/disk` therefore reports the batch size and a live driver-call count. The probe reads the count before and after the write and requires an increase of four.

Failing cases: compiling the recovery write body into the everyday kernel fails the ordinary boot, tying the refusal to the build switch. Setting the batch size to one sector fails the driver-call assertion while preserving every assertion about the write result. Reducing only the bounded-window read chunk fails the post-MBR case. Making AHCI split each call into one-sector commands fails the command and flush counts without changing the counts above the driver.

### What the review changed

The review produced eleven confirmed findings. The first showed that the claimed authority applied to one function, not the complete kernel.

The rescue shell's `disk write` command wrote the final sector by calling `block0->write` directly, bypassing devfs and its `-EPERM` check. An ordinary kernel booted with `rach2.rescue` could therefore write outside both bounded windows. The command is now absent from an ordinary build, which prints the reason for the refusal.

Batching exposed an ambiguity in partial failures. The block interface returns zero or an errno and cannot report how much of a request completed. ATA writes sectors individually within one command and can fail after several writes, so a failed 64-sector call can change as many as 63 sectors while reporting none. Retrying the batch one sector at a time does not measure the original operation because the retry can fail earlier. The documented contract has three parts. Sectors counted by a short return were written. The batch beginning at the returned offset failed and has unknown state. No later batch was attempted. The uncertain batch contains 64 sectors, or fewer for the final partial batch. A caller resumes at the returned offset, rewriting the uncertain batch and the remainder. A narrower uncertainty would require a block interface that reports partial command completion, affecting every driver and caller.

The first instrumentation counted calls into the block interface. It measured devfs batching but not the number of commands issued by a driver. A driver could split one 64-sector request into 64 commands, appear identical at that level, and still incur the cache flushes that batching was intended to avoid. Command and flush counters now increment immediately before the corresponding register write, excluding refused or abandoned commands. `/proc/disk` reports both, and the test requires exactly eight commands and four flushes for a 200-sector write. Exact counts prevent an implementation that counts nothing from passing a loose bound.

Three findings concerned test coverage. The test exercised only whole-disk writes over AHCI, so regressions in either bounded window or in a read loop could pass. It now drives all four batched loops across more than one batch: it reads and writes the post-MBR window and the whole-disk window. Two limits remain documented: this target does not exercise ATA, and no test causes a failure during transfer because the block layer has no fault injector.

The remaining findings were smaller. A first-chunk user-copy fault from the whole-disk writer returned `-EIO`, while the bounded writer correctly returned `-EFAULT`. The 32-bit call counter wrapped after about nine passes over a 250 GB disk, and the probe parsed it as a signed 32-bit value that overflowed sooner. The DMA buffer lacked declared alignment although the AHCI PRDT uses its address directly. `make rach2-recovery` checked an artifact on which it did not depend, so a clean tree could reach a missing file. Several comments also described `/dev/disk0` as read-only in every kernel.

One claim was too broad. `check_recovery.py` detects a kernel built without the switch, which is the build error it is intended to catch. The banner and handler use separate conditionals, however, so the checker does not prove a deliberately altered artifact. `make authority-test` boots both kernels and verifies their behavior.

## Phase D: the boot menu that can be rewritten

GRUB now reads two configuration files from this disk. `/boot/grub/grub.cfg` is an immutable stub written when the disk is built. It defines every required entry and then sources `/boot/boot.cfg`. `bootctl config` rewrites the latter file in place. Its 8192-byte preallocation prevents an update from extending the file or relocating its cluster chain.

Entry order provides the safety property fixed by the plan. `source` appends the second file's entries to the first file's entries, while `set fallback` selects an index rather than an id. The stub entries must therefore retain the low indices regardless of the staged menu. A missing, partially written, or invalid staged menu then affects only the entries it was intended to add.

The entry numbers form a fixed contract: candidate 0, fallback 1, candidate-static 2, fallback-static 3, debug 4, and recovery 5. The static-address entries initially use an empty token set because the networking phase has not yet assigned the machine an address; they exist here to reserve their indices. Entry 5 refers to a kernel and RAM image on partition 3 that the recovery phase has not built, so its title marks it as unbootable.

The plan left one design question open, and the first implementation was incorrect. Its proof required booting an entry defined only by the staged file. The implementation allowed that file to select its own default through a variable read by the stub. An in-place rewrite is not atomic: a power loss can leave a valid prefix of the new menu followed by the old tail. Such a file could set the variable to an entry whose failure would not necessarily activate fallback. In addition, `load_env` imports every grubenv key as a variable, allowing the environment block to set the same value. Both paths violated the claimed property.

The mechanism was removed. A second review also rejected the first replacement claim. `source` executes GRUB script, so a staged file can set environment keys read by the stub or load a kernel before returning. It cannot be restricted to declarations. Write access to the file therefore grants control over the boot, consistent with level-one authority: `bootctl kernel` already replaces the next kernel.

The stub provides a narrower guarantee. Every required entry is defined before the staged file is read. A staged file that is missing, truncated during a write, or syntactically invalid therefore removes only its additional entries and leaves the required menu bootable.

`bootctl` gained four verbs. `config` stages a menu and refuses an oversized file before writing. `env` reads or sets one of two allowed grubenv keys; it is not a general editor. `trial` arms one boot of a stub-defined entry, and `cancel` clears it. The library now resolves `boot.cfg` as a fourth file. Its writer rewrites every slot sector, even after the new text ends, so no tail of a longer menu remains. It reads each sector back before writing the next.

Proof: `make menu-test` performs four boots from one disk. The first stages a two-entry menu with `bootctl config`, round-trips a grubenv key through `bootctl env` and `bootctl cancel`, and corrupts the candidate kernel so the next boot must use fallback. The second requires both staged entries in the menu and verifies that the fallback index still refers to the stub's entry. The third truncates the staged file within an entry from the host; the machine still boots through the stub, and the log contains GRUB's parse error. The fourth verifies that the slot extent has not moved.

Failing cases: moving the stub entries after `source` makes `set fallback=1` select `stagedtwo` instead of the stub fallback, which the GRUB log identifies by name. A menu one byte larger than the slot is refused. A writer that updates only sectors reached by the new text leaves the previous tail; the host test detects it by replacing a full-slot menu with a short one.

### What the review changed

The review produced ten confirmed findings.

Two findings described the same defect through different inputs and caused the feature to be removed. A torn slot write could publish a staged default, and `load_env` imports grubenv keys as variables, allowing the environment block to set it as well. The claim that this was impossible was false.

A second round rejected the replacement claim. Removing the variable did not make the staged file inert because `source` executes GRUB script. The file can set keys used by the stub or load a kernel before returning. The record now states only the narrower damage guarantee above.

The two static-address entries used only the static token set, which remains empty until the networking phase. On the sealed disk they would therefore omit `rach2.disk=ahci` and fail to find the root. They now include the machine token set as well.

`bootctl trial` accepted two ids that the stub did not consume. Such a value would remain armed indefinitely, block later commits, and select no entry. The stub now consumes them. The verb also accepted `recovery` before any phase had built its kernel; that id was removed from the allowlist until the entry exists.

The menu writer used truncating division for its sector count. A slot whose size was not sector-aligned left its final bytes unchanged, while a zero-sized slot reported success without writing. Both geometries are now refused when the volume is opened, before a partial write is possible.

Making the menu slot mandatory prevented `status`, `kernel`, and `commit` from operating on older boot partitions even though none needs the menu. The slot is now optional, and only `config` requires it.

The requirement that a staged menu end with a newline was neither necessary nor sufficient. It rejected a valid GRUB file ending at its final brace and accepted a transfer cut after a complete entry. The requirement was removed, and the tool now states that it checks only the size.

The remaining findings were three test omissions and several stale comments, including a `--menu-short` fixture option unused by every test case.

A third round found the most serious remaining defect. `bootctl cancel` wrote an empty `next_entry=`, but an empty `next_entry` does not mean "no trial". It is the marker left by GRUB after consuming a trial, and `bootslot_commit` requires it. Installing a kernel, cancelling its trial, and committing could therefore make an untested kernel the default. Cancelling now removes the key. `env` refuses empty values and directs the user to the proper command. A host case installs, cancels, and requires commit to fail.

The second round found four more defects. `bootctl env` validated only the key, so `env next_entry recovery` armed an entry refused by `trial`; a newline in a value could also create a second grubenv assignment. Values are now checked for control characters and against the same entry list used by `trial`. `config` filled a fixed buffer without probing for one additional byte, so a sufficiently large slot could truncate an oversized menu and report success. The harness checked only the fallback index, allowing entries 2 and 3 to be exchanged; it now reads every displayed title and requires the complete order. Finally, the claim that a zero-size slot is refused was both false and unnecessary: an empty file without a cluster is correctly treated as absent.

## Phase E: replacing the boot loader from the running machine

Sector 0 is immutable after the disk is built. It contains the partition table, and whole-sector writes prevent changing the boot-code field at byte 0x5c without also rewriting that table. LBA 1 therefore acts as the selector. GRUB's `diskboot` sector contains a blocklist with an 8-byte LBA at offset 0x1f4 and a count at 0x1fc. The post-MBR region contains two 1023-sector tails: A begins at LBA 2, and B begins at LBA 1025. Replacing the core image writes the inactive tail before changing the one-sector selector.

`utils/grubslot.c` implements the layout arithmetic. `grubslot_active` reads the blocklist and identifies the active tail. It refuses layouts unusable by the installer: more than one record or a count larger than one slot. The function validates geometry, not disk provenance. `grubslot_other` identifies the inactive tail. `grubslot_patch` updates a replacement image's blocklist and refuses images smaller than 2 sectors or larger than 1024. `grubslot_install` reads the selector, patches the image for the inactive tail, writes all 1023 tail sectors, reads and compares all 1023, and finally writes LBA 1. Sector-reader and sector-writer callbacks let the same implementation operate on `/dev/grubcore` and on a host-test model. `bootctl grub FILE` reads the file, checks its size, and invokes this sequence.

Writing the complete tail removes any remainder from a longer predecessor. A failure before the final selector write leaves the old tail selected, so the machine boots as before. The final write remains hazardous: a torn write to LBA 1 can make GRUB unreachable, and recovery cannot run because GRUB loads it. The plan records this limit.

Proof: `make grub-test` performs three boots from one disk. `tools/mkgrubcore.sh` creates two core images that differ only in a configuration embedded with `grub-mkimage -c`; it prints `GRUBCORE=A` or `GRUBCORE=B`. The first boot installs B and reboots. The second must print B, install A, and reboot. The third must print A. Because the configuration inside the running image prints the marker, this tests execution rather than only stored bytes. Reading back the blocklist would allow an implementation that writes only the selector to pass. The two installs must also report different tails.

Failing cases: a readback that always reports agreement allows a dropped write to pass, and the host test fails in three cases. A `grubslot_other` implementation that returns the active tail fails eight cases. Removing the kernel's second-open refusal fails `make grub-test`. The harness also failed twice during development: once because the embedded configuration printed before a terminal existed, and once because its expected marker count was wrong.

### What the review changed

The embedded configuration now initializes the serial terminal itself. It runs before the partition configuration, so an earlier `echo` reached only the screen and was invisible to the serial harness.

The two core images were initially staged through `optional-file` lines in `os-real.manifest`, like Doom's wad, instead of through a generated manifest. `check-setid` correctly requires each allowed manifest to be a source file because a generated manifest could bypass its set-id list.

Two successful swaps did not by themselves prove alternation. Overwriting the same tail twice would still boot the most recently written image, so markers alone could pass without using the other slot. The harness now requires the two installations to report different tails.

The install sequence was inline in `bootctl` and operated directly on a file descriptor, preventing a test of a disk that reports success but drops a write. Moving the sequence into callback-based `grubslot_install` lets a host model simulate that failure. The test requires readback to detect the drop, the selector to retain the old tail, and the old image to remain there.

The second round produced six more confirmed findings.

The installer checked candidate size but not content. A truncated transfer or unrelated file of at least 513 bytes could be padded, written, verified, and activated, leaving invalid code for the next boot. Candidates are now validated before any write. They must contain GRUB's blocklist segment 0x0820 at offset 0x1fe, and the tail length declared at 0x1fc must equal the file's sector length.

`grubslot_active` read the blocklist start LBA but ignored its count. A core image extending from tail A into tail B therefore appeared to occupy only A, and installing into "inactive" B could overwrite the running image. The count must now be between 1 and 1023. `grub_embed.py`, which builds the initial core image, previously accepted any image fitting before the first partition, a region of 2047 sectors. It now refuses images larger than 1024 while a malformed disk is still cheap to rebuild.

The install lacked exclusive access. Two installers could read the same active tail, verify separate writes, and leave the selector referring to one image with the other's length. `/dev/grubcore` now permits one opener and returns `EBUSY` to a second. A lock file could remain stale on a remote sealed machine. The kernel flag instead clears when the final descriptor closes, including on process exit.

Staging the two marked images through `optional-file` in `os-real.manifest` did not keep them out of later production images. `make grub-test` leaves the files in the build tree, so a subsequent build could include them. They now belong to `krffs/manifests/grub-test.manifest`, which adds the two lines to os-real. Before building, the target compares the manifests so their common content cannot diverge.

The Readme claimed that the build already embedded two `configfile` calls. It did not; `mkdisk_grub.sh` passed no `-c`. The paragraph was removed until the recovery phase implements the behavior.

The prose also personified the core image. It was corrected to state that a configuration inside the image prints a marker.

A third round identified a limit in the second round's correction. The declared length is measured in sectors because a core image stores no byte length. The current image is 157703 bytes, so removing its final 6 bytes still rounds to 309 sectors and passes; removing a full sector fails. Packaging the image with a digest would close the gap but would also prevent installation of an ordinary image from grub-mkimage, defeating the verb's purpose. The limit remains documented in `utils/grubslot.h` and the GRUB section of `tools/Readme.md`. Operators must compare `sha256sum` at both ends of the transfer. The checker validates the format; the harness establishes that an actual image boots this disk.

The same round produced two more confirmed findings. The truncation test installed a complete synthetic 309-sector image and removed no bytes. Rounding and zero padding occurred inside `bootctl`, beyond the host test's reach. They now live in `grubslot_prepare`, and the test removes zero through seven bytes from a 157703-byte image. It requires six missing bytes to remain undetectable and seven to be refused. `utils/Readme.md` also described only the three milestone 23 verbs and claimed the tool never locks its device. It now includes the menu verbs and a separate `bootctl grub` contract: candidate checks, write order, the LBA 1 exposure, the limits of validation, and the single-opener rule.

The review proposed decoding the LZMA payload and requiring its output length to match the value at 0x20c, which would detect the six missing bytes. This was deferred because it requires an LZMA decoder in the installer and a validator for every compression mode emitted by grub-mkimage. An operator can instead verify the image with `sha256sum` before transfer, while `grub-install` performs no comparable core-image validation. The remaining exposure is final-sector transfer damage when the operator omits the checksum.

The fourth round found one defect and one overstatement. `grubslot_prepare` rounded the byte count to sectors before validating it. A count near the end of the 32-bit range could wrap to zero sectors, pass every bound, and write beyond the buffer. The current caller cannot supply such a value, but the public function accepts one. It now validates the byte count first. The host test covers the wrap, exact capacity, one byte beyond capacity, and an exact whole image.

The overstatement concerned the disk-layout check. The code and prose claimed that a selector naming neither tail identified a disk not built by this project. A conventional GRUB installation also places its core image at LBA 2 with a length below 1023, so it passes. The review described this as destructive to an unrelated disk, but `/dev/grubcore` is published only when `mbr_gap_clear` finds no partition overlapping LBA 1 through 2047. A disk whose first partition starts at LBA 63 has no window. On a conventional GRUB disk that passes, installation writes the unused half of the region and selects it while retaining the previous image. A proposed versioned layout marker was therefore not added; it would create a new on-disk format to prevent a non-destructive result. `utils/grubslot.h`, the refusal diagnostic, and both Readmes now state only the validation the code performs.

The fifth round examined the same boundary further, and one part required a change. GRUB reads twelve-byte blocklist records descending from 0x1f4 until it reaches a record with count zero. A loader spread across several extents can therefore name sectors the implementation had not examined. Project-built images and contiguous embeddings from grub-mkimage use one record; the installer now refuses a second, with a host test for the case. The other concern remains valid but unchanged: a post-MBR region without a partition may contain data absent from the partition table. Preventing use of such data would require an ownership marker and a new on-disk format deliberately omitted by the plan after four reviews. The tool header, `utils/Readme.md`, and `tools/Readme.md` now state that `bootctl grub` claims LBA 2 through 2047 on the target disk. This milestone seals a disk built by the project, where that region belongs to the tool.

The sixth round found that the one-record rule applied only to the running image, not the candidate. The test fixture concealed the omission: `make_core` filled the terminator with 0x5c5c, so every successful install in that file used a candidate that GRUB would interpret as having a second extent. The fixture now writes a zero terminator. `grubslot_patch` refuses a candidate with a second record, and a test requires refusal before any write. Candidate checks now validate the segment before the record list, so a non-core file receives the appropriate diagnostic instead of a bad-extent error. This round also corrected the phase summary, which still claimed that `grubslot_active` rejects disks not built by the project even after the fifth round disproved that statement.

## Phase F: networking, one interface ABI and two clients

Phase F was implemented as six separately committed steps in this order: packet sockets and `recvmsg`, which a DHCP client requires; interface ioctls, address-change cleanup of ARP and affected TCBs, and `rach2.ip=none`; connected UDP and implemented semantics for `SO_BROADCAST`, `SO_BINDTODEVICE`, `SO_REUSEADDR`, and `PACKET_AUXDATA`; the BusyBox patch, configuration, and stamp prerequisites for `udhcpc`; the in-house client, `/etc/network/interfaces`, and generated `/etc/resolv.conf`; and finally the harnesses and Readmes.

Step 1 implements packet sockets. `ip_rx` drops datagrams not addressed to this machine, including the replies expected by a DHCP client. `ethernet_rx` now offers each IPv4 payload to bound packet sockets before `ip_rx` checks the destination. With `AF_PACKET` and `SOCK_DGRAM`, the interface hides the link header in both directions: receive returns an IP packet, while send accepts one and prepends an Ethernet header using the MAC from `sockaddr_ll`. Opening the socket requires effective uid 0. Socketcall 17, `recvmsg`, was added because the client uses `recvmsg` instead of `read` to query `PACKET_AUXDATA`. The kernel returns `ENOPROTOOPT` for that unsupported option, after which the client verifies the UDP checksum itself.

The vendored BusyBox source defined the required ABI. `networking/udhcp/packet.c` and `dhcpc.c` identify every call and structure. The first implementation used incorrect `sockaddr_ll` offsets, which the probe detected.

Proof: `make packet-test` performs a real DHCP exchange. The probe constructs a discover with a correct IP header checksum, sends it to the broadcast MAC, and waits for an offer from SLIRP. A reply requires a well-formed transmitted frame, while its destination of 255.255.255.255 verifies delivery before `ip_rx` discards it. The probe waits with `poll`, as the client does, requires matching `select` readiness, and tests refusals for a stream packet socket, a non-IPv4 EtherType, `PACKET_AUXDATA`, a nonexistent interface, a non-MAC destination, and an ordinary user. Three mutations fail: removing the `ethernet_rx` hook, removing the root check, and hiding the queue from `poll`.

### What the review changed

The first review round produced five confirmed findings. `packet_send` called `ethernet_tx` without `net_lock`, allowing a receive interrupt to transmit through the shared assembly buffer while it was being filled. The single datagram slot used elsewhere was insufficient because packet sockets receive all traffic: one interrupt can drain several frames, and unrelated traffic after an offer could replace it before the client runs. Each bound socket now has an eight-entry queue, drops the newest packet when full, and reports the condition once. `do_recvmsg` consumed a datagram before validating the caller's iovecs, so an invalid pointer discarded a message before returning the fault. The reported AF_INET sender occupied 8 bytes instead of the 16 required by `sockaddr_in`. The probe also depended on an ICMP reply that `ip_rx` would accept normally, so it proved neither packet transmission nor the special receive path.

The second round found four more defects. `poll` and `select` checked `recv_full`, which the packet queue never sets, so neither reported packet-socket readiness. Since `udhcpc` polls before reading, this would have failed on the laptop. The probe now uses the same wait instead of sleeping. `recvfrom` fabricated an AF_INET sender of 0.0.0.0 port 0 for a packet socket and validated the writeback only after consuming the datagram. `recv` refused every datagram socket, disagreeing with the other three receive interfaces about queued data. A zero-capacity `msg_name` also failed to report the actual length.

The work also exposed an existing `select` difference outside this step's scope. Rach2 reads and writes the complete 1024-bit `fd_set` regardless of `nfds`, while Linux accesses only the bytes required by `nfds`. The tree's callers use musl's full-sized `fd_set`, so they are unaffected. A Linux-specific program that supplies a smaller object could have adjacent memory overwritten. The probe did so until it was corrected.

The third round found two `recvfrom` defects, one older than this step. A blocking receive interrupted by a signal reached address writeback with uninitialized `src_ip` and `src_port`. It could expose stack data or report `EFAULT` for an operation whose real result was `EINTR`. The precheck also validated the caller's complete declared capacity, although writeback uses at most sixteen bytes and writes none for a packet socket. It read that capacity again after consuming the datagram, allowing an overlapping receive buffer to change the value. The implementation now reads the capacity once and determines the writeback size before consuming data. A failed receive writes nothing. The probe requires an empty socket to leave all address arguments unchanged and a successful packet receive to report zero address length without writing an address.

The fourth round found the same capacity defect in `recvmsg` after it had been fixed only in `recvfrom`. An AF_PACKET receive with a large unmapped `msg_name` was refused even though no address would be written. Both paths now determine the address length before receiving and validate only the bytes they will write. The probe also treated a child killed by a signal as a child that was refused because both left a zero exit byte. It now checks the signal first.

Step 2 implements interface configuration. A DHCP client assigns the address after boot, so `rach2/sys/network/netif.c` implements Linux's `SIOCxIF*` ioctls on a socket descriptor: name by index, index, hardware address, MTU, and get and set operations for the address, netmask, broadcast address, and flags. `SIOCADDRT` and `SIOCDELRT` manage the default route. Any user may read the state, but only root may change it. BusyBox issues these ioctls through an `AF_INET/SOCK_RAW/IPPROTO_RAW` descriptor. The kernel permits ioctls on that descriptor and refuses every other operation because it has no raw IP path. `rach2.ip=none` joins an explicit address and an absent option as the third boot-line state, and the sealed image uses it.

An address change also updates dependent state. Under `net_lock`, `netconf_set_address` clears the ARP cache, aborts every TCP connection using the old address, and removes a gateway outside the new subnet. Listening sockets remain active and follow the interface address. `ip_rx` accepts both broadcast forms used by DHCP replies and delivers broadcast traffic only to UDP.

Proof: `make netif-test` performs two boots. The first reads all reported interface state, checks every refusal, and changes the address while a connection is open. It requires that connection to end with `ENETRESET` through a write, a blocking read, a nonblocking read, and `SO_ERROR`. The listener must accept a new connection on the new address. The test then takes the interface down, verifies that both reception and transmission stop, and restores the state. The second boot starts with `rach2.ip=none` and configures the interface from an empty state in `ifconfig` order. Four mutations fail: omitting connection notification, refusing an address before a mask exists, hiding the result from `poll`, and removing the receive guard.

### What the review changed

Thirteen review rounds produced twenty-five confirmed findings. Four would have prevented remote work on the laptop.

`rach2.ip=none` initially had no valid transition to a configured state. The address ioctl supplied the current zero mask, while the mask ioctl supplied the current zero address, so each rejected the other. `ifconfig` sets these values in separate calls. An address set before its mask now temporarily uses the classful mask implied by its first byte. An address that becomes a host only under a wider mask remains inactive until the real mask arrives. If the client dies between calls, the machine therefore does not answer for a network address. Reapplying the current pair changes nothing because DHCP repeats the same `ifconfig` calls on every renewal; resetting connections on each renewal would drop all sessions at the lease interval.

The kernel refused `AF_INET/SOCK_RAW/IPPROTO_RAW`, the first descriptor opened by BusyBox's client, so the client would exit before configuring anything.

Address changes destroyed listeners. Dropbear would stop accepting connections after the first lease without receiving an error.

`SIOCADDRT` without a gateway cleared the active gateway, so `route add default dev eth0` could remove the route used to reach a remote machine. `SIOCDELRT` reported success when no route existed. BusyBox's lease script removes old routes until removal fails, so it would loop forever instead of applying the lease.

The other findings were: no `net_lock` around the transition; leaked half-open passive blocks after an address change; an unconfigured machine claiming 0.0.0.0; broadcast admission allowing echo requests and amplification to a forged sender; reversed `SIOCGIFNAME` behavior; `IFF_UP` disabling transmission but not reception, while also disabling netconsole when it was needed to diagnose misconfiguration; routes ignoring their named device; the implicit subnet route claiming successful removal; and a connection reset reported as a permission error because a closed connection returned a bare -1.

One review suggestion added a harness feature. Proving that a down interface receives nothing requires an external frame, which the harness could not send. `tools/harness.py` now supports a `udp=PORT:TEXT` step, and the test forwards a host port to the guest. Later DHCP tests use the same feature.

Step 3 implements the ordinary datagram-socket behavior required by DHCP. `connect` records a peer, after which `write` and `send` target that peer and incoming datagrams from other senders are ignored. `AF_UNSPEC` disconnects it. A datagram requires `SO_BROADCAST` before it may reach every machine on the link. `ip_tx` then uses the broadcast hardware address instead of routing it, and only UDP may use this path. `SO_BINDTODEVICE` accepts the complete `struct ifreq` passed by BusyBox and validates its name through the ioctl helper. `SO_REUSEADDR` allows two sockets to share a UDP port so a renewal can be sent while the listener remains bound; the reply is delivered to the socket connected to its sender.

Proof: `make dgram-test` captures every frame, and `tools/bcast_check.py` requires one addressed to `ff:ff:ff:ff:ff:ff`. Removing the broadcast path from `ip_tx` fails this check, so a successful return alone cannot satisfy it. The harness injects a datagram from an external source address that loopback cannot produce, which tests sender filtering.

### What the review changed

The review took seven rounds. The first produced eight findings, including one that prevented DHCP entirely. `SO_BINDTODEVICE` rejected arguments longer than an interface name, while BusyBox passes a complete `struct ifreq`, so both client sockets would fail before transmission. `ip_tx` also broadcast every protocol instead of limiting the path to UDP. A connected socket rejected `send` without an explicit address, and `getpeername` did not report its peer. The datagram limit was 1500 although a UDP payload may be 1472. The peer tuple was published without locking against receive interrupts. `AF_UNSPEC` required a complete address. `getsockopt` did not implement `SO_BINDTODEVICE` and reported a length for data it had not written. The probe proved neither the broadcast nor its sender filter.

The second round found three defects, including a new one: connect with `AF_UNSPEC` on a stream socket used six bytes of its stack as the destination and could transmit them. The third round found that holding the network lock during ephemeral binding let any user stop interrupts by filling the eight-slot table and connecting another socket. The fifth found that `SO_REUSEADDR` remained only a stored flag, preventing the client's renewal. Correct implementation required unbinding by socket and selecting between two sockets sharing a port.

The sixth round found a correction that an unrelated `git checkout` had reverted. It also required a credential rule for shared ports, exposing an older defect: an ordinary user could bind below 1024 and take a DHCP or SSH server port. The new privileged-port rule exposed a self-build assumption in the seventh round. The in-house `httpd` ran as an ordinary user on port 80. `httpd` now accepts a port argument, and the test uses 8080 instead of weakening the rule.

Step 4 builds BusyBox's `udhcpc` against the ported musl using the first patch carried by this vendored tree. In `networking/udhcp/dhcpc.c`, `<linux/filter.h>` is needed only by a BPF block that upstream has kept inside `#if 0` for years. `<linux/if_packet.h>` is replaced by musl's `<netpacket/packet.h>` plus the auxiliary-data structure and status bit missing from that header. Copying the Linux headers was attempted but pulled in four more dependencies. `BUSYBOX_STAMP` depends on both the patch files and their directory, so edits, additions, and removals all invalidate the build.

`ifconfig` is enabled, while `CONFIG_FEATURE_IFUPDOWN_IP` is disabled because it selects the `ip` backend for `ifup` and every `ip` applet is disabled. `/etc/network/interfaces` selects `dhcp`. The image contains the four hook directories processed by `ifup` through `run-parts`. `/usr/share/udhcpc/default.script` applies a lease as an address, route, and resolver using the ordinary administrative commands.

Three prerequisites were required. `execve` now honors a script's first line because the DHCP client executes its lease script directly; previously only an explicit shell could run it. `/proc/net/dev` was added because `ifconfig eth0` otherwise reports that the interface does not exist. Init clears `/var/run` at boot because `ifup` records configured interfaces there, and this file system has no other reboot-cleared runtime storage.

Metal init brings up the network after starting console login and before starting the SSH server. A network failure therefore leaves local login available. Init performs this step only when the boot line assigns address ownership to userland through `rach2.ip=none`. The sealed disk now uses that token instead of the fixed 192.168.0.105 address.

Proof: `make dhcp-test` has five parts. A RAM image obtains a lease and must finish with the assigned address, netmask, broadcast address, resolver, and default route. A captured off-subnet packet must use the gateway's hardware address, proving the route from the wire. Two disk boots repeat the configuration while `/var/run` persists between them. An unattended metal image must transmit from the leased address after an SSH-port probe; the lease log alone is insufficient because BusyBox prints it before applying the script. A boot with an explicit command-line address must retain that address even when a server offers another. On a network without a server, the client must enter the background and continue requesting while console login remains usable.

### What the review changed

Two reviews covered this step: five Codex rounds and a four-lens review whose findings were each assigned for attempted refutation. All fourteen findings were confirmed.

Two defects could have left the sealed machine unreachable. The client used `-R -n`, so it stopped after three unanswered discovers; a switch that had not started forwarding would require a visit to the laptop. Init also ran `ifup -a` on every metal boot. On a machine with a command-line address, `deconfig` would remove that address and, without a server, never replace it.

Three test assertions were invalid. The unattended run checked only the lease line, which BusyBox prints before executing the script, so an image without the script passed. The no-server run checked a login prompt printed before network configuration began. A connection attempt to a forwarded port also proves nothing because QEMU's user network accepts the host side before determining whether the guest is listening. The corrected tests inspect guest traffic on the wire.

The remaining findings were: the lease script invented a /24 when a lease omitted its netmask, retained the previous resolver when a lease provided none, ignored all writes while constructing the replacement, and gave no diagnostic when no router was supplied; a damaged stored name containing a slash could escape the `/var/run` cleanup directory; a shebang filling the buffer was truncated instead of refused and could name another existing interpreter; a non-canonical interpreter such as `#!/bin/../dev/x` bypassed the restriction on execution from synthetic mounts; `/proc/net/dev` printed counters as signed values; the style checker ignored `.script` files; the BusyBox Readme called the tree unmodified and omitted its patch from the update procedure; and two documentation blocks described the wrong functions.

This step also exposed a harness hazard. If a success token appears intact in the command, the harness can match the command's echo and let a mutation pass. The static-address case did so until the token was split as `STATIC""KEPT`.

Step 5 implements the same function in project code for images without BusyBox. `utils/dhcpc.c` constructs the complete IP, UDP, and BOOTP messages with both checksums and sends them through the packet socket from step 1. A server broadcasts its reply to an unconfigured machine, which cannot receive it through the IP layer. The client performs discover, request, and acknowledgement; applies the lease through the step 2 ioctls; replaces `/etc/resolv.conf`; renews with the granting server at T1; rebinds by broadcast at T2; and removes the address at lease expiry. `utils/netup.c` provides the same role as `ifup -a` for this image. It reads `/etc/network/interfaces`, starts the client for a `dhcp` stanza, and applies a `static` stanza directly. With no shell or lease script, the client applies its own lease.

Each program separates system-call code from host-tested libraries. `dhcpmsg.c` implements framing, checksums, option parsing, and lease arithmetic. `netcfg.c` parses the interfaces file. `ipcheck.c` contains the validation shared by both programs because an administrator's stanza and a server's lease require the same decision. This follows the `bootslot.c` and `bootctl` division: inputs from the wire and an administrator-edited file need direct malformed-input tests that normal producers will not generate.

`tests/host/dhcpmsg_hosttest.c` creates messages a server will not send: truncated options; an option extending beyond the message; a code without a following length; no end marker; an incorrect transaction id; another machine's hardware address; no magic cookie; a fragment; each invalid header length and port; and both broken checksums. Each message is copied to a heap allocation of its exact length before parsing. This makes bounds checks falsifiable. In the client's 576-byte buffer, an option extending beyond the received message can still be read safely and fail later for another reason, allowing deletion of the intended guard. The exact allocation turns the same deletion into an AddressSanitizer failure. Nine of eleven mutations are detected by their intended diagnostics. The two initial frame-length guards mask each other, so the three-byte frame requires removing both. `tests/host/netcfg_hosttest.c` applies the same method to the configuration file, with a distinct diagnostic for each refusal so a mutation cannot pass by reaching another check.

The boundary test found a defect before review. An interface name longer than its field was stored in truncated form but later searched in full, making the stanza unreachable. Truncating both values would make distinct long names with the same prefix identify one interface. Since no interface can have such a name, the parser now rejects the file.

Only one client may control an interface. Its claim file is `/var/run/dhcpc.INTERFACE.pid`, created with `O_CREAT|O_EXCL` and filled through the same descriptor so normal creation never exposes an empty file. Readers treat an empty claim as busy. A claim for a dead process is reread before removal and replacement. When backgrounding, the parent publishes the child's pid before exiting. The client rereads the claim before configuring the interface and before applying each renewal. If it names another live client, the current client leaves the interface unchanged. If the file has disappeared, the current client recreates it because the address still belongs to its lease; exiting would leave no process responsible for removing that address. This is a claim check, not a lock. The kernel grants every `fcntl` lock unconditionally, so the contract permits one administrator at a time, like `bootctl`. There is no configure-once mode. A client that exits after applying a lease would leave the address active after expiry, possibly while the server assigns it elsewhere. TERM, INT, and HUP set a flag; poll returns, and the client uses the ordinary cleanup path. `dhcpc -i eth0 -k` signals the claimant and waits for the claim to disappear before a fixed address is applied. os-simple needs this operation because it has no `kill` command.

Rebinding remains outside boot-test coverage. QEMU's server always answers the unicast renewal, so no test reaches T2. Source review and host-tested timers provide the evidence for rebinding. The twenty-second lease permits a real renewal during the machine test.

Proof: `make dhcp-test` gained two `/16` boots. In the first, os-simple obtains a lease with its own tools. The `/16` makes the netmask observable because the classful mask for a 10.x address is /8. A client that applied only the address would ARP for 10.200.0.1, while the capture requires the packet to use the gateway. The test then stops the client and requires the gateway to stop answering, runs `netup -a` to obtain another lease, and requires the gateway to answer again. These checks prove removal of the address and release of the claim. The second boot caps the lease at twenty seconds so renewal occurs during the run. It requires exactly one address request because a client that repeatedly expires and discovers can print the same lease line without renewing. Removing unconfiguration from the signal path leaves the gateway reachable after the stop and fails the run.

### What the review changed

Fifteen review rounds produced forty-eight confirmed findings. Three required redesigning the client.

Lease timers had three errors that could stop renewal. The first implementation started the lease when the acknowledgement arrived, excluding time spent in a slow exchange. It ignored the server's T1 and T2, accepted a pair that was out of order or beyond expiry, and applied either default only when both values were absent. It converted a zero-second lease and an acknowledgement without a lease time into a week. A renewal without a lease time also moved the boundaries. The lease now begins with the request. Server-provided boundaries are accepted only in order and within the lease. An initial acknowledgement without a usable length is refused. A renewal without one leaves the existing boundaries unchanged and is treated as no answer; otherwise a rapidly repeated invalid answer would trigger an immediate new request.

Seven eighths was calculated as `(seconds / 8) * 7`, producing zero for leases shorter than eight seconds. Half of a one-second lease is also zero, so the client would rebind immediately and repeatedly. `DHCP_MIN_LEASE` now sets a ten-second minimum, below which two boundaries and an expiry cannot be scheduled. Shorter leases are refused. Seven eighths is calculated as `(seconds * 7) / 8`.

The initial claim file recorded ownership without enforcing it. Two clients starting over a stale claim left by a power loss could both observe a dead process, remove the file, and take control. Claims are now created with `O_CREAT|O_EXCL`, and the pid is written through that descriptor. A stale claim is reread immediately before removal. Every client verifies that the claim still names it before configuring the interface or applying a renewal. A loser in the race therefore exits without disturbing the winner. This remains a check rather than a lock because the kernel grants all `fcntl` locks unconditionally. The contract, as for `bootctl`, permits one administrator at a time.

Two corrections introduced new defects. Before configuration, the code treated a missing claim like one owned by another client and exited. Manual removal could therefore leave the address active without a client responsible for it. A missing claim is now recreated. The stop path also removed stale claims without the reread used by acquisition, allowing it to delete a claim taken concurrently by a new client and make that client unfindable.

Background handoff had three defects. If the parent could not publish the child's pid, it returned failure while the child continued without a usable claim. It now stops the child and removes the claim. A stop signal between fork and publication set only the parent's flag; the parent then published and exited while the child retained the lease. The parent now forwards the signal, and `dhcpc -k` follows a claim that changes ownership while waiting. If the parent died in that interval, the child retained the lease under a claim naming a dead process. `dhcpc -k` could remove the address and claim, report success, and leave the live child able to renew or remove a later address. A pipe now connects parent and child. The child begins holding the lease only after the parent publishes it; end of file means the parent died, so the child releases the address and exits. Three review rounds corrected that branch. First the child cleaned up without checking claim ownership and could remove a successor's address. Then the parent removed the claim before the child checked it, causing the child to leave the address configured. Finally, the child removed the claim even when address removal failed. The final rule is uniform: remove the address first, and remove the claim only when the address was removed, because the claim is the only record of which address belongs to the client.

Three findings affected recovery from failure. Signal handlers were installed after creating the claim, so a signal in that interval took its default action and left an ownerless claim. `netup` treated an existing empty claim as free, although that is the transient state during claim creation. `netup` also stored a failed state snapshot as zero, allowing rollback to set 0.0.0.0. It now refuses to modify the interface when it cannot read the original state. It no longer removes the old default route before adding the replacement because setting a gateway already replaces it, while a failed addition after removal leaves no route.

Three failure paths terminated the client and could strand a remote machine. An acknowledgement with a non-contiguous netmask or an off-subnet router was applied field by field until the kernel refused a value, leaving a partial configuration before exit. An unapplied renewal and a resolver-write failure also ended the client. `ipcheck.c` now validates a complete lease before any field is applied, using the same rules as `netup` uses for static configuration. An unusable lease is treated as no response, and the client continues requesting. If application fails, it unconfigures the interface and resumes requests, remaining in the background when requested.

The one-shot mode was removed. `dhcpc -1` applied a lease and exited, leaving no process or timer to remove the address at expiry. The machine could continue answering for an address reassigned by the server. Nothing used the mode.

Claims now record the address as well as the pid. A client killed without cleanup leaves an active address, and only its successor can know which one to remove. The client records the address before configuring it. A takeover or `dhcpc -k` for a dead client removes that address only if the interface still has it, since an administrator may have assigned another. Cleanup removes the address before the claim because the claim supplies the identity needed for removal. Updating a claim writes a neighboring file and renames it over the old one. The kernel lacks `ftruncate`, while rewriting with `O_TRUNC` could leave an empty claim if the client dies at that instant. Readers must treat an empty claim as busy, so such a state would prevent startup and lose the address identity.

The remaining findings were: rebinding sent one request and then remained silent for the final eighth of the lease; SIGPIPE was ignored only after backgrounding, so writing a lease line to a closed terminal could kill a client holding an address; messages were shorter than the 300 octets required by RFC 1542 and could be dropped by relays; all sockets remained open across backgrounding; `netup` had no rollback; `-l` could increase the one-week limit instead of only reducing it; a static stanza without a netmask could remove the current address and report success; and the interfaces file was parsed in one pass, causing a stanza before its `auto` line to be ignored.

Step 6 tests the configuration file's static method. The earlier steps obtain addresses from a server, but the milestone ends with a machine using a fixed address. Two additional boots cover that half of the format. os-simple stages `/etc/network/interfaces.static` beside the active file because it has no editor; copying the prepared stanza is its only update method. The test copies it, verifies refusal while the DHCP client controls the interface, stops the client with `dhcpc -k`, applies the stanza, and reaches the gateway from the fixed address. os-real writes the same stanza through its shell and applies it with BusyBox's `ifup`. Both use a `/16` and are checked from captured traffic. The `/16` makes the netmask observable. On the original 192.168 network, the requested mask matched the classful default, so ignoring the directive looked correct. On a 10.x network, the capture must show an off-subnet packet addressed to the gateway. Two mutations fail independently. Removing the netmask ioctl from `netup` makes os-simple ARP for 10.200.0.1 without sending the packet. Removing the `netmask` line from the os-real stanza prevents BusyBox's `ifup` from configuring the static address, so no packet uses it.

The review found an unrelated menu defect. The sealed disk has two entries named "static address", but they used the ordinary command line. Step 4 placed `rach2.ip=none` in the tokens shared by every entry, while the kernel rejects a command line containing two address values. Appending a fixed address would therefore leave those entries unconfigured. Network tokens are now split between ordinary and static entries, and `mkdisk_grub.sh` refuses any menu entry containing `rach2.ip` more than once. The sealed disk's static entries use 192.168.0.105/24 through 192.168.0.1 by default, matching the metal stick, with build-time overrides. This split exposed another defect. GRUB's `fallback` selects an entry number, and the stub's single value sent every failed trial to the ordinary fallback. On a network without a server, a failed static trial would therefore fall back to an unconfigured machine. Each static entry now sets its own fallback. The saved default also accepts static entries; previously `bootctl` accepted `saved_entry=candidate-static`, but the stub recognized only `candidate` and selected DHCP on later boots. `make menu-test` uses an already corrupted candidate to verify both changes. It arms a static trial, saves the static default, and requires the static fallback in both cases. Removing either line fails the test.

The final defect was in the ids written by `bootctl` during install and commit. It always wrote `candidate` and `fallback`, regardless of the running pair. Installing a kernel from a fixed-address entry could therefore return the machine to DHCP and leave it unreachable on a network without a server. The library now receives the running pair and appends `-static` when appropriate. `bootctl` determines the pair from the address in `/proc/cmdline`, the only difference between them. Commit detects an armed trial by key instead of matching one value, so both pairs work. `tests/host/bootctl_hosttest.c` installs and commits from each pair and checks the ids written. `make install-test` continues to pass the ordinary-pair loop on a booted machine.

## Phase G: recovery and whole-disk reflash

Repair requires a kernel booted outside the file systems being rewritten. Partition 3 contains that recovery build, whose `/dev/disk0` accepts whole-disk writes, and a RAM root based on the metal profile with two repair programs. Menu entry 5 starts them with `set root=(hd0,msdos3)`. Recovery mounts no file system from the target disk, so it can replace any partition.

`reflash DEVICE OFFSET BYTES` receives an image through `ssh`: `gzip -c root.img | ssh machine 'gunzip | reflash /dev/disk0 OFFSET BYTES'`. The caller supplies the length because it is unknown until a stream ends, by which time the tool could already have written to an invalid region. With a declared count, the tool measures the device, checks offset and count by subtraction, and reads no input until the range is valid. `utils/sectorcopy.c` contains the device-independent arithmetic and converts arbitrarily sized stream reads into whole-sector writes. The stream must end at the declared length. A short stream would otherwise leave a mixture of the new image and previous contents.

The kernel publication rule also changed. The everyday build publishes the whole-disk window only with `rach2.diskdiag`, preventing accidental raw access. The recovery build publishes it on every boot. Its menu entry is fixed when the disk is built, and recovery cannot depend on a diagnostic token that may be absent when the disk needs repair.

Proof: `make drill-test` performs five boots from a disk with the sealed layout's three partitions. First, the recovery kernel boots without a command-line request for the device and must publish it. Next, the ordinary shipped image must refuse both `reflash` and shell redirection to the device; the first sector is then compared byte for byte to prove that no write occurred. With the machine stopped, the test destroys the root's first sector. The ordinary entry must fail to mount and must not reach login. The environment block then arms recovery as an operator would from the console. Recovery boots from partition 3, obtains an address, and accepts SSH without console input. It refuses a range beyond the device before reading a byte. The complete 16 MiB root then arrives through one pipe in about two seconds because gzip reduces the mostly empty partition to a few tens of kilobytes. Recovery reads back the region through the same device and compares its hash with the host's value. The final ordinary boot must mount the root and offer login. Two mutations fail: removing automatic device publication from recovery stops the first boot, and omitting the core image's configuration prevents boot after partition 1 is destroyed.

### What the review changed

The first review round produced five findings. Two concerned recovery reachability rather than repair behavior.

Recovery still depended on the boot partition it was meant to repair. The core image's prefix named only partition 1, so damage there removed the stub, menu, and every route to partition 3. The core now embeds two ordered `configfile` commands for the menus on partition 1 and partition 3. Its parser has no `if`, but the first menu does not return after a successful boot; the second command runs only when the first file cannot be read. Since `configfile` does not change the root, the partition 3 file sets its own root. The drill destroys partition 1's boot sector and both FATs, then requires a login that no file on partition 1 can provide.

Recovery originally had one DHCP entry. Without a server, it obtained no address and therefore offered no SSH. It now has the same DHCP and static pair as the ordinary kernel. Existing entry numbers remain fixed, and the static recovery entry is 6.

`bootctl` initially could neither select recovery nor operate from it. `recovery` and `recovery-static` now belong to the entry allowlist, allowing a working machine to enter the repair image. Both recovery entries include `rach2.slot=recovery`, telling `bootctl` that neither ordinary slot is running and either may be installed. Only a running candidate may still be committed.

Two narrower defects remained. Concurrent reflashes over overlapping ranges could both complete and report success while producing neither image. `/dev/disk0` now permits one writer, as `/dev/grubcore` already did, while allowing concurrent readers. The final read used to prove exact stream length also ignored errors, allowing success without completing the check.

The writer-reservation mutation exposed a build dependency error. `make authority-test` did not depend on its boot image, so rebuilding a probe in `tests/` did not update the guest and the old probe passed. The target now depends on `test-image`.

The second round produced four findings; two showed that the first round's corrections were incomplete. The core image built for `bootctl grub` lacked the embedded configuration, so an ordinary boot-loader update silently removed recovery. The two `configfile` commands now live in `tools/grub-embed.cfg`, which both builders consume. Each refuses a configuration that cannot reach partition 3. The check reads the configuration source because compression removes the plain text from the built image.

The writer reservation applied to only one device name, although all three raw windows refer to one disk. The boot partition lies within the whole-disk window, and the loader region precedes it, so reflash and installation can address the same media through different nodes. One reservation now covers all three. Over SSH to a real disk, the drill keeps `/dev/disk0` open for writing and requires a second writer through the loader window to fail; the loader writer succeeds when run alone.

The other two findings concerned fallback and manifest identity. A failed static-recovery trial selected the DHCP recovery entry, which would be unreachable on the network requiring the static choice. It now sets a static fallback like the other fixed-address entries. The image checker reused the metal profile's trusted set without pinning the two recovery programs, allowing the manifest to point `reflash` elsewhere. Both added programs are now named, and repointing either fails the build.

The third round found four defects. The first concerned GRUB's handling of `root`: it is not restored when an entry fails. Recovery entries set it to partition 3, so their fallback searched for the ordinary kernel on the same partition and failed. Every entry now sets its own root. The drill destroys partition 3 on a copy, arms recovery, and requires the machine to boot a kernel from partition 1. Removing the root assignment fails this case.

The embedded-configuration check used substring matching, so a commented command passed. Both core builders now run `tools/check_grubembed.py`, which requires exactly two uncommented `configfile` commands, with partition 1 before partition 3. `bootctl` accepted `saved_entry=recovery`, although the stub considers the saved default only for the four ordinary kernel entries. Saved defaults now use a smaller allowlist. The review also found that `tools/grub-embed.cfg` was untracked. Since both builders read it, a clean checkout would fail while building the first core image.

The fourth round found another checker defect. It examined two lines without parsing the complete command list, while GRUB treats `;` as a separator. A hidden partition 3 `configfile` command before the expected pair passed and would direct healthy machines to recovery. The checker now requires the complete command list to contain only the two expected commands. Its nine-case selftest runs under `make verify-style` beside the kernel checker.

The fifth round covered a combined failure: the boot partition is damaged enough to reach partition 3, and no DHCP server is available. Although its menu has a static entry, unattended recovery selected no address. The recovery image now handles this without menu input. Its `interfaces` file runs `krffs/rescue-static.script` after `ifup`; the script waits through one lease attempt and applies the fixed address if no lease arrives. The static menu entries use the same address. Because the script contains its own copy, the sealed-disk build refuses disagreement between script and menu. The drill boots recovery on an otherwise empty hub port and requires the fixed address. Removing the script check fails the case.

The sixth round traced the next operation after that rescue. The machine runs on a fixed address even though its boot entry requested DHCP. If `bootctl` inspected only the command line, it would arm a newly installed kernel in the DHCP pair, and rebooting without a server would lose the address. The script now records its action in `/var/run`, and `bootctl` reads that marker with the command line. The drill requires the tool to report the fixed-address pair; removing the marker check fails.

The seventh round found that the failed DHCP client continued requesting. A later server response could replace the fixed address while the marker still selected the fixed pair, causing a later installation to arm the wrong entries. The script now stops the client and waits for termination before assigning the fixed address. If the client does not stop, the script refuses the assignment. The drill requires both the fixed address and the absence of a client.

The eighth round found that the marker could outlive the fixed address. An administrator could return a rescued machine to DHCP while the marker still selected static entries, causing a later installation to use the wrong pair. The lease script now removes the marker when applying a lease. One limitation remains documented: manually assigning an address with `ifconfig` after rescue leaves the marker present, so an installation selects the configured static entries rather than the address entered by hand.

The ninth round found a destructive error in the published repair command. Its example offset named the start of the boot partition instead of the root, one MiB too early. A valid image could therefore be written to the wrong location and reported as successful. Both instructions now use the correct values and identify their source. `/proc/disk` reports each partition as `partN TYPE START_LBA SECTORS`, and the drill reads the extent from the disk's partition table instead of reconstructing it from build parameters.

The tenth round found that a one-boot trial was insufficient for repair. Rewriting a root takes several minutes. If power failed during the operation, the next boot returned to a kernel unable to mount the partial root, while the healthy boot partition prevented fallthrough to partition 3. Recovery may now be the saved default as well as a trial, so an administrator can retain it until repair completes. `debug` remains bootable but cannot be saved because its kernel shell has no init or remote access. The drill saves recovery, stops the machine, and requires unattended recovery with SSH on the next boot.

One finding was documented instead of implemented. If the root cannot mount while the boot partition remains healthy, the machine stops without remote access. GRUB reads the menu from partition 1 and starts the ordinary kernel. The kernel loads successfully, so GRUB fallback does not run, and readable partition 1 prevents embedded-configuration fallthrough. The drill already assumes an operator selects recovery by editing the environment block while the machine is stopped. Automatic handling requires a boot-attempt counter in the environment block and a userland success acknowledgement. `tools/Readme.md` records this separate mechanism as a limit rather than implementing part of it.

The eleventh round corrected the exit procedure. Instructions said to restore `candidate`, but a machine without DHCP uses the fixed-address entries. Following them would boot a kernel waiting for a lease that never arrives. The operator must record the previous value before repair and restore that value afterward. The instructions and drill now do so without naming a fixed entry.

## Phase H: the sealed image, rehearsed end to end

`make sealed-test` operates the artifact from `make rach2-sealed-disk` in its intended installed form: through AHCI and SSH, without console input after power-on. Other harnesses test individual mechanisms on purpose-built disks. This harness tests the complete sealed artifact. It remains separate because `install_test.sh` owns installer assertions that must not move.

The harness runs two separate copies of one artifact. The fault run covers the machine's operating life. It boots and accepts key authentication. A kernel arrives through the same connection, is installed as a one-boot trial, prints its own marker, and is committed. With the machine stopped, the candidate's first sector is destroyed; fallback boots, and that system repairs the slot. A staged menu is truncated by the host, and the stub still boots. A marked core image replaces the boot loader, followed by a second image that must use the other tail. Finally, recovery rewrites a destroyed root through one `ssh` pipe, reads it back through the same device, and hashes the stored bytes. The control run uses the same artifact without injected failures or deliberate recovery. It requires the absence of both fallback and recovery, which the fault run cannot establish.

The installed test kernel is the disk's kernel with its banner rewritten in place, the smallest change that identifies the running image. The structural checker accepts it. Its marker appears only during the trial boot.

Execution exposed three harness requirements. A command that stops the machine cannot return because its connection closes, so `reboot -f` and `poweroff -f` use short bounds and their results are ignored while the QEMU process is monitored. Without that treatment, the first run waited three minutes at each shutdown. Host-side trial selection must edit the environment block like `grub-editenv`, changing one key while retaining every other line. Recreating the block discards the saved default and causes an unrelated fallback. Recovery is identified by loading a RAM image, not by the word in the menu title, because every entry title is printed on the console.

Measured timing was 3s for preparation, 11s to the first cold boot, 26s for kernel installation and trial, 49s for candidate destruction and repair, 47s for menu tests, 111s for two boot-loader replacements, 114s for root destruction and repair, and about 13s each for the final cold boot and control run. Machine time remained below seven minutes, within the plan's twelve-to-fifteen-minute allowance; artifact construction accounts for the remaining time.

### What the review changed

The review produced five findings. The first two concerned the physical session. The checklist retained development passwords on a machine connected to a real network. Although SSH rejects root password login, the test account has a password and the image contains set-id `su`; anyone knowing the repository credentials could log in as `test` and become root without the key. The checklist now generates both passwords before building and explains the requirement. It also identified the target drive only by exclusion, allowing any similarly sized external disk. The procedure now requires exact model and serial values from the drive's label, repeats the comparison immediately before writing, and refuses adapters that report no serial.

The third finding also concerned artifact identity. Readback compared the drive with the current file in `build/images`, so a rebuild after rehearsal could compare the new artifact with itself. The procedure now records the rehearsal digest and compares every later copy with that fixed value.

The second round found that the password instruction had no effect. `gen_demo_password.sh` exits when `krffs/shadow` already exists, as it does after a build, so the checklist appeared to replace credentials while retaining the development values. The script now accepts `RACH2_REPLACE_PASSWORDS=1`, reports replacement, and the checklist uses it and explains how to restore development credentials. The write procedure also named the device separately for validation and `dd`. It is now one `set -e` block that names the node once, derives the raw path, stops at the first mismatch, and checks the artifact digest within the same block as the write.

The third round found both corrections incomplete. Credential replacement wrote new plaintext files before replacing hashes. An interruption could leave new passwords beside old hashes, and the next build treats an existing `shadow` as complete. The script now removes the old `shadow` first, so interruption leaves no credentials instead of inconsistent ones. Device checks also used `grep` over complete `diskutil info` output, allowing an empty placeholder or substring match. The block now parses model, serial, size, and external status into variables, requires each value, and compares exact strings. An unparseable field fails validation.

The fourth round found that rehearsal covered only one boot-loader transition. It installed one core image and stopped, so a defect in the reverse transition would first appear during an administrator's second update, after both GRUB and recovery depended on it. The harness now installs and boots two images and requires different reported tails. The fifth round found that a host without `timeout(1)` could hang forever after a stalled connection because the harness checks its own deadline only between boots. It now refuses to start without that command.

Two findings affected harness assertions. It assumed MBR entry 2 was the root, while the kernel selects the first partition whose first sector contains a KRFFS root node. A changed layout could therefore make the harness damage and repair an unused partition while later assertions passed. It now locates the root as the kernel does, boots the ordinary entry, and requires mount failure before arming recovery. The armed-trial refusal also checked only for a diagnostic. It now requires failure status and verifies that both saved default and armed trial remain unchanged.

The physical write procedure is the document `dev-log/25-sealed/checklist.md`, not an executable script. It requires model and serial confirmation before any write because the existing baseline describes the laptop's original 1 TB disk, while sealing targets another drive. The procedure writes 660602880 bytes, reads back the same count, compares hashes, and lists the required record. Only the owner runs this session.

## Phase I: the sweep, the record, and verification

The final pass ran `make distclean` and rebuilt from an empty build tree, verifying that the repository contains every build input. This milestone had already omitted `utils/ipcheck.c` and `tools/grub-embed.cfg`; diff review, not testing, found both. Each suite then ran separately: `make verify-style` and `make verify-host`; the probe suite; network suites (`packet`, `netif`, `dgram`, `ping`, `netconsole`, `dhcp`); disk suites (`bootpart`, `authority`, `diskpolicy`, `diskdiag`, `test-disk`, `test-disk-ahci`); account suites; `reset`, `shutdown`, and `test-addressed`; then `menu`, `grub`, `install`, `drill`, the SSH capstone, self-build, and `sealed-test`.

QEMU and Docker harnesses must run one at a time. Concurrent runs cause timeouts, and `musl/pwauthmusl` fails under that load because it retains a lock across a timed assertion. It failed once while a Codex review ran concurrently and passed immediately when run alone.

### The invariant the plan named that the menu no longer meets

The plan's "what must not be broken" list requires exactly one menu entry with `rach2.slot=candidate`. The completed menu has slot tokens on four entries, and both `candidate` and `candidate-static` use that value because both load `/boot/kernel`, the candidate slot. The intended invariant is that each token identifies the file loaded by its entry. Every entry satisfies that rule: static and ordinary entries differ only in their address configuration, while the recovery pair uses `rach2.slot=recovery` because it loads neither ordinary slot. `bootctl commit` retains its meaning by refusing unless the running kernel came from the candidate slot.

### What the closing review changed

The closing review produced three findings about the complete artifact.

The sealing procedure replaced passwords but reused the SSH login key because `gen_demo_key.sh` retains an existing pair. A tree that had built any earlier image would therefore seal the machine with its demo key. The script now accepts `RACH2_REPLACE_KEYS=1`, which the checklist supplies.

Recovery has no stable SSH server identity. The ordinary root retains the host key generated during its first boot, allowing `ssh` to pin it. Recovery uses a RAM root and generates a new key on every boot, so `ssh` reports an identity change just before a root image is sent. The generated fingerprint appears on the console for independent comparison. The checklist and `tools/Readme.md` require that comparison and explain why disabling host verification is unsafe. A fixed key in the recovery image would require a host-side `dropbearkey`, which the project does not build, so this remains a procedure rather than a staged file.

The second round found both responses incomplete. The fingerprint was read from unauthenticated netconsole broadcast, where any station can send a frame containing any value. Such a capture records traffic but does not authenticate the server. The laptop screen is now the required independent channel and the only recovery step that requires a visit. If the screen is unavailable, the instructions state the remaining risk and its limit: the image is not secret, and its stored bytes are verified by a hash read back from the drive. `check_sealednet.py` also matched two commands as text, allowing a comment or prefix address to pass. It now tokenizes commands and requires exactly one of each. The third round found that it parsed only the first command on a line, allowing an extra command after a semicolon. It now examines every segment of each uncommented line. The fourth found that unknown segments were ignored, allowing wrappers such as `command`, `eval`, subshells, or backticks. The checker now fails closed: any segment mentioning `ifconfig` or `route` must match one of three exact forms. Its thirteen-case selftest runs under `make verify-style` with the kernel and GRUB checkers.

The sealed network also represented one configuration in two forms: the menu uses a prefix, while the recovery script uses a dotted mask. `make SEALED_PREFIX=26 rach2-sealed-disk` could create a menu whose gateway was outside its subnet and a fallback using another mask. Before writing a disk, `tools/check_sealednet.py` now requires the values to agree. It also rejects a gateway that is not a host on the subnet, a network or broadcast address used as the machine address, and a script containing different values.

### Verification as separate claims

Each target ran separately against the tree rebuilt from nothing, and all passed: `verify-style`, `verify-host`, `test` (62 probes), `test-addressed`, `test-disk`, `test-disk-ahci`, `packet-test`, `netif-test`, `dgram-test`, `ping-test`, `dhcp-test`, `netconsole-test`, `bootpart-test`, `authority-test`, `diskpolicy-test`, `diskdiag-test`, `reset-test`, `shutdown-test`, `account-test`, `login-test`, `cred-persist-test`, `menu-test`, `grub-test`, `install-test`, `drill-test`, `ssh-test`, `selfbuild-test`, `tcc-test`, `make-test`, `sealed-test`.

## The hardware session

This section was added after the milestone from a session with the drive and laptop. It records exact measurements for the next machine and operator, followed by the commands that produced them. Values that can be reproduced from the tree and drive are distinguished from observations specific to this session.

The SSD contains artifact `a7f7d76e550761e69a92d7a6d18b46aa5649001e8bf14287a7040ead07361bbf`, 660602880 bytes. Reading 630 MiB from the drive produced the same hash. This was the second image written. The first was `a6155446b6eef802074d81df2e0d62835b38dfde1a4956f86f895c013cce64e9`. The second differs only by including the owner's SSH public key beside the project demo key, allowing access from an ordinary workstation without the project's private key.

The adapter prevented serial-number identification. `diskutil info` reported model `Generic` and no serial because the USB-to-SATA bridge described itself instead of the drive. The target was identified as the only attached external physical disk: USB, whole disk, 250059350016 bytes, with the layout from the previous write.

The directory's `checklist.md` requires exact agreement between the drive label, model, and serial. It therefore cannot be followed with this adapter and instructs the operator to replace it. This remained an open decision rather than a documentation correction; milestone 26 resolves it. Until then, the checklist disagrees with the approval used for these writes.

QEMU booted from the physical drive instead of the image file. It reached login in five seconds, accepted SSH key authentication, and reported `booted: candidate`, `saved_entry: candidate`, and `next_entry: absent`. The candidate began at sector 312 with 4096 sectors and 512 clusters; fallback began at 4408 with 1568 sectors and 196 clusters. The root mounted from partition 2 through AHCI with 575 files, and `tcc -static` compiled and ran a program. Three boots left the drive hash unchanged because QEMU used `-snapshot`.

On the Lenovo G50-70 through the USB adapter, the machine received `192.168.0.107` from the building router. The reservation matched `28:D2:44:7E:E0:AF`, the station address recorded in `dev-log/24-metal/baseline.md`. Netconsole captured the complete discover-to-lease exchange. The lease was infinite, 4294967295 seconds. The resolver file was generated from the lease, and `nslookup` succeeded. Root authenticated by key, the pty was `/dev/pts/0`, and a client-provided `TERM` arrived intact.

The session found but did not modify the laptop's internal disk, confirming milestone 24's requirement: `Samsung SSD 870 EVO 1TB`, serial `S74ZNX0Y927920A`, firmware `SVT04B6Q`, 1953525168 sectors, reported unmounted. Its first-sector hash remained `652ad40fe72061185cf3a69c1a9a45394f68374f409153e082b7c1fa04c107d3`, the value measured by Windows before Rach2 ran on the notebook.

Two limitations constrain this session's evidence. Only recovery entries can run while the sealed drive is attached by USB because the kernel has no USB driver and cannot access its boot drive. Ordinary entries search the AHCI bus, where the laptop's Windows disk remains installed. Every session boot used the adapter, so the laptop has not booted the ordinary root. It has no laptop `bootctl status` result or persistent SSH host key yet. This describes the session and adapter, not the drive's earlier use. Recovery creates a new host key on every RAM-root boot, so its identity cannot be pinned. The ordinary system will create its persistent key on the first boot from the SATA bay. The operator must read it from the laptop screen, the one checklist step requiring physical presence.

The session found six diagnostic programs broken by one missing file each: `ps` needs `/proc/uptime`, `top` needs `/proc/stat`, `route -n` needs `/proc/net/route`, `netstat` needs `/proc/net/tcp`, `arp` needs `/proc/net/arp`, and `df` needs `/proc/mounts`. `dmesg` fails on `klogctl`, while `stty` fails on a pty where `tty` succeeds. Milestone 27 owns these defects.

### The commands behind those numbers

These commands permit repetition of the measurements. Host commands ran on the Mac attached to the drive; machine commands ran over SSH in the booted system.

    shasum -a 256 build/images/rach2-sealed.disk
    a7f7d76e550761e69a92d7a6d18b46aa5649001e8bf14287a7040ead07361bbf

    sudo dd if=/dev/rdiskN bs=1m count=630 | shasum -a 256
    a7f7d76e550761e69a92d7a6d18b46aa5649001e8bf14287a7040ead07361bbf

    diskutil info /dev/diskN
    Device / Media Name:  Generic          (the adapter, not the drive)
    Protocol:             USB
    Disk Size:            250059350016 Bytes
    Device Location:      External
    Whole:                Yes

The lease came from a netconsole capture on the same segment (`tcpdump -i en0 -nn -e -s0 'ether proto 0x88b5'`). Netconsole is unauthenticated broadcast. It records wire traffic but does not establish the sender's identity:

    udhcpc: broadcasting discover
    udhcpc: broadcasting select for 192.168.0.107, server 192.168.0.1
    udhcpc: lease of 192.168.0.107 obtained from 192.168.0.1, lease time 4294967295

The recovery system read the laptop's internal disk over SSH, first through `/proc/disk` and then from its first sector:

    model Samsung SSD 870 EVO 1TB
    serial S74ZNX0Y927920A
    sectors 1953525168
    mounted 0

    dd if=/dev/disk0 bs=512 count=1 | sha256sum
    652ad40fe72061185cf3a69c1a9a45394f68374f409153e082b7c1fa04c107d3

`dev-log/24-metal/baseline.md` recorded the final value from Windows before Rach2 ran on the machine. That file supplies the baseline; equality with the new read is this session's observation.

### What is left, and for whom

Half of phase F's two-location boot check is complete. At home, the machine obtained an address from a real router without a boot-line change, verifying the client. The office run remains undone.

The sealing session in `dev-log/25-sealed/checklist.md` remains. It requires the physical drive. The SSD is outside the laptop on a USB adapter and contains the named artifact; the original Windows disk remains installed.

The milestone omitted the items excluded by the plan: a second root partition, configuration partition, watchdog, network boot, and redundancy below GRUB. Two additional limits were decided during implementation and documented with their owning procedures. A root mount failure with a healthy boot partition still requires console recovery because the boot path cannot detect a kernel that loaded and stopped; automatic handling needs an environment-block boot counter. Recovery also changes its SSH identity on every RAM-root boot because a fixed key would require a host-side `dropbearkey`, which the project does not build.
