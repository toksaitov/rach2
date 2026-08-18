# Milestone 25: the sealed disk

The plan is in `plan.md`, written before any code and put through four rounds of adversarial review before a line was implemented. This file records what was built, what was proved and where, what was refuted along the way, and what is still waiting for the hardware.

The goal is a laptop whose disk is written once and never removed again. Everything that changes afterwards arrives over SSH: kernels, the boot menu, GRUB itself, and in the worst case the whole disk from a recovery image.

## Phase A: the receiver window update

Milestone 24 measured bulk transfer at 0.9 KB/s on the laptop against 1.1 KB/s in QEMU, and reasoned from how close those two numbers are that the fault lay in the TCP stack rather than in the new driver or the 100 Hz poll. That reasoning was right. Its record then called the result "a property of the TCP stack", and that part was wrong: it is one defect, in one direction, with a one-function fix.

A TCP receiver has to report a window that shrank and a window that grew. This stack only reported the first. The inbound paths in `tcp.c` recompute `rcv_wnd` whenever a segment arrives, so a filling buffer was announced at once. A draining buffer was announced to nobody, because `socket_tcp_recv` and `socket_tcp_try_recv` advance `recv_head` in syscall context and sent nothing. A peer that had filled the window waited for its own persist timer before probing again, and that timer set the transfer rate.

The fix is `tcp_window_update`, called from both receive paths after `recv_head` advances. It takes `net_lock` itself and holds it from the TCB snapshot through the send, because an IRQ-side `tcp_close` detaches the TCB. It sends at most one bare ACK and only from ESTABLISHED, where the closing states have no reader making room. Whether to send is the whole of `rach2/sys/network/tcp/rcvwnd.c`: a pure function, no state and no transmit, that stays quiet until the window has grown by one maximum segment size or half the buffer, whichever is smaller. That is the RFC 1122 4.2.3.3 rule. Announcing sooner would spend a segment per byte read and draw segments just as small in reply.

Measured host to guest over SSH into `/dev/null`, same image, same QEMU invocation, with the call removed and restored:

| Bytes   | Without | With    | Rate without | Rate with |
|---------|---------|---------|--------------|-----------|
| 4096    |         | 0.361 s |              | 11.4 KB/s |
| 65536   | 60.179s | 0.291 s | 1089 B/s     | 225 KB/s  |
| 1048576 |         | 0.408 s |              | 2.57 MB/s |

The 64 KiB figure without the update reproduces milestone 24's measurement to three digits, which is what identifies the two as the same defect rather than two separate slow things.

Proof: `tests/host/rcvwnd_hosttest.c` drives the rule at ring and segment sizes the running kernel never uses, because which half of the rule wins depends on the geometry: at 4 KiB one segment is smaller than half the buffer, at 8 bytes half the buffer is smaller. Four mutations were shown to fail it: never announcing, announcing every byte, using two segments instead of one, and dropping the 16-bit clamp. The measurement above is the other half, and the column without the update is the phase's shown-failing case.

### What the review changed

An independent review of the phase raised five findings. Three were confirmed and fixed in the same commit. The bar was two maximum segment sizes, which is not the RFC 1122 rule; it is one segment or half the buffer, whichever is smaller, and the old value made the receiver quieter than it should be. The returned window was cast to 16 bits without a clamp, so a ring of 65536 would have announced zero, which this function uses to mean stay quiet, and a fully drained buffer would have announced nothing. And the new function was inserted between `tcp_is_connected`'s documentation block and `tcp_is_connected` itself, stranding the block on the wrong function.

### What is deferred, and why

The rule governs the update a reader triggers, not every window the stack advertises. The inbound paths still recompute `rcv_wnd` from free space whenever they owe an ACK, so a segment arriving after a small read can advertise a window below the bar. This predates the milestone, and the update path largely hides it: a reader draining normally announces a full-sized window before a peer has cause to probe. Correcting it means holding the advertised right edge fixed as data is accepted rather than recomputing it from free space, which changes the receive state machine that phase A deliberately does not touch, and it needs a stateful test to be proved rather than asserted. It is the next thing worth chasing in the stack.

The host test covers the rule, not the wiring: deleting both call sites leaves it passing. The wiring is proved by measurement instead, run once with both call sites present and once with them removed, and that measurement is manual. Every figure here is host to guest; the reverse direction was never slow and was not measured again. An automated guard would need a packet recorder around a live connection, which is worth building when something else needs one too.

## Phase B: the tool's new name and shape

`install-kernel` is now `bootctl`, after systemd's tool, which manages a boot partition's kernels and defaults on most Linux machines; a student who learns this one carries the knowledge off the machine. The verbs stay this project's own, because the kernel diagnostics, the Readmes and the harness already speak trial and commit:

    bootctl status         what is installed, committed, and booting next
    bootctl kernel FILE    stage a kernel into the candidate slot
    bootctl commit         make the booted trial the saved default

