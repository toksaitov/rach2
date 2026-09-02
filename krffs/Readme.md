# krffs/: host file-system tools

This directory contains the host implementation of KRFFS and the tools that create, inspect, and stage the file-system images used by Rach2. The kernel implementation is documented in `rach2/sys/fs/Readme.md`. GRUB disk construction belongs to `tools/image/mkdisk_grub.sh` and is documented in `tools/Readme.md`.

## Build

The component uses the host C compiler and builds eight binaries under `build/krffs/`:

```sh
make -C krffs
```

The default build needs neither FUSE nor `pkg-config`. The FUSE mount is a separate optional target, `make -C krffs mount`, whose two objects use the flags reported by `pkg-config fuse` or `pkg-config fuse-t`; without an installed FUSE it fails with an installation hint. The other tools do not include FUSE headers or link with FUSE. Header dependencies are written beside the objects under `build/krffs/`.

`mkhash.krffs` compiles the SHA-512 crypt implementation from the extracted musl 1.2.5 source. The root build orders KRFFS after `check-libc`. In a fresh tree, run the root `make` before a standalone `make -C krffs`.

## Tools

* `krffs` (optional, `make -C krffs mount`) mounts a whole-file image through the FUSE 2.6 interface. It forces single-threaded FUSE operation.
* `mkfs.krffs` initializes an existing regular file or region. `-f` permits replacement of an existing KRFFS signature.
* `fsck.krffs` validates the root and the complete node ring. It clears the dirty flag after validating the root. After a successful structural walk, it frees reserved nodes whose names are empty.
* `defrag.krffs` runs `fsck.krffs`, then moves allocated nodes toward the start and coalesces free space. It currently works only on a whole-file image; its pre-check does not forward region arguments.
* `fsviz.krffs` draws the node layout with termbox2. `-t` selects text output.
* `edfs.krffs` edits selected node fields for diagnosis and repair.
* `stageimage.krffs` reads a manifest and writes a hierarchical image without mounting it.
* `readfile.krffs` extracts one absolute guest path into a host file through a bounds-checked directory walk.
* `mkhash.krffs '$6$salt'` reads a password from standard input and writes a musl-compatible SHA-512 crypt hash.

The image tools other than the FUSE mount accept a backing-file region where applicable:

```text
--offset BYTES --size BYTES
```

`mkfs.krffs`, `fsck.krffs`, `fsviz.krffs`, `edfs.krffs`, `stageimage.krffs`, and `readfile.krffs` support regions. A nonzero offset must be a multiple of 1 MiB and requires an explicit size. The region must fit in the backing file and may be at most 4095 MiB. `defrag.krffs` has the limitation noted above. The FUSE mount accepts a whole-file image, not a region.

## Format

KRFFS v2 is a packed sequence of variable-size nodes. The last node wraps to the root, and each node stores its own size and the previous node's size. The three node types are root, free, and reserved. Reserved nodes represent files and directories.

Each node stores:

* the `KR` magic and node type;
* a 64-bit hash identifier and the containing directory's identifier;
* a 256-byte name buffer, for at most 255 name bytes plus the terminator;
* mode, uid, gid, and access, modification, and attribute-change times;
* total node size, previous node size, and data size.

The allocator uses first fit, splits a free node when the remainder can hold a node header, and coalesces adjacent free nodes. Identifiers are hashes; the host and kernel implementations do not detect collisions.

`stageimage.krffs` writes parent-aware identifiers and directory modes. The FUSE mount remains a flat root directory: it has no `mkdir`, `rmdir`, hard-link, or symbolic-link operation. It supports lookup, listing, create, read, write, truncate, unlink, rename, timestamps, mode, ownership, sync, and file-system statistics for root-level files.

## Manifests and images

`stageimage.krffs` accepts these line-oriented manifest directives:

```text
dir           MODE GUEST_PATH
file          MODE GUEST_PATH HOST_PATH
optional-file MODE GUEST_PATH HOST_PATH
text          MODE GUEST_PATH TEXT
exec-dir      MODE GUEST_DIR HOST_DIR
tree          MODE GUEST_DIR HOST_DIR
```

