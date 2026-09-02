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

## Open items

* Phase 5: the partition nodes report their size, so that `e2fsck -p`
  does not abort on a size of zero.