No alias is shipped. `--commit`, `--where` and a bare kernel path are refused, and there is a test that says so, because a compatibility spelling nobody documents has to be carried forever to be worth anything.

Three boundaries held the change to twenty-one files. The `bootslot` library keeps its name, since it is named for what it manages rather than for the tool that drives it, and two dozen of its error substrings are matched literally by a host test. `dev-log/` keeps its history as written. `make install-test` keeps its name, because the loop it proves still installs a kernel.

`status` is the phase's one new behavior, and it exists because the tool is the only window onto a disk nobody will open: the three files' sector extents, what the environment block holds, whether a trial is armed or consumed, and which slot this boot came from. It says nothing about the GRUB core image, because the device that would answer that does not exist until phase C.

Two changes were made that the phase did not strictly need, both to close hazards the plan predicted.

`prune-stale` now sweeps `build/utils/sbin` as well as the top of `build/utils`. It did not before, on the reasoning that sbin's contents are staged by name rather than by sweep. That reasoning has a hole: a manifest line left on the old name would keep staging the old binary, and `check_setid.py` compares a manifest's declared source against its own table without ever stat-ing the host file, so nothing would report it. The rename left exactly that artifact behind on the first build, which is what closed the gap.

The harness's `awk` was anchored. `tools/install_test.sh` read the candidate's first sector with `/candidate/{print $5}`, which was unambiguous when `--where` printed three lines and is not now that `status` prints six, three of which contain the word. In the shipped line order the loose pattern still happens to work, because the extra matches yield an empty field five and the shell strips the trailing newlines; put a non-extent line first and it yields a leading newline instead, and `dd` gets a malformed seek. The pattern is now anchored on the extent line's own shape, which does not depend on the order.

Proof: `make install-test` green end to end, twice through the loop on one disk, which is what shows the trial, commit, fallback and repair machinery survived the rename; `make selfbuild-test` green, which builds `bootctl` from `guest.mk` on the machine itself and so proves the guest list moved with the host list. `tests/host/bootverb_hosttest.c` covers the parser.

Shown failing: the harness pointed at the old `install-kernel: committed` string fails the run, so that assertion is reaching the tool's output rather than passing vacuously. Two mutations of the parser fail its host test: accepting `--commit` again, and tolerating a surplus operand. The anchored `awk` against a reworded extent line yields nothing rather than a wrong sector.

### What the review changed

Six findings, all confirmed. Two were defects in new code and two were in code the rename only touched in passing.

`status` called every empty environment value a consumed trial. Only `next_entry` carries that meaning; an empty `saved_entry` says nothing about a trial. The annotation is now asked for by the caller rather than assumed by the printer.

A known verb given the wrong number of operands was reported as though it were not a verb at all, so `bootctl kernel` with no path answered "not a command this tool takes: 'kernel'". Exit status was right and the message was not. `bootverb_known` now separates the two, and arity refusals print the usage line alone, as they did before the rename.

`bootslot_commit` matched the armed-trial marker as a bare 21-byte string with no leading newline, so a preserved line ending in `next_entry=candidate` would have been read as this machine's armed trial and blocked a commit that should have succeeded. The writer deliberately preserves lines it does not recognise, so such a line can exist. The pattern is now anchored the way the consumed marker already was, both bounds are checked independently so neither shortens the other's search, and a fixture carrying `x_next_entry=candidate` beside a real consumed marker proves it. This is older than the milestone; the rename found it by putting a second reader on the same block.

The parser test asserted only that `kernel` yielded some operand, not which one. A parser returning `argv[1]` instead of `argv[2]` passed it, and would have made `bootctl kernel FILE` try to open a file called `kernel`. The test now checks the value, and that mutation fails it.

The remaining two were smaller. `utils/Readme.md` still described `--commit`, named the host test by its old filename, and said pruning covered only the top level, which stopped being true in this phase. And the new parser test carried section labels inside its body, which the project's comment rule does not allow; they are gone.

A second round over those fixes found one more and refuted nothing. `bootverb_parse` and `bootverb_known` had grown separate copies of the verb list, so a verb added to one and missed in the other would compile and then report itself as no verb at all. They share one catalog now, and a test walks it: every verb the parser accepts must also be one the diagnostic knows, which fails if the two ever drift.

One finding from that round was refuted with evidence. The new fixture in `tests/host/bootctl_hosttest.c` opens with a comment introducing the case, which the comment rule reads against. That file is a sequence of eighteen such cases, each introduced the same way, and the rule is written for production bodies rather than for a test whose structure is its scenarios. Following the file is right here; restyling it is a separate cleanup, not part of a rename.

## Phase C step 1: the geometry