`MODE` is octal and may include an owner as `MODE:UID:GID`; omitted ownership is `0:0`. Guest paths are absolute. `exec-dir` stages the regular executable files from one directory. `tree` recursively stages regular files and creates directories. Both walks are sorted. `stageimage.krffs` rejects a set-id `tree`. The image scripts' set-id checker also rejects a set-id `exec-dir`; a privileged file must have its own `file` directive. The stager rejects paths below the synthetic `/dev` and `/proc` trees.

Staged nodes use the fixed time one second after the Unix epoch, and the root is forced to uid 0 and gid 0. These rules and sorted bulk walks make repeated staging reproducible. The image scripts run the set-id guard (`tools/inspect/setidcheck.c`) on both the manifest and the completed image before publishing it.

`stageimage.krffs MANIFEST --tree DIR` materialises the manifest into a host directory instead of an image: one host file or directory per node, every node at the fixed staged instant, and one line `node MODE UID GID PATH` on standard output for each, with the POSIX type bits in the mode. The directory must not exist, so that nothing a previous run left can end up in an image. `tools/image/mkext2.sh` hands the directory to `mke2fs` and the lines to `debugfs`, which sets the owners and the set-id bits a build that is not root cannot put on a host file.

`mkimage.sh MANIFEST OUTPUT [SIZE_MB]` creates a whole-file RAM image. The default size is 10 MiB. It builds a temporary file, runs `mkfs.krffs` and `stageimage.krffs`, checks it, and renames it over the output only after every step succeeds.

`mkdisk.sh MANIFEST OUTPUT [DISK_MB] [PARTITION_MB]` creates a disk file with an MBR and one KRFFS partition of type `0x7f` at LBA 2048. The defaults are a 64 MiB disk and a 48 MiB partition. The partition may not exceed the kernel's 512 MiB mount window. This script also builds through a temporary file and publishes only a checked image.

The root Makefile owns the named image profiles and their dependencies. The root `make` builds `test.krffs`, `os-simple.krffs`, and `os-real.krffs`. The metal and recovery RAM images have separate targets, named in the root Makefile.

`gen_demo_key.sh` provisions separate Ed25519 keypairs for root and the test account. Optional `authorized_keys.extra` and `test_authorized_keys.extra` files add local public keys; the script rejects private-key material and malformed lines. `RACH2_REPLACE_KEYS=1` replaces existing generated pairs.

`gen_demo_password.sh` provisions the two development passwords and a matching `/etc/shadow`. It uses `mkhash.krffs`, writes credentials with a restrictive umask, and publishes the two-line shadow file atomically. Existing credentials remain unchanged unless `RACH2_REPLACE_PASSWORDS=1` is set. `RACH2_ROOT_PASSWORD` and `RACH2_TEST_PASSWORD` supply replacement values.

## Mounting a scratch image

Build the optional mount tool with `make -C krffs mount`, then create the image and mount point under the ignored build directory:

```sh
mkdir -p build/krffs/mount
truncate -s 10M build/krffs/example.krffs
build/krffs/mkfs.krffs build/krffs/example.krffs
build/krffs/krffs build/krffs/example.krffs build/krffs/mount
```

The mounted directory supports ordinary root-level file operations. Use the host's FUSE unmount command when finished: `umount build/krffs/mount` on macOS, or `fusermount -u build/krffs/mount` on Linux with FUSE 2.

`create_file_system.krffs.sh [SIZE_MB]` is a convenience wrapper that recreates `build/krffs/file_system.krffs`; its default size is 10 MiB.

## Verification

`tests/run host` covers the bounded reader and the musl-compatible password hash. `tests/run style` builds the component as part of the in-house tree and checks dependency, text, and source conventions. Image builds exercise staging and both set-id checks. The disk, persistence, credential, crash, shutdown, and stress targets exercise images made by `mkdisk.sh` through the kernel implementation.
