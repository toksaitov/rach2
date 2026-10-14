# e2fsprogs

e2fsprogs is the ext2 tool set. Rach2 builds five of its programs statically against the ported [musl](../musl/Readme.md): `e2fsck`, `resize2fs`, `mke2fs`, `tune2fs` and `dumpe2fs`. BusyBox has no ext2 checker or resizer. The recovery system's init runs `e2fsck -p` on each MBR partition with the ext2 magic when the boot line has `rach2.fsck`, `growroot` runs `resize2fs` after it enlarges a root partition, and `mke2fs` makes an ext2 volume in a file for a loop device to mount. The [manifests](../../krffs/manifests/) say which image stages which program, and [e2fsck(8)](../../man/man8/e2fsck.8), [resize2fs(8)](../../man/man8/resize2fs.8), [mke2fs(8)](../../man/man8/mke2fs.8), [tune2fs(8)](../../man/man8/tune2fs.8) and [dumpe2fs(8)](../../man/man8/dumpe2fs.8) are their pages. `debugfs` is not built for the machine; the host's own e2fsprogs builds and checks the ext2 root images, as [tools/Readme.md](../../tools/Readme.md) says.

## Sources and license

The source is the release tarball from kernel.org (`pub/linux/kernel/people/tytso/e2fsprogs/`) in this directory. [`build_e2fsprogs.sh`](build_e2fsprogs.sh) has its version in `VERSION` and its SHA-256, from the `sha256sums` file published beside the tarball, in `TARBALL_SHA256`, and refuses a tarball whose checksum differs; the root `Makefile` gives the same file as `E2FSPROGS_TARBALL`. The programs are under version 2 of the GNU General Public License; `lib/ext2fs` and `lib/e2p` are under the GNU Library General Public License version 2, `lib/uuid` under a BSD-style license, and `lib/et` and `lib/ss` under an MIT-style license, as the tarball's `NOTICE` says.

## Local changes

### Patches

The build applies the patches [`patches/series`](patches/series) lists, in its order. `i686-elf-gcc` and `x86_64-elf-gcc` define no operating-system macro, so the build script sets `MUSLGCC_DEFINES=-D__rach2__`, which the `muslgcc` wrapper adds to each compile and final link of this port and of no other, and the patches test `__rach2__`.

* `raw-device-io.patch` makes `lib/ext2fs/getsize.c` define `BLKGETSIZE` and `BLKGETSIZE64` under `__rach2__` as under `__linux__`, and makes `lib/ext2fs/unix_io.c` take its Cygwin path under `__rach2__`. That path sends a request whose offset and length are sector-aligned to the device and passes any other through a sector buffer that keeps the bytes around the partial sector. A partition node takes a write only at a sector-aligned offset and length ([disk(4)](../../man/man4/disk.4)); without the patch `e2fsck` sizes a device by reading single bytes and writes the changed bytes of a superblock alone, and the kernel refuses the write.
* `buffer-alignment.patch` drops, under `__rach2__`, the test that the caller's buffer address is aligned, so an aligned offset and length use the caller's buffer directly. The kernel copies user bytes into its own buffer before disk I/O, so the user address needs no alignment for DMA. A partial-sector write still goes through the sector buffer.
* `proc-mounts.patch` makes `lib/ext2fs/ismounted.c` read `/proc/mounts` under `__rach2__` as under `__linux__`. Without it the programs read only `/etc/mtab`, which only the GNU system has (a link to `/proc/self/mounts`), and elsewhere they cannot see that a volume is mounted.

### mke2fs defaults

[`mke2fs.conf`](mke2fs.conf) is copied into the tree as `misc/mke2fs.conf.custom.in`, which upstream's make compiles into `mke2fs` as its defaults in place of its own. It is upstream's file with the features and block sizes the kernel's ext2 driver mounts writable: `dir_index` and `ext_attr` are not in `base_features`, and the `small` and `floppy` types keep the 4096-byte block where upstream gives them 1024 bytes. No image stages `/etc/mke2fs.conf`, which `mke2fs` would read in place of these defaults.