The sealed disk's layout is now something the build produces rather than something the plan asserts. `make rach2-sealed-disk` writes it, and every number in the plan's map was checked against the built image: 660602880 bytes, 1290240 sectors, 630 MiB; partition 1 at LBA 2048 for 131072 sectors, type 0x0c; partition 2 at 133120 for 1024000; partition 3 at 1157120 for 131072; entry 4 empty; 4096 bytes per cluster in the boot partition.

`tools/mkdisk_grub.sh` grew three knobs, each defaulting to what it always built, so no existing caller changes: the boot partition's size, mformat's sectors per cluster, and a third partition that is absent at zero. The two size knobs are not independent, and the plan says why: `bootslot_open` refuses a volume whose FAT is over 512 sectors or whose cluster count is over 65536, and a 64 MiB partition at one sector per cluster is about 130500 clusters with a 1020-sector FAT. At eight sectors per cluster it is 16348 clusters with a 128-sector FAT.

Three things had to change to make that build correct rather than merely produce a file.

The builder's chain verifier compared cluster counts against sector counts. It was right only because mformat had always chosen one sector per cluster, and it now reads the cluster size out of the volume it just formatted and divides by that. Its report names the figure, so a geometry that silently changed would be visible in the log.

The same verifier read `mshowfat` output with a pattern that matched only a run of clusters, `<a-b>`. A file that fits a single cluster prints as `<a>`, which never happened at 512 bytes and happens to `grub.cfg` immediately at 4096. It now accepts both.

`krffs/mkdisk.sh` refused any partition over 256 MiB, citing the kernel's mount window. That window became 512 MiB in milestone 24 when it moved above the region real firmware uses for MMIO, and this copy of the number was left behind. It is corrected, with the reason recorded so the next person does not have to rediscover it. This path did not block the sealed image, which goes through `mkimage.sh`, but the number was wrong wherever it was read.

The host fixture assumed one sector per cluster in fifteen places, which is why the library's cluster arithmetic had never been exercised: at one sector per cluster the multiply by `cluster_sectors` is the identity, so a version that dropped it entirely would have passed every test in the suite. `mkfatfixture.py` now takes the geometry as a parameter, `--cluster8` builds the same files at 4096 bytes per cluster, and every existing option was checked to produce a byte-identical image to before. A new case opens that volume, checks the library reports eight sectors per cluster and 512 clusters for the 2 MiB slot rather than 4096, and installs and refuses a commit there.

Proof: the sealed image built and its table read back and compared field by field against the plan; the current kernel booting that image, mounting the 500 MiB root and reaching a login with partition 3 ignored; `make install-test` green at the unchanged default geometry, its log naming 512 bytes per cluster.

Shown failing: dropping the `* cluster_sectors` from the library's cluster-to-sector conversion now fails the host test, which it did not before this fixture existed; and computing the volume's cluster count as a sector count fails it too.

### What the review changed

Four findings, all confirmed.

Partition 3 had a type byte and no filesystem. `parted`'s filesystem argument selects the MBR type and creates nothing, so the entry named FAT32 over 64 MiB of zeros, and GRUB could not have read the partition the recovery entry will need. It is now formatted, with the two directories its contents will live in. What goes into it still belongs to phase G.

The image was not reproducible. Two builds of one tree differed in the FAT volume serial, which mformat randomises, and in the creation and access dates mcopy stamps into every directory entry from the clock. Both are pinned, the second through `SOURCE_DATE_EPOCH`, which mtools honours, and two builds now produce one byte-identical image. Two inputs are deliberately left unpinned and are named as inputs rather than treated as noise: the container is the floating `ubuntu:24.04` tag, and the machine's SSH host key and password salt are generated per tree.

The new cluster case did not constrain everything the geometry made relevant. It checked the cluster-to-sector conversion but not the volume's cluster count, and a version computing that count as though a cluster were a sector passed: the wrong value is larger than the volume, so it bounds nothing, and every chain in the fixture ends well below it. The count is now asserted directly, and a second fixture at the same geometry runs a chain past the end so the count is exercised as a bound rather than as a number that is merely reported.

The owning documentation still described a two-partition disk and none of the new knobs.

## Phase C step 2: the level-one devices

Two devices carry level one of the disk model: `/dev/bootpart` over the boot partition, and `/dev/grubcore` over the post-MBR gap where GRUB's core image lives. Every image may write both, including the everyday one, because updating a kernel or a boot loader is ordinary work.

The old order made that impossible for the case it matters most in. `/dev/bootpart` was published at the very end of a successful KRFFS mount, so a machine booting a RAM image and mounting nothing from the disk had no way to touch the boot partition that had stopped it booting, which is precisely what recovery is for. Discovery now runs before the root source is chosen: the disk is probed once, sector 0 is read and judged once, both devices are published, and only then does the machine decide where its root comes from.

