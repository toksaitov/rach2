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

## Phase C step 2: in progress, uncommitted

This section describes work in the tree that is NOT committed. It compiles and the host tests pass, but the kernel is not wired up yet, so nothing here is proved.

Done so far:

* `mbr_bootpart_ok` in `rach2/sys/block/mbr.c`, the boot-partition judgement with no mounted root to compare against. A RAM-root boot never mounts a KRFFS partition and still has a boot partition to manage, so entry 0 is judged against the whole table: FAT-typed, non-empty, inside the disk, starting at or after the end of the post-MBR gap, and sharing no sector with any other non-empty entry. `mbr_bootpart_pick` is now that judgement plus its KRFFS-entry constraints, which makes it stricter than before: it refuses a boot partition starting inside the gap.
* Host cases for all of it in `tests/host/mbr_hosttest.c`, with a labelled `CHECKN` macro added because that file counted failures without naming them. Two mutations were shown to fail it: dropping the gap check, and comparing entry 0 against only entry 1.
* `/dev/grubcore` in `rach2/sys/fs/devfs.c`, minor 3, mode 0600, over LBA 1 through 2047. `bootpart`'s read and write became `raw_window_read` and `raw_window_write` over a shared `struct raw_window`, so both devices run the same proven path. The write handler now checks the effective uid at write time rather than trusting the open, because the mode stops a non-root caller and nothing else: a process can open while privileged, drop privilege, and keep the descriptor.

Not done:

* `rach2/kernel.c` still publishes `/dev/bootpart` only at the end of a successful disk mount, and never publishes `/dev/grubcore` at all. The boot order still has to be refactored so `block_probe` runs once, sector 0 is read and judged once, both level-one devices are published, and only then the root source is chosen. Until that lands, `devfs_grubcore_publish` has no caller.
* No proof has been run for any of it: no QEMU boot showing both devices present on a RAM-root boot, and none of the bounded-range refusals.

Steps 3 and 4 of phase C, and phases D through I, are untouched.
