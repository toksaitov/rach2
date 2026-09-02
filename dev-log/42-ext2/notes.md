# Milestone 42 notes

## Phase 1 record

Arrival on 2026-09-02 at `cbf7d80 Close milestone 41's record` with a
clean tree. The twelve commits before it:

```
cbf7d80 Close milestone 41's record
2c71a28 Keep the default boot's transcript
b2336d9 Read a failed kernel self-test summary at its level and score the group in one function
e855085 Take the clock check's walk up to three times against a late tick
42777c2 Record milestone 41's review round
58192f8 Correct the hub claims the closing review found in the Readmes
9aed48c Fix what the closing review found in the hub tree and its tests
3a88dc4 Keep a transfer event on the ring when the completion queue is full
896a944 Draft milestone 41's record and index entry
16b57ac Record phase 5 of the hub milestone
369d82f Build /proc/usb's path where the records' path is built
547dfd8 Add the hub checks to the acceptance script and rehearse them behind a hub
```

### The review units

Three rounds: after phase 3, after phase 5, and before the sweep, on
Codex through its rescue agent, units of at most six files and ten
minutes, at most two units in a round running at once and none starting
another:

* unit A, the driver: `ext2.c`, `ext2.h`, `ext2.Readme.md`, and after
  phase 4 the writer's half of `ext2.c` read on its own;
* unit B, the walker and the syscalls: `vfs.c`, `vfs.h`, `vfs.Readme.md`,
  the syscall table and `vfsprobe.c`;
* unit C, the tools: `mkext2.sh`, `stageimage.krffs.c`, `setidcheck.c`,
  `mkdisk_grub.sh`, `preflight.sh`, `doctor.sh`;
* unit D, the tests: `ext2_hosttest.c`, `ext2.sh`, the matrix script,
  `metal_accept.sh`;
* unit E, the port and recovery: `build_e2fsprogs.sh`, the recovery
  manifest and its `init`, `vendor/e2fsprogs/Readme.md`;
* unit F, the reader-facing documents: `rach2/Readme.md`, `Readme.md`,
  `tools/Readme.md`, `krffs/Readme.md`, `utils/Readme.md`,
  `tests/system/Readme.md`.

The round after phase 3 runs A and B; after phase 5, A, C and E; before
the sweep, D and F, with A once more over the whole driver.

### The host tool

e2fsprogs 1.47.4 was installed from Homebrew, which keeps it keg-only under
`/opt/homebrew/opt/e2fsprogs/sbin`. `tools/setup/e2fsprogs_path.sh` adds
that directory, its Intel twin, `/sbin` and `/usr/sbin` to the path, and
`preflight.sh`, `doctor.sh` and `mkext2.sh` source it. `preflight.sh`
requires `mke2fs`, `debugfs` and `e2fsck` and probes `mke2fs` for the
`-d` option; `doctor.sh` probes the three with `-V`, because `debugfs`
takes `--version` as no option and opens its prompt on standard input,
which hung the first run of the report.

### The image by hand

The os-real manifest was materialised into a directory by a throwaway
script, 637 nodes, and `mke2fs -t ext2 -b 4096 -I 128 -O
sparse_super,large_file,filetype,^dir_index,^resize_inode,^ext_attr,
^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,
^extra_isize -d DIR -L rach2` wrote a 64 MiB volume from it. A
`debugfs -w -f` script of 1911 `set_inode_field` commands set each node's
mode, uid and gid; the first form wrote the permission bits alone into
`mode` and cleared the type bits, after which every path below `/bin`
answered `not a directory`, so the script writes the type with the bits,
`0100755` for a file and `040755` for a directory, which is what
`mkext2.sh` must do. `e2fsck -fn` on the result: clean, 644 of 16384 inodes,
3248 of 16384 blocks. `debugfs ls -l /bin` lists `su` and `login` as
`104755` owned by 0:0, and `stat /bin/su` shows inode 24, mode 04755, size
88556, 184 sectors in `i_blocks`, twelve direct blocks and an indirect
block. The superblock at byte 1024: magic `0xEF53` at offset 56, state 1
at 58, revision 1, first inode 11, inode size 128, compat 0, incompat 2
(`filetype`), read-only-compatible 3 (`sparse_super`, `large_file`), 16384
blocks of 4096 in one group, 819 reserved blocks for uid 0. `mke2fs -d`
takes the source tree's own times, so `mkext2.sh` sets a fixed time
through `E2FSPROGS_FAKE_TIME` for a reproducible image.

