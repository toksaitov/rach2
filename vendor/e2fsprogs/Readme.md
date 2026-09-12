# e2fsprogs 1.47.4 integration

Rach2 stages two programs of e2fsprogs, `e2fsck` and `resize2fs`, linked against the ported [musl](../musl/Readme.md) and placed in the recovery image at `/sbin/e2fsck` and `/sbin/resize2fs`. Recovery's `init` runs `e2fsck` on each of the four MBR partition nodes that has the ext2 magic it finds before it starts anything else, and both can be run by hand over SSH. The installer's grow runs `resize2fs` on the root it has just written, which is why the resizer is staged. `mke2fs` and `debugfs` are not built for the machine: nothing on it formats or edits a volume, and a formatter with no proof would be staged for no use. The host's own e2fsprogs builds and checks the ext2 root image; `tools/Readme.md` says where it is looked for.

The integration includes `.gitignore`, this Readme, `build_e2fsprogs.sh`, `e2fsprogs-1.47.4.tar.gz`, a `muslgcc` wrapper, and the project patches named by `patches/series`. The compiler behind `muslgcc` is `i686-elf-gcc` and defines no operating-system macro, so the wrapper defines `__rach2__`, and `raw-device-io.patch` takes it in the two places the library decides how to reach a device: `getsize.c` defines the `BLKGETSIZE` and `BLKGETSIZE64` request numbers, which it otherwise does only under `__linux__`, and `unix_io.c` aligns every read and write to 512 bytes through its bounce buffer, as it does for Cygwin. The kernel's partition nodes are raw windows that take whole sectors at sector-aligned offsets and answer the two requests; without the patch `e2fsck` sizes a device by reading single bytes and writes the superblock's changed bytes alone, and both are refused. The second patch, `buffer-alignment.patch`, lets a sector-aligned offset and length use the whole caller buffer regardless of its address. Devfs stages user bytes before issuing disk I/O, so the user address needs no DMA alignment. A partial-sector update still uses the bounce buffer and preserves the surrounding bytes. The extracted source, the staged binaries and the stamp are ignored and reproducible.

## Build

From the repository root:

```
make e2fsprogs
```

The root `make` builds it through the same stamp on the way to the recovery image, and `make check-e2fsck` requires the staged binary to be a static executable this kernel loads. The stamp depends on the build script, the archive, the wrapper, the patch manifest and patches, and the musl stamp.

The build script:

1. Requires `e2fsprogs-1.47.4.tar.gz` to have SHA-256 `da274408bebbfd13a5a2fc3cfc66e3ffff17c48534673aa67f88d49b99123b96`, the sum published beside the tarball on kernel.org.
2. Replaces the extracted tree and applies the patches `patches/series` names, in order.
3. Configures for `i686-linux-musl` through `muslgcc`, with `i686-elf-ar` and `i686-elf-ranlib`, the private uuid and blkid libraries and the resizer on, and off: message catalogs, thread locals, the uuid daemon, fuse2fs, the initrd helper, e2image, e4defrag, debugfs, multi-mount protection, backtraces, tdb and rpath.
4. Builds the libraries, then `e2fsck` and `resize2fs`, each with `RDYNAMIC` empty, because that flag exports every symbol for the backtrace that is switched off and the freestanding linker does not take it.
5. Strips both and stages `build/vendor/e2fsprogs/e2fsck` and `build/vendor/e2fsprogs/resize2fs`.

Each staged binary is a stripped static ELF32 i386 `ET_EXEC`: `e2fsck` is 673800 bytes and `resize2fs` 390816. The root `Makefile` refuses a build whose `e2fsck` is over 737280 bytes or whose `resize2fs` is over 430080, so a configure that quietly turned a library or a feature back on is caught here. The build takes fifteen seconds on the ten-core machine after musl is built.

## What it is checked by

`make check-e2fsck` runs `elfcheck` over both binaries and checks their sizes against the limits above, as the other ports' checks do. `tests/run ext2` boots the recovery image against a dirty ext2 partition and requires `e2fsck` to repair it and the next ordinary boot to mount it clean; `tests/run selfbuild` on the ext2 root reads a volume the checker has passed.
