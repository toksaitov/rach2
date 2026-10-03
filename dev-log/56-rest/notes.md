# Milestone 56: notes

## Where the work stands

On 2026-10-03 the owner asked for every item `dev-log/55-open/notes.md` left open, and the three faults of the console Chawan showed that morning, to be fixed while the owner slept, the framebuffer's terminal browsers apart. The plan is [`plan.md`](plan.md). Every item is done, the Lenovo runs the final build, and the record is [`Readme.md`](Readme.md).

## The batches

The first batch was the console's answers to queries, its UTF-8 and Chawan's terminal entry, made after milestone 55's close (`308d9571`, `a2dc2d69`). Its checks, s53 and s54 at the testing host, passed style, host, console and gnu, and the fault that drops the answer was caught by the console suite with the restored kernel equal to the first. Phases 1 to 4 were then written as one batch, since its first check run (s55) stopped on a style finding while phase 2 was being written; the checks of the whole came after.

## What was built

* The console's font: GNU Unifont 18.0.01's glyphs for the Cyrillic letters of Russian, Kyrgyz, Kazakh, Ukrainian and Belarusian, loaded into the VGA adapter at boot in code page 866's places, with the Kyrgyz and Kazakh letters in those of the box glyphs that join a single line to a double one, and checked by reading them back. `tools/font/mkfont.sh` writes `rach2/sys/display/vga_font.c` from the verified hex font.
* Alt on the console keyboard sends ESC before the key's character, and the console has `LANG=C.UTF-8`.
* `uname -s` is `Linux`, the release `6.18.0-rach2` and the version `#1 Rach2 1.0.0`, the owner's decision; the GNU root's site file for `configure` is gone, since `config.guess` now names the build itself.
* A pseudo-terminal is `root:tty` 0620, and util-linux's `write` and `wall` are set-group-ID `tty`.
* `ping` prints the reply's `ttl=`, which the kernel's call now returns beside the length.
* The mqueue file system at `/dev/mqueue`, and the type for `mount(2)`.
* Every boot publishes its disks' whole-disk nodes, mode 0400, whose writes the everyday kernel refuses.
* A page dated `$Mdocdate$` alone is refused by `render_man.sh`, and tmux's gets the date its file has in the release.
* tmpfs: `size=` is enforced, half the machine's memory by default, and taken again at a remount, which now gets mount(2)'s data; a file reaches 16 TiB less a page through a two-level page table.
* `fallocate` on tmpfs, with hole punching, and on ext2, which fills the range's holes with zeroed blocks and writes a block the file has only past its size.
* ext2's inode flags: `FS_IOC_SETFLAGS` stores them and the VFS enforces immutable and append-only; `i_flags` and `i_generation` are now written back to the inode, which they were not.
* ext2 writes back as Linux's ext2 does on a mount without `sync` or `dirsync`; such a mount keeps the ordered write-through and its promise.
* A file page cache in free frames for the pages of ext2 files that private mappings read.
* A bind of a directory of KRFFS.
* The ports m4, Autoconf, Automake, Bison, flex, jq, screen, sudo, CMake, and BIND's `dig` and `host` with libuv, Userspace RCU and libcap.

## Decisions taken while building

* ext2's write-back: on a default mount an operation's changes stay in the buffer cache until a sync, as on Linux, measured on the Lenovo at 1.1 ms for each empty file made before the change. A mount with `sync` or `dirsync` keeps the ordered write-through the interruption matrix judges, which already ran with `dirsync`. The cost is the promise on a default mount: a stop before a sync leaves what `e2fsck` repairs, where the order left only lost blocks, orphans and counts.
* The file page cache keeps copies, not the frames a mapping maps, so nothing a process stores reaches the cache, and it is limited to ext2, whose inode numbers and generations name one file; ext2 now gives each new inode a generation counted from the mount's second, so a reused number is not taken for the file it named before.
* Linux's ext2 answers `EOPNOTSUPP` to `fallocate`; this one fills holes instead, since the owner asked for the call, and still refuses a punched hole.