### The numbers, sourced

Every number in the plan's ext2 bullet, against `lib/ext2fs/ext2_fs.h` of
the pinned tarball, by line:

| Number | Value | Source |
|--------|-------|--------|
| Magic | 0xEF53 | `EXT2_SUPER_MAGIC`, line 83 |
| Superblock offset | 1024 | `SUPERBLOCK_OFFSET` in `ext2fs.h`; the first 1024 bytes are the boot area |
| Revisions | 0 and 1 | `EXT2_GOOD_OLD_REV`, `EXT2_DYNAMIC_REV`, lines 796 and 797 |
| Block sizes | 1024 << `s_log_block_size`, 1024 to 65536 | lines 102 to 105 and 113; this driver reads to 4096 |
| Inode size | 128 at revision 0, `s_inode_size` at 1 | lines 802 and 115 |
| First inode | 11 at revision 0, `s_first_ino` at 1 | lines 78 and 117 |
| Bad blocks inode, root inode | 1, 2 | lines 66 and 67 |
| Direct blocks, pointer levels | 12, then indirect, double, triple | lines 312 to 316 |
| Fast symbolic link | size below 60, the sixty bytes of `i_block` | `ext2fs_is_fast_symlink`, `symlink.c` line 208 |
| Directory entry padding | 4 | `EXT2_DIR_PAD`, line 1110 |
| Name length | 255 | `EXT2_NAME_LEN`, line 1019 |
| File types in entries | 0 to 7, `EXT2_FT_MAX` 8 | lines 1086 to 1093 |
| Link limit | 65000 | `EXT2_LINK_MAX`, line 97 |
| Valid bit, error bit | 1, 2 | `EXT2_VALID_FS`, `EXT2_ERROR_FS`, lines 555 and 556 |
| `filetype` | incompatible 0x2 | line 862 |
| `sparse_super`, `large_file` | read-only-compatible 0x1, 0x2 | lines 838 and 839 |
| `dir_index` | compatible 0x20 | line 829 |
| Refused incompatible: journal device, extents, 64-bit, flexible groups | 0x8, 0x40, 0x80, 0x200 | lines 864 to 869 |
| Read-only unknown: huge file, dir_nlink, extra isize, metadata checksum | 0x8, 0x20, 0x40, 0x400 | lines 841 to 853 |
| Ignored compatible: journal, extended attributes, resize inode | 0x4, 0x8, 0x10 | lines 826 to 828 |
| Size high word | `i_size_high` at offset 108, with `large_file` | line 427; `EXT2_I_SIZE` in `ext2fs.h` line 586 |
| Deletion time | `i_dtime` | line 411 |
| Device numbers | `i_block[0]`, else `i_block[1]` | `ext2fs_inode_data_blocks2` and the kernel's `old_decode_dev`; the plan's rule |
| Timestamps | 32-bit seconds | the 128-byte inode's four `__u32` times, lines 405 to 411 |

The offset of the superblock is the first 1024 bytes reserved for a boot
sector in every block size, which the hand image shows: the magic at byte
1080, the feature words at 1116, 1120 and 1124.

### The port

`vendor/e2fsprogs/build_e2fsprogs.sh` configures for `i686-linux-musl`
through the wrapper with every program but `e2fsck` off, builds the
libraries and `e2fsck` with `RDYNAMIC` empty, and stages a stripped static
`e2fsck` of 673672 bytes; the build is fifteen seconds through the stamp.
Two things the trial found: `make -C lib/ext2fs` alone fails for want of
the generated `ext2_err.h`, so the script builds the `libs` target; and
the link adds `-rdynamic`, which the freestanding linker does not take.
`make check-e2fsck` runs `elfcheck` and requires the binary under
737280 bytes, a tenth over the first build; a limit of 600000 fails the
check with the size named.

The recovery image stages it at `/sbin/e2fsck`; the image is a fixed
4 MiB and has room. Booted in QEMU with 64 MiB and a disk carrying the
hand image as partition 1, the recovery system answers SSH in two
seconds, `/proc/meminfo` says 16412 kB free of 32768 at rest, and
`e2fsck -fn /dev/disk0p1` checks the volume clean, taking 800 kB at its
peak on this 64 MiB volume, sampled every 20 ms. The RAM limit is set at
8 MiB at the peak on the 1 GiB image, measured in phase 6.