Judging entry 0 needed a second question, because the old one cannot be asked without a mounted root. `mbr_bootpart_pick` compares entry 0 against the entry the root was found in; a RAM-root boot has no such entry. `mbr_bootpart_ok` compares it against the whole table instead: FAT-typed, non-empty, inside the disk, starting at or after the end of the post-MBR gap, and sharing no sector with any other non-empty entry. `mbr_bootpart_pick` is now that judgement plus its own two constraints, so the two cannot drift apart. The mount still has one fact discovery cannot know, which entry holds the root, so a disk whose FAT entry 0 turns out to be the root itself loses the device again through `devfs_bootpart_withdraw`.

The gap rule found a real problem in the project's own test fixture. `tools/mkbootfixture.py` had been placing its FAT boot partition at LBA 64, inside the 1 MiB gap that holds the boot loader. That was harmless while nothing else reached those sectors and stopped being harmless the moment `/dev/grubcore` was published over them: a boot partition overlapping the boot loader is a disk this kernel now refuses to offer one for. The fixture moved into the free space after the KRFFS partition.

`/dev/grubcore` is minor 3, mode 0600, over LBA 1 through 2047. Its range is a constant of the layout rather than anything the partition table reports, so a damaged table cannot widen it. Sector 0 is deliberately outside: it carries the partition table, and a disk writes whole sectors, so there is no way to touch the eight bytes of boot code that point at the core image without rewriting the table beside them.

Both devices now share one implementation. `raw_window_read` and `raw_window_write` took over from the bootpart-specific pair, so the second device runs the path the first one proved. The write handler gained one check it did not have: the effective uid, tested at write time rather than trusted from the open. The mode stops a non-root caller at open and nothing else, because the credential check passes uid 0 whatever the mode says, so a process could open the device while privileged, drop privilege, and keep writing through the descriptor. `block_ready` caches the probe, so the three callers that each used to reset and re-identify the controller now agree on one answer.

Proof: `make bootpart-test` grew a third boot that mounts a RAM image with the disk attached and requires both devices anyway, which is the milestone property stated as a test. It drives the gap window end to end: its exact extent, a read at the start, exactly 0 at the end, `-ENOSPC` at and across the end, `-EINVAL` for an unaligned offset and for a partial-sector count, and `-EACCES` for an unprivileged open. `mbr_hosttest` covers the new judgement, and gained a labelled macro because it had been counting failures without naming them.

Shown failing: making discovery return before it publishes anything fails the RAM-root boot; dropping the write-time uid check lets the privilege-drop write through and fails it; widening the gap window past 2047 fails its extent and end-of-window assertions. In `mbr_hosttest`, dropping the gap check and comparing entry 0 against only entry 1 each fail.

### What the review changed

Six findings, all confirmed, and the first was a way to destroy a file system.

`/dev/grubcore` was published before sector 0 had been read at all, and its only condition was that the disk held 2048 sectors. On a disk with an unreadable or unsigned table, or one whose partitions reach into the gap, the window would have handed out write access to sectors a file system owned. That is not hypothetical: this repository's own test fixture put a partition at LBA 64 until this milestone. Nothing is published now until the table has been read and its signature accepted, and `mbr_gap_clear` withholds the window when any non-empty entry reaches into LBA 1 through 2047.

Discovery and the mount each read sector 0 and parsed their own copy, so the record's claim that the table is read once was false, and two reads of one sector can disagree. Discovery keeps the parsed table and the mount uses it.

`block_ready` cached failure as well as success, which quietly disabled the rescue shell. A controller that did not answer at boot could never be asked again, and the rescue shell exists for exactly that machine. It now answers from `block0` when a disk has already been found, so a working controller is still never reset twice, and retries when none has.

The rescue shell's `ahci` command re-initialises the controller, which clears `block0` and the HBA globals. Before this step that was harmless, because no raw window existed on a boot that had not mounted a disk; now one does, and it would have been left addressing a driver with no port. The command takes all three raw windows down first, `/dev/disk0` included, and republishes afterwards on both the success and the failure path.

The last two were documentation: the devfs and block Readmes still described the old contract, and three comments had drifted from the declarations they belonged to, including a claim that the boot partition is 16 MiB.

### A scope decision worth challenging

The plan asks for MBR validation against the geometry compiled into the build: the exact type and bounds of entries 0, 1 and 2. That is not what was built. Pinning those numbers would make every image that is not the sealed disk fail the check, so `install-test`, `test-disk` and the rest would lose `/dev/bootpart` and fail. What is built instead is a judgement that holds for any disk: the gap rule, containment, and non-overlap. The review agreed the gap rule was the part that mattered for safety and raised it to a finding; it did not accept that the exact pinning can wait. Pinning the sealed layout belongs with the sealed kernel build, which step 4 introduces, and it is recorded here rather than done quietly.
