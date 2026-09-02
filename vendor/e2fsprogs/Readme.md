# e2fsprogs 1.47.4 integration

Rach2 stages one program of e2fsprogs, `e2fsck`, linked against the ported [musl](../musl/Readme.md) and placed in the recovery image at `/sbin/e2fsck`. Recovery's `init` runs it on every ext2 partition it finds before it starts anything else, and it can be run by hand over SSH. `mke2fs` and `debugfs` are not built for the machine: nothing on it formats or edits a volume, and a formatter with no proof would be staged for no use. The host's own e2fsprogs builds and checks the ext2 root image; `tools/Readme.md` says where it is looked for.

The tracked integration has six files: `.gitignore`, this Readme, `build_e2fsprogs.sh`, `e2fsprogs-1.47.4.tar.gz`, a `muslgcc` wrapper, and the patch manifest `patches/series`, which is empty because there are no project source patches. The extracted source, the staged binary and the stamp are ignored and reproducible.

## Build

From the repository root:

```
make e2fsprogs
```

The root `make` builds it through the same stamp on the way to the recovery image, and `make check-e2fsck` requires the staged binary to be a static executable this kernel loads. The stamp depends on the build script, the archive, the wrapper, the patch manifest and patches, and the musl stamp.

The build script:

1. Requires `e2fsprogs-1.47.4.tar.gz` to have SHA-256 `da274408bebbfd13a5a2fc3cfc66e3ffff17c48534673aa67f88d49b99123b96`, the sum published beside the tarball on kernel.org.
2. Replaces the extracted tree and applies the patches `patches/series` names, in order.
3. Configures for `i686-linux-musl` through `muslgcc`, with `i686-elf-ar` and `i686-elf-ranlib`, the private uuid and blkid libraries on, and off: message catalogs, thread locals, the uuid daemon, fuse2fs, the initrd helper, e2image, resize2fs, e4defrag, debugfs, multi-mount protection, backtraces, tdb and rpath.
4. Builds the libraries, then `e2fsck` alone with `RDYNAMIC` empty, because that flag exports every symbol for the backtrace that is switched off and the freestanding linker does not take it.
5. Strips it and stages `build/vendor/e2fsprogs/e2fsck`.

The staged binary is a stripped static ELF32 i386 `ET_EXEC` of 673672 bytes. The build takes fifteen seconds on the ten-core machine after musl is built.

## What it is checked by

`make check-e2fsck` runs `elfcheck` over the binary, as the other ports' checks do. `tests/run ext2` boots the recovery image against a dirty ext2 partition and requires `e2fsck` to repair it and the next ordinary boot to mount it clean; `tests/run selfbuild` on the ext2 root reads a volume the checker has passed.