`e2fsck` first said `The physical size of the device is 0 blocks` and
asked `Abort?`, which `-n` answered no: the partition node gives no size,
neither through `BLKGETSIZE` nor `lseek` to the end. Under `-p` that
question aborts the run. Phase 5, where the recovery build's partition
nodes become writable, gives them a size as well, and the row that runs
`e2fsck -p` from recovery's `init` is what requires it.

## Phase 2 record

`stageimage.krffs MANIFEST --tree DIR` is the tree mode: the four stage
functions branch at their top into a host file or directory under `DIR`,
every node is printed as `node MODE UID GID PATH` with the POSIX type
bits, the directory must not exist beforehand, and every node gets the
fixed staged instant, directories last. On the os-real manifest it
materialises 633 nodes, four fewer than the throwaway script of phase 1,
which did not skip dotfiles as `tree` does; the difference is
`src/rach2/.gdbinit`, which the image should not carry.

`tools/image/mkext2.sh` is the image build: the tree, `mke2fs` on the
profile with `E2FSPROGS_FAKE_TIME=1` and a fixed identifier, the
superblock read back at bytes 1080, 1100, 1048, 1112, 1116, 1120 and 1124
and refused outside the profile, the `debugfs` script from the node lines
with the type bits kept, `e2fsck -fn`, the manifest sweep, and the guard's
ext2 reader; the rename comes last. `make os-real-ext2` builds
`build/images/os-real.ext2`, 1 GiB, 19 MiB on disk, in under a second
after the tree. The 128-byte inode deprecation line `mke2fs` prints is
filtered, since the profile is deliberate.

`setidcheck` gained the reader: `ext2_open` refuses anything but revision
1, 4096-byte blocks, 128-byte inodes, first inode 11, first data block 0
and the three feature words 0, 0x2 and 0x3 exactly; it marks the
superblock and descriptor copies in every `sparse_super` group and each
group's two bitmaps and inode table as structural, and a data pointer
into any of them, or past the volume, is a refusal. `ext2_reach` walks
from inode 2 with a visited set for directories, checks `.` and `..`,
each entry's `rec_len`, name and type against the inode's mode, and lists
every path as a node of the same table `check_image` judges, so the policy
half is one function for both formats; a regular file's bytes are copied
through the three pointer levels for the trusted-text and sysroot checks,
which read them through `node_bytes` now. A second name for one inode is
a second node, which is how a hard link to a set-id file is an undeclared
set-id path. `--ext2 FILE --manifest PATH` and `--nodes --ext2 FILE` are
the entry points.

The self-test builds its ext2 fixtures with the host's `mke2fs` and
`debugfs`, the inventory fixture's four nodes on the profile, and has
thirteen ext2 rows: the reader lists the fixture as `debugfs` wrote it,
mode, owner, bytes and `lost+found`; the clean fixture passes; a set-id
file at the wrong owner, at the wrong group, with a bit added, with its
bit removed, an inode the manifest does not name, and a hard link to a
set-id file each fail with the policy's own message; and a feature outside
the profile, a block outside the volume, a block inside the inode table,
a malformed entry with a `rec_len` of 6, and an entry naming inode
268435455 each refuse the image by name. The last row first poked the
root's first entry, which is `.`, and was refused as a wrong `.` instead;
it pokes the third entry. `check-setid: selftest: 122 cases PASS`.

The two script mutations: `mkext2.sh` without its `debugfs` step fails
the guard with every node at the build's own uid, and with `dir_index`
on it stops at the superblock with `compatible features 32, not 0`.

The first reader crashed the self-test: the KRFFS walk's node was never
zeroed, so the `data` pointer the ext2 nodes own was garbage there and
`image_free` freed it. `unpack_node` zeroes its node now.

## Phase 3 record

`sys/fs/ext2.c` is the reader: revision 0 and 1, block sizes 1024 to 4096,
inode sizes 128 and 256, the three feature words against the sets, the
descriptors read once into a table of 128 groups, a node pool of 64, and
every pointer checked by `data_block_ok` against the volume and the
structural blocks, every inode number against the count, every entry
against its block. `block_at` walks the three pointer levels with a hole
as zero; `read_data` copies through the cache; `ext2_path` rebuilds a
directory's path through `..`. The kernel has no libgcc, so the byte
offset is split by a shift and a mask and not by a division: the first
link of the kernel failed on `__udivdi3`, which the host test, built with
a host compiler, had not shown. `klog` has no `%u`; the records use `%d`.

