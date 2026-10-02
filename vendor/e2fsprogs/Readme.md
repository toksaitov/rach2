# e2fsprogs 1.47.4 integration

Rach2 stages three programs of e2fsprogs, linked against the ported [musl](../musl/Readme.md). The recovery image has `/sbin/e2fsck`, `/sbin/resize2fs` and `/sbin/mke2fs`; os-real and the GNU system have `/sbin/mke2fs`. Recovery's `init` runs `e2fsck` on each of the four MBR partition nodes that has the ext2 magic it finds before it starts anything else, and each program can be run by hand over SSH. The installer's grow runs `resize2fs` on the root it has just written, which is why the resizer is staged. `mke2fs` makes a volume: in a file on the ordinary kernel, which writes no disk node, for a loop device to mount, and on a partition under the recovery kernel. `debugfs` is not built for the machine. The host's own e2fsprogs builds and checks the ext2 root image; `tools/Readme.md` says where it is looked for.

`mke2fs` takes its defaults from `mke2fs.conf` beside the build script, which the build copies to `misc/mke2fs.conf.custom.in`, the file upstream's make compiles into the program in place of its own. It is upstream's file with the features and block sizes the kernel's ext2 driver mounts writable: `dir_index` and `ext_attr` are off, and the `small` and `floppy` types keep 4 KiB blocks where upstream gives them 1 KiB. No image stages `/etc/mke2fs.conf`, which the program would read in place of its defaults.

The integration includes `.gitignore`, this Readme, `build_e2fsprogs.sh`, `e2fsprogs-1.47.4.tar.gz`, `mke2fs.conf`, a `muslgcc` wrapper, and the project patches named by `patches/series`. The compiler behind `muslgcc` is `i686-elf-gcc` and defines no operating-system macro, so the wrapper defines `__rach2__`, and `raw-device-io.patch` takes it in the two places the library decides how to reach a device: `getsize.c` defines the `BLKGETSIZE` and `BLKGETSIZE64` request numbers, which it otherwise does only under `__linux__`, and `unix_io.c` takes the alignment path it uses for Cygwin, which sends a request whose offset and length are sector-aligned straight to the device and bounces the rest through a sector buffer that preserves the bytes around a partial sector. The kernel's partition nodes read any byte range but take a write only at a sector-aligned offset and length; without the patch `e2fsck` sizes a device by reading single bytes and writes the superblock's changed bytes alone, and the write is refused. The second patch, `buffer-alignment.patch`, lets a sector-aligned offset and length use the whole caller buffer regardless of its address, which is the only one of the three alignment tests it drops. Devfs stages user bytes before issuing disk I/O, so the user address needs no DMA alignment. A partial-sector update still uses the bounce buffer and preserves the surrounding bytes. The third, `proc-mounts.patch`, has `ismounted.c` read `/proc/mounts` under `__rach2__` as under `__linux__`; otherwise the programs read only `/etc/mtab`, which only the GNU system has, as a link, and each run elsewhere warned that it was missing and could not see a mounted volume. The extracted source, the staged binaries and the stamp are ignored and reproducible.

## Build

From the repository root:

```
make e2fsprogs
```

The root `make` builds it through the same stamp on the way to the images, and `make check-e2fsck` requires each staged binary to be a static executable this kernel loads. The stamp depends on the build script, the archive, `mke2fs.conf`, the wrapper, the patch manifest and patches, and the musl stamp.

The build script:

1. Requires `e2fsprogs-1.47.4.tar.gz` to have SHA-256 `da274408bebbfd13a5a2fc3cfc66e3ffff17c48534673aa67f88d49b99123b96`, the sum published beside the tarball on kernel.org.
2. Replaces the extracted tree, applies the patches `patches/series` names, in order, and copies `mke2fs.conf` in as `misc/mke2fs.conf.custom.in`.
3. Configures for `i686-linux-musl` through `muslgcc`, with `i686-elf-ar` and `i686-elf-ranlib`, the private uuid and blkid libraries and the resizer on, and off: message catalogs, thread locals, the uuid daemon, fuse2fs, the initrd helper, e2image, e4defrag, debugfs, multi-mount protection, backtraces, tdb and rpath.
4. Builds the libraries, then `e2fsck`, `resize2fs` and `mke2fs`, each with `RDYNAMIC` empty, because that flag exports every symbol for the backtrace that is switched off and the freestanding linker does not take it.
5. Strips the three and stages them under `build/vendor/e2fsprogs/`.

Each staged binary is a stripped static ELF32 i386 `ET_EXEC`: `e2fsck` is 673800 bytes, `resize2fs` 390816 and `mke2fs` 501608. The root `Makefile` refuses a build whose `e2fsck` is over 737280 bytes, whose `resize2fs` is over 430080 or whose `mke2fs` is over 552960, so a configure that quietly turned a library or a feature back on is caught here. The build takes sixteen seconds on the ten-core machine after musl is built.

## What it is checked by

`make check-e2fsck` runs `elfcheck` over the three binaries and checks their sizes against the limits above, as the other ports' checks do. `tests/run ext2` boots the recovery image against a dirty ext2 partition and requires `e2fsck` to repair it and the next ordinary boot to mount it clean. In the same suite `mke2fs` makes a volume in a file on the ordinary kernel, which a loop device mounts writable, and one on a partition under recovery, which the host's `e2fsck` passes and whose features and block size the host's `dumpe2fs` reads; `tests/run selfbuild` on the ext2 root reads a volume the checker has passed.