### Configuration

The configure run uses the private uuid and blkid libraries and the resizer, and turns off message catalogs, thread-local storage, `uuidd`, `fuse2fs`, the initrd helper, `e2image`, `e4defrag`, `debugfs`, multiple-mount protection, backtraces, tdb and rpath. Each program is linked with `RDYNAMIC` empty: that flag exports every symbol for the backtrace, which is off, and the freestanding linker does not take it.

## Building

`make e2fsprogs` builds for the architecture `ARCH` selects, i686 by default; `make e2fsprogs ARCH=x86_64` builds for x86-64. The root `make` builds it too, through the same stamp. The script takes the architecture from `RACH2_ARCH` through `mk/arch.sh`, which gives the cross prefix, `i686-elf-` or `x86_64-elf-`, the configure host `i686-linux-musl` or `x86_64-linux-musl`, and the directories. An i686 build extracts into this directory and stages under `build/vendor/e2fsprogs/`; an x86-64 build extracts under `build/x86_64/vendor-work/e2fsprogs/` and stages under `build/x86_64/vendor/e2fsprogs/`. Each has its own stamp.

`build_e2fsprogs.sh`:

1. Checks the tarball's SHA-256, deletes the extracted tree and extracts it again.
2. Applies the patches `patches/series` lists. It fails on a named patch that is absent and on a `patches/*.patch` the series does not list.
3. Copies in `mke2fs.conf`.
4. Configures with `CC` set to the `muslgcc` wrapper and the cross `ar` and `ranlib`, with the options above. The configure output goes to `configure.log` in the extracted tree, and each later step to its own `build-*.log` there; a failed step prints the last 20 lines of its log.
5. Builds the libraries, then the five programs, with `-j` set from `JOBS`, 4 by default.
6. Strips the five and stages them.

Each staged program is a stripped static `ET_EXEC`, ELF32 i386 or ELF64 x86-64. The stamp depends on the script, the tarball, `mke2fs.conf`, `muslgcc`, `mk/arch.sh`, `patches/series` and every patch, and the musl stamp. `make check-e2fsck` requires each program to be nonempty, runs `elfcheck` on it for the architecture, and refuses one larger than its limit in the root `Makefile` (`E2FSCK_LIMIT`, `RESIZE2FS_LIMIT`, `MKE2FS_LIMIT`, `TUNE2FS_LIMIT` and `DUMPE2FS_LIMIT`, one set for both architectures), so that a configure that turns a library or a feature back on fails here and not in an image.

## Updating

1. Put the new release tarball in this directory and delete the old one.
2. Change `VERSION` and `TARBALL_SHA256` in `build_e2fsprogs.sh`, and `E2FSPROGS_TARBALL` in the root `Makefile`.
3. Rebase each patch and delete one upstream has made unnecessary.
4. Compare upstream's `misc/mke2fs.conf.in` with `mke2fs.conf`, and the configure options with the new `configure --help`.
5. Rebuild; if a program passes its size limit, find the cause before raising the limit.
6. Run the checks under Tests.

## Tests

`make check-e2fsck` runs with every image build that stages the programs. The suites, described in [suites.md](../../tests/system/suites.md):

* `tests/run ext2` boots the recovery system against a damaged ext2 partition and requires `e2fsck` to repair it and the next ordinary boot to mount it clean. It also has `mke2fs` make a volume in a file, which a loop device mounts writable, the host's `e2fsck` passes and the host's `dumpe2fs` reads for its features, block size and label.
* `tests/run grow` grows an ext2 root with `growroot` and `resize2fs` on the recovery system.
* `tests/run gnu`, on the GNU system, has `mke2fs` make a volume in a file, `tune2fs` label it, `resize2fs` grow it and `dumpe2fs` read it, and `fsck` check it.