The walker follows links: after a lookup on a type with `readlink`, one
`getattr` says whether the node is a link, and if more path follows, or
the caller did not pass `VFS_WALK_NOFOLLOW`, the target replaces the
component in a buffer of the walker's own, an absolute target restarts at
the root, the rest continues from the directory the link was found in,
and the forty-first link is `ELOOP`. The first form copied the rest after
the target into the same buffer the rest already lived in; a chain of two
links then read `hello.txt/o`, which `ext2probe` reported as `ENOTDIR` on
`c39`, and the rest moves before the target is written. `symlink` is a
new operation at the end of `vfs_ops`, so every positional table gained a
slot. The syscalls: `readlink` through the type, `lstat` and `fstatat64`
with `AT_SYMLINK_NOFOLLOW` and `utimensat` with it leaving the link,
`open` leaving it under `O_NOFOLLOW`, which then answers `ELOOP`, and
under `O_CREAT|O_EXCL`, which answers `EEXIST`; `link` and `linkat`
through `do_link`, the source as itself unless `AT_SYMLINK_FOLLOW`, a
directory `EPERM`, another mount `EXDEV`, a read-only mount `EROFS`;
`symlink` and `symlinkat`, numbers 83 and 304, through `do_symlink`, with
`EROFS` before `EPERM` so that a read-only ext2 mount answers as Linux
does.

`ext2_hosttest` builds its volumes with the host's `mke2fs` from a tree
it stages, at 4 KiB blocks with 128- and 256-byte inodes, at 2 KiB, and at
1 KiB for a 70 MB sparse file with a byte behind each pointer level; the
image is a file the block device reads through `fseek`, so a sparse
fixture costs nothing. `debugfs` adds device nodes in the old encoding;
its `mknod` refuses a major above 255, so the new encoding is written by
field, `block[1]` as 0x11112c70 for 300 and 70000. A size past the
kernel's offset is `set_inode_field size 5000000000`, the 64-bit field.
The guard's reader judges the profile volume only, since it refuses the
others by design. `tests/run ext2` is new, with `mkext2fixture.sh`
writing a disk whose first partition is the fixture, `ext2probe links`
mounting it read-only and running the walker's rows, and `tests/run`
sourcing `e2fsprogs_path.sh` so that the registry's `mke2fs` is found
where the build finds it. The probe first booted as the stale copy in the
test image, because `run-ext2` did not name `test-image`; it does. A
freestanding probe enters at `_start` and reads its arguments off the
stack, as the others do.

The ten mutations the plan names, each caught by its row: an unknown
incompatible feature ignored; `dir_index` mounted writable; a slow link
read from the inode, which in its first form read the pointer words past
their end and crashed the sanitised test instead of failing the row, so
the mutation clamps the link to fifty-nine bytes of the inode and the row
fails on its bytes; a device number from the first pointer alone; a
`rec_len` trusted past its block, whose first form removed the term and
did not build for an unused variable, so it loosens the bound instead; a
block inside the inode table, which first escaped because the row poked
block 1, the descriptors, guarded by another check, so the test gained a
row that reads the table's first block from `dumpe2fs` and pokes that,
beside the descriptor row; a file past the offset opened; a forty-first
link followed; the last component followed under `O_NOFOLLOW`; and a
dangling link followed under `O_CREAT|O_EXCL`. The last three fail
`ext2probe`'s rows through `tests/run ext2`, the rest `ext2_hosttest`'s.

### The review round after phase 3

Three Codex units through the rescue agent, each moved to a background
job by the agent's ten-minute limit and read from the job's output: unit
A over the driver, its header, its Readme and its host test; the walker
and the syscalls first as one unit, which returned nothing in ten
minutes, then as two, the walker's link arm and the link syscalls with
the probe. Every finding was confirmed and fixed, and each fix that a row
can see has one.