## The review

Codex was not available until 2026-10-04, by the owner's quota, so phases 1 to 4 were reviewed by hand before the commits were made, and these were found and fixed:

* `ext2_fallocate` wrote zeros over the bytes a file had when the range crossed its size inside a block it already had; it now writes such a block only past the size, and the gnu suite's fallocate row checks the bytes kept.
* The file page cache could keep a copy of a page read while a write changed it, with the size and the modification time the write left. The VFS now drops a file's pages after the change, each drop counts as a change, and a page read while the count moved is not kept.
* A punched hole walked every page of its range, which for a file sparse to 16 TiB is 2^32 steps; it now passes over a leaf the table does not have and stops at the table's end.
* A remount did not take `size=`, so `/dev/shm`, now held to half the memory, could not be given more. mount(2) passes its data to a remount, and tmpfs takes a new size the files fit in.
* Text: `rename_guarded` had been put between `vfs_rename`'s comment and its body, and ext2's Readme still said the flags were acted on by nothing.

## The checks

* s61 built the ports and was stopped once they were done, since the tree had changed under it; its fault script, which a stop of its shell did not reach, ran on and was stopped with it.
* s62 found two faults before any suite ran. The GNU root did not build: CMake installs 25 help pages whose names have spaces, `Unix Makefiles.rst` among them, and `mkext2.sh` refused any name with a space, since debugfs splits its words there; it now gives each name an overlay adds to debugfs in double quotes and refuses only a quote, a backslash or a tab. And two probe faults no longer named their text, `shmem-page-past-1gib-backed` after the two-level table and `memfd-root-has-a-path` after the bind of KRFFS.
* mapmusl's row that a page 2^31 pages into an empty file in `/dev/shm` is never backed held only because files stopped at 1 GiB. A shared mapping of a tmpfs file now has only the pages the file's size reaches, as on Linux, where the touch is `SIGBUS` and here `SIGSEGV`, and the fault `shmem-page-past-end-backed` proves the row.
* s63, the first full set, passed 81 suites and failed 10, and the Lenovo, updated to its build, showed the same faults:
  * sudo's install made `/run/sudo`, so `install_root.sh`'s link `/run` became `/run/run`, and D-Bus's system bus, which binds `/run/dbus/system_bus_socket`, did not start, nor did the browser's row pass. sudo's recipe now leaves `/run` out, and `install_root.sh` refuses a root where a port made it.
  * `losetup -a` listed nothing: util-linux reads a Linux release of 2.6.37 or later as a sysfs with `/sys/block/loopN/loop/backing_file`, which the release `1.0.0` had kept it from asking. sysfs now has the directory for a bound loop device.
  * screen exited at once, since the pseudo-terminal's master refused `TIOCPKT`; the master now has packet mode, and `TCFLSH`.
  * automake's scripts named the build host's autoconf by its path in the work directory; its recipe now puts that autoconf first in the path, so configure writes the bare name.
  * The large suite wrote at 0.9 MB/s and passed its idle limit: under write-back each dirty block of a large file went to the disk alone when it was evicted, where the drain's sync had written runs. ext2 now writes a device's dirty buffers back in runs once they are a quarter of the cache's limit (`em_balance`), as Linux makes a writer write back.
  * installer's row 11 stopped the machine with its changes in the cache; it now syncs first, as a shutdown would.
  * procprobe and tmpfsmusl counted the permanent mounts without `/dev/mqueue`; two proof fixtures that compile parts of `vfs.c` lacked the cache's stubs; the disk node row expected `EPERM` where an open for writing over the mounted root is `EBUSY` (disk(7)); the dig row's responder answered two queries and dig retried; the chattr row ran `su -s /bin/sh` where the other rows run the account's shell; and style found two comments over 80 columns, a variable clang could not see set, and two functions over their size, `sys_mount` and tmpfsmusl's `placed`.