Unit A, the driver, eight findings. The constructor read the descriptor
table without proving it lay inside the volume and accepted a table at
block zero, so a one-block volume could mount with a root inode read from
the boot block; and `data_block_ok` consulted the descriptor of the
candidate block's own group, which a descriptor placing its bitmap in
another group defeated. The constructor now requires the table inside the
volume and each group's two bitmaps and inode table inside that group,
past its superblock copy and descriptors, apart from each other and past
block zero, which is the placement the per-group lookup assumes. A
directory entry was checked against its block and not the directory's
size, and `readdir` returned an inode number past the count, or, without
`filetype`, took a failed inode load as a file; both are malformed
entries now, and a failed load is the error. A slow link was refused past
1020 bytes, which the format allows to a block less one, and a slow link
without a block read sixty zeros. Group free counts were summed
unbounded; each is now at most its group's size or the mount is refused
by name. The mount reported itself writable with no writer; every mount
is read-only until phase 4. The cache split used a 64-bit division by a
constant, which the compiler lowers but the rule forbids; it is a shift
and a mask. The host test gained a row for each, with the read-only
reasons, the refusals and the failures checked against the warning
record, which a `last_warn` copy in the test's `klog` keeps, since the
mount's own record overwrote the last line; and a revision 0 volume, a
directory whose size ends inside its first entry, an entry naming an
inode past the count, a 1021-byte link, a blockless slow link, a free
count past the group, tables where a group's cannot be, sixty-two held
nodes with the next refused, and a size of 4 GiB less one.

Unit B1, the walker, six findings. The link arm called `getattr` and
`put` without checking that the type has them, and a `getattr` error was
read as "not a link"; the arm runs only on a type with both, puts through
a helper that tolerates a missing `put`, and fails the walk on the error.
The splice added a slash after the target with nothing to follow it, so a
511-byte target answered `ENAMETOOLONG` one byte early; the slash joins
only a non-empty rest. The header promised a common-layer `EPERM` for a
type without `symlink` and no wrapper existed; `vfs_symlink` is the
wrapper, beside `vfs_link`, and `do_symlink` calls it. The ordering of
the link check before the covering-mount check is safe because a mount
covers a directory, which the comment now says. The prose it quoted is
recast.

Unit B2, the syscalls, six findings. `open` with `O_CREAT` through a
dangling link resolved the link's own parent, created the link's name,
answered `EEXIST` and went round again forever; it now resolves the name
without following, and when that is a link it reads the target, puts the
link's directory in front of a relative one, and starts over on that
path within the resolver's bound of forty. The `/proc` links reached
`synth_open` before the `ELOOP` and `EEXIST` checks; they take the same
two answers through `synth_is_link`. `readlink`'s kernel buffer and the
`symlink` wrappers' target buffers were 256 bytes, so a 300-byte target
failed both; they are the resolver's 512. `do_link` answered `EPERM` for
a KRFFS source before resolving the destination, so a cross-mount link
never reached `EXDEV`; the order is Linux's, another mount, then a
directory or a type without links, then a read-only mount. `fstatat64`
and `utimensat` accepted any flag; each answers `EINVAL` outside its
set. Five stale comments went with them, one claiming `link` always
answers `EPERM`. The probe gained the rows the unit named: `O_CREAT`
through a dangling link on the read-only volume answering `EROFS` and not
looping, `O_CREAT|O_EXCL` on a link with a target, a one-byte and an
empty `readlink` buffer with a sentinel past the byte, a 300-byte target,
`fstatat64` with and without `AT_SYMLINK_NOFOLLOW` and with an invalid
flag, `utimensat` with an invalid flag, the old `stat` and `lstat`, a
hard link across mounts, and `/proc/self` and `/proc/self/fd/1` under the
two open flags. What the unit noted and this milestone leaves: an
ordinary `open` of `/proc/self/fd/N` answers `ENOENT` rather than
reopening the descriptor, which is procfs's contract from milestone 31
and not the walker's; it is written in `synthfs.Readme.md` as it is.

## Phase 4 record