* s64 passed 86 suites and failed 5, and its build on the Lenovo showed one fault more:
  * dig and host printed their answers and then aborted at exit, and host's answer, still buffered, was lost: BIND gives OpenSSL its own allocator, and OpenSSL 4's cleanup from its library's destructor allocated from the pool BIND had freed. BIND is built from a copy of its tree without that call.
  * The ext2 suite's row 3 found the root with lost blocks and orphan inodes: the probe rows before it end in a kill with their changes in the cache. They sync before their last marker now.
  * The installed metal rows expected no whole-disk node on an ordinary boot; they now require the MBR's hash read through it.
  * tmpfsmusl's KRFFS bind row passed with the bind's repointing taken out, since descriptors are repointed apart from it; it now grows the bound directory until KRFFS moves it and lists it through the bind.
  * Creating a file on the Lenovo's root took 1.7 ms of processor time, against 0.28 ms on a new volume: ext2 read the parent group's inode bitmap a bit at a time, and the GNU root fills its first groups. The allocators read 64 bytes at a time and pass over full bytes.
  * Style found two functions over their size and two files clang-format would wrap otherwise, and a host test lacked the cache's new stub.
* s65 passed 89 suites and failed 2: a comment the rebase had joined into one line of 92 columns, and the installed metal row with a read-only root, whose MBR read made its temp file in `/tmp`; it is made in `/dev/shm` now. Its build on the Lenovo passed every item tried there, dig and host among them, and 1000 creates took 411 ms where the old build took 1144 ms. But the browser's second start still read its program from the disk, 1850 commands each time: exec reads a program's segments through the file system, which the cache, serving private mappings, never saw. Exec's read now takes whole pages from the cache and keeps them there; on the Lenovo, with a kernel of that source built on the Mac, the second start took 3.3 s against 5.0 s and, with its profile kept, one disk command, which the gnu suite's browser row now requires under 200.
* s66 passed 87 suites; style found `kbound` one branch over its size and the record's directory without its index entry, and tmpfsmusl's new check failed on the real kernel at `fchmod`, `EROFS`: a file's mode, owner and times change through the KRFFS boot root's own paths, and a bind of it has no `setattr`. The check keeps its `fstat`, which reads the description's own node through the bind, the reference only the bind's repointing moves, and the limit is recorded in mount(2) and the VFS's Readme. Its build went onto the Lenovo, root and kernel, and was committed there: every item passed, and the browser's second start took 3.8 s and one disk command.
* s67, on the final tree, passed all 91 suites. Its probe fault that took the bind's own repointing out was again not caught: on the KRFFS root, `fstat` of a description opened through a bind reads the descriptor's node, which the root's repointing moves, and the bind's repointing is reached only through a bind of a mounted KRFFS volume, whose descriptions keep offsets; no suite binds one, so the entry was removed.

## The machine

The Lenovo was updated five times from the recovery system, with the builds of s63, s64, s65, s66 and s67, each root written by `rach2-install --update` from the stick and each new kernel through `bootctl`, and a kernel of s66's source built on the Mac was tried once through `bootctl` to measure the browser's second start. After each update the items were tried as a user would: `uname`, the group `tty` and the modes of `write`, `wall` and sudo, D-Bus's socket, `losetup -a`, a screen session, `/dev/mqueue`, a remount of `/dev/shm` with `size=`, `fallocate`, `chattr +i`, `ping`, dig and host against the real resolver, the ports' versions, 1000 creates, the console's locale and font, tmux's page, and the browser started twice. The last build, s67's, passed all of them and was committed.

The disks were written as the owner's standing authorization allows: the stick's exFAT volume with each build's root image, kernel and installer; the internal disk's root partition by each `rach2-install --update`, which archived the configuration on the recovery partition each time; and its kernel slots by `bootctl`. Each write followed the check of the disk's identity.