The writer is the second half of `ext2.c`: `write_bytes` through or
back, `inode_store` rewriting the fields the driver keeps into the 128
bytes read back and zeroing `i_dtime`, `inode_store_freed`, the bitmaps
through `bit_get` and `bit_put`, the descriptors through `gd_store` and
the superblock's counts and state through `sb_store`; `block_alloc`
first-fit from the block after the last given out, wrapping once, with
the reserve refused below root, and `inode_alloc` from the parent's group
on; `map_alloc` through the three pointer levels with a pointer block
zeroed and written before its pointer, and `punch` from the end with each
pointer cleared before its block is freed; the directory writes `dir_add`,
`dir_remove_at`, `dir_set_dotdot`, `dir_is_empty` and `is_below`; and
the operations, `setattr`, `write`, `truncate`, `fsync`, `create`,
`unlink`, `mkdir`, `rmdir`, `link`, `symlink`, `rename`, `sync` and
`prepare_unmount`, with `put` freeing an unlinked inode at its last hold.
The mount clears the valid bit before it is published.

What the rows found while the writer was written, each fixed with the
row that showed it: the entry split wrote the shortened old header before
the new entry, and `e2fsck -n` refuses to judge a directory with bytes no
entry covers, so the new entry goes into the slack first; a partial write
into a block just given out left the rest of the block as the junk it
held, seen only once the matrix's free blocks were filled with junk, so
a fresh block is zeroed before the bytes land; a truncation left the tail
of the last block kept as it was, so a later growth read old bytes, and
the tail is zeroed; the byte-writing macros evaluated their value twice,
so a count read from the bytes being written lost its high byte at every
wrap of the low one, which showed as a group with no free blocks beside
a superblock with 256, and each macro takes its value once; a reused
inode kept the deletion time of the one freed there, which `e2fsck`
reports as in use with dtime set, and a live inode is stored with none;
and `mkdir` wrote the directory's inode before its block, so a stop
between left a zero-length directory, and the inode follows the block.
The test's lock stubs record the holder, since the writer asserts the
lock as the FAT driver does, and its entry buffer had room for 255 name
bytes and none for their padding.

The matrix walks 439 stops of twelve sequences, the many-files sequence
left out for its thousands of writes, on a volume whose free blocks hold
14 MB of random bytes freed by `debugfs`, built once and copied for each
stop, and judges each three ways. The host suite ends a test at sixty
seconds; the first form of the matrix took 127 s, formatting the junk
volume at every stop, and 63 s with the copy, so the sequences write
less, 100000 bytes for the extension and 40000 for the truncation, which
keeps every pointer level and every kind of stop, and the test takes
24 s. The writer's first link into the kernel failed on `__udivdi3`
again, seven divisions of a 64-bit offset or size by the block size;
each is a shift or a mask now.

The crash state was first taken by syncing the cache into the image
after the stop, which put the unsynced data on the disk and let the
mutation that writes the size before the sync escape; the stop now drops
the cache as `bcache_device_gone` drops a gone device's buffers, and the
true crash state then showed one more fault: a truncation's zeroed tail
was written back, so a growth over it published bytes the disk still
held, and a growth syncs the device before it writes the size. The
kinds of finding `e2fsck -fn` reported over every stop of the finished
driver were collected and are the allowed list: free counts, bitmap
differences, `i_size` and `i_blocks` behind their pointers, a reference
count, a deleted inode without a deletion time, an unattached inode or
directory, and `..` or an entry naming the wrong directory during a
directory's move; nothing else appeared, and nothing else is allowed. A
renamed or linked file must be reachable by a name at every stop past the
operation's start, which each sequence marks by the write count at which
it calls the operation, and must keep a name after the repair; a repair
whose base file was changed underneath fails the comparison.

The plan's eight mutations: an inode named before it is written, which
judge two catches at the stop where the name points at an inode never
stored (`Entry ... has deleted/unused inode`, not an allowed kind); the
size before the data sync, which the junk oracle catches once the crash
state is a true one; a held inode freed at unlink, which fails the open
unlinked file's row and `e2fsck`; the old entry removed before the new
one in a rename, which fails the row requiring a name at every stop past
the operation; the superblock counts written on every allocation, which
the counts row catches; and the valid bit never cleared, which the mount
row catches. Two are recorded as not separable by any judge, as the hub
milestone recorded one: an inode allocated before it is zeroed at free,
because every allocation here writes the inode whole before it is named,
so a stale pointer cannot survive into a live inode and the two orders
leave the same states; and the size grown before the pointers, because a
size over a block the pointers do not yet name reads as a hole, which is
zeros, and the oracle allows zeros for bytes not yet synced. The order in
the code is the plan's in both cases.

## Open items

* Phase 5: the partition nodes report their size, so that `e2fsck -p`
  does not abort on a size of zero.
