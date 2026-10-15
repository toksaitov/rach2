# `krffs/`: the KRFFS host tools and the image manifests

`krffs/` is the build machine's side of KRFFS, the project's own file system. It has the host programs that make, check, compact, edit, draw and read KRFFS images (`mkfs.krffs`, `fsck.krffs`, `defrag.krffs`, `edfs.krffs`, `fsviz.krffs`, `readfile.krffs`), the stager that writes a directory tree into an image from a manifest (`stageimage.krffs`), a password hasher (`mkhash.krffs`) and an optional FUSE driver (`krffs`); the scripts that build images and the development credentials; the 21 manifests in `manifests/`, which list what each image contains; and four scripts the images install as programs, `dmesg.script`, `man.script`, `udhcpc-default.script` and `rescue-static.script`. Every system image is staged on the host without mounting anything, so the build needs neither root nor FUSE and works the same on macOS and Linux, and `fsck.krffs`, which shares no code with the kernel, judges the images the kernel writes in the suites. The format is the kernel's: [krffs.Readme.md](../rach2/sys/fs/krffs.Readme.md).

## Use

### The programs

`make` builds the programs into `build/krffs/`, all but the FUSE driver, which `make -C krffs mount` builds:

```text
mkfs.krffs IMAGE [-f] [--offset BYTES --size BYTES]
fsck.krffs IMAGE [--strict] [--offset BYTES --size BYTES]
defrag.krffs IMAGE
edfs.krffs IMAGE POSITION [--set-magic N] [--set-type root|free|reserved]
    [--set-id N] [--set-name NAME] [--set-size N] [--set-prev-size N]
    [--set-data-size N] [--offset BYTES --size BYTES]
fsviz.krffs IMAGE [-t] [--offset BYTES --size BYTES]
readfile.krffs IMAGE GUEST_PATH OUTPUT [--offset BYTES --size BYTES]
stageimage.krffs MANIFEST IMAGE [--offset BYTES --size BYTES]
stageimage.krffs MANIFEST --tree DIRECTORY
mkhash.krffs '$6$SALT' < PASSWORD_FILE
krffs IMAGE MOUNT_POINT [FUSE_OPTIONS]
```

The first five print their usage line for `-h`.

`mkfs.krffs` writes an empty file system, a root node and one free node, over an existing regular file or a region of one, which must be at least two node headers (714 bytes). It refuses a region that already begins with the KRFFS magic number unless `-f` is given. The root gets the caller's user and group and the current time.

`fsck.krffs` walks the ring and prints a line for each node. A fault that the kernel's order of writes cannot produce is fatal and leaves the image as it was. Four faults that a stopped machine can leave are repaired, each with a message: a backward link the forward chain contradicts, a reserved node with no name, one name on two nodes of a directory, and the dirty flag. [krffs.Readme.md](../rach2/sys/fs/krffs.Readme.md) explains the two kinds and how a repeated name is resolved. `--strict` reports the repairable faults and changes nothing. The exit status is 0 for a clean or repaired image; 1 for a bad argument, an open or mapping that fails, or a root without the magic number or the root type; -10 for a chain that does not return to the root, -11 for a node whose header leaves the region, -12 for a missing magic number, -13 for an unknown type, -14 for a size, data length or name that does not fit, and -15 for repairs owed under `--strict`. A shell sees the negative values as 246 down to 241.

`defrag.krffs` runs `fsck.krffs IMAGE` first, which repairs what it can, and stops if that fails; it looks for `fsck.krffs` on `PATH`, then in the current directory. It then moves the reserved nodes, in order, toward the start of the region, so the free space ends as one node at the end.

`edfs.krffs` sets fields of the node that starts POSITION bytes into the region, to break a test image in a known way or mend one by hand. Numbers are decimal. `--set-name` stores at most 255 bytes and sets the identifier to the name's djb2 hash, in place of any `--set-id`. An option given twice is an error. When no node starts at POSITION, nothing changes and the status is still 0.

`fsviz.krffs` draws the ring in the terminal, through the termbox2 library, as a row of bars labelled with file names and drawn in proportion to the nodes' sizes. `h` and `l` or the arrow keys scroll, `+` halves the bytes per character, `_` doubles them, and `q` or Ctrl-C quits. `-t` prints one frame on standard output instead, at 512 KiB a character.

`readfile.krffs` copies the regular file at the absolute GUEST_PATH out of an image into OUTPUT. It opens the image read-only and checks every node its path walk visits against the region (`krffs_reader.c`), so a damaged image is refused with the reason instead of being read out of bounds. It refuses a directory, a FIFO, a socket, and an OUTPUT that is the image itself. It is how a file a guest wrote to its disk reaches the host.

`stageimage.krffs` writes the tree a manifest describes into an image that `mkfs.krffs` formatted, and prints the bytes used and free. It reads host paths relative to the current directory, the repository root when the image scripts run it. With `--tree DIRECTORY` it writes the tree as host files under DIRECTORY, which must not exist, and prints `node MODE UID GID PATH` for each node, the mode with its type bits; a directory no line declares is a node of 0:0 with mode 0755, whatever the builder's identity and umask. [`tools/Readme.md`](../tools/Readme.md) describes `tools/image/mkext2.sh`, which makes the ext2 images from that output.

`mkhash.krffs` reads a password from the first line of standard input, not empty and at most 255 bytes, and prints its SHA-512 crypt hash under the `$6$` setting given as its argument. It is compiled from musl's own `crypt_sha512.c`, so the hash is the one the guest's C library checks at login.

### Regions

With `--offset` and `--size`, a program works on a KRFFS that is a region of a larger file, such as a partition of a disk image; without them the region is the whole file. The values are bytes, in any C base. A nonzero offset must be a multiple of 1 MiB and needs an explicit size. The region must be at least two node headers, lie inside the file, and be at most 4095 MiB, since the kernel keeps a file system's size in 32 bits (`krffs_file_system_t` in `rach2/sys/fs/krffs.h`).

### Manifests

A manifest has one directive a line:

```text
dir           MODE GUEST_PATH
file          MODE GUEST_PATH HOST_PATH
optional-file MODE GUEST_PATH HOST_PATH
text          MODE GUEST_PATH TEXT
exec-dir      MODE GUEST_DIR HOST_DIR
tree          MODE GUEST_DIR HOST_DIR
```

Fields are separated by blanks, a line whose first word begins with `#` is a comment, blank lines are skipped, and a line is at most 2046 bytes. `MODE` is octal, at most `07777`, and may give an owner as `MODE:UID:GID`; the default owner is 0:0. A guest path is absolute and at most 255 bytes, with no empty, `.` or `..` component and no trailing slash.

* `dir` makes a directory, or gives an existing one this mode and owner, so the line takes effect wherever it stands.
* `file` copies a host file. `optional-file` skips one that does not exist, such as Doom's game data; one that exists and cannot be read is an error for both.
* `text` writes the rest of the line, after the blanks that follow the path, with `\n`, `\t` and `\\` expanded.
* `exec-dir` copies the regular files with an execute bit directly in HOST_DIR, not symbolic links and not files in its subdirectories.
* `tree` copies every regular file under HOST_DIR, makes its directories with mode 0755 and the line's owner, and leaves a directory already in the image as it is. Any other kind of file, a symbolic link among them, is an error, and so is a HOST_DIR that is a link.

`exec-dir` and `tree` leave out names that begin with a dot and stage the rest in `strcmp` order. A missing parent directory is made with mode 0755 and owner 0:0. The stager refuses a file at a path already staged, a path at or below `/dev` or `/proc`, which the kernel provides, a path below a file, and a `tree` line with a set-user-ID or set-group-ID mode. In a KRFFS image it also refuses a file larger than the room left, and a directory, declared or made as a parent, whose identifier a directory in the image already has; `bC` and `cb` in one directory are such a pair.

Every staged node, the root included, has the time one second after the epoch, and the root has the owner 0:0. With the sorted walks, one manifest over one set of host files gives one image, byte for byte. The time is not zero because pdpmake takes a file of time zero for one that does not exist, and the staged sources are built on the machine with `make`.

A manifest gives host files by their i686 paths, `build/...` and `vendor/musl/install/...`, so one manifest serves both architectures. When `RACH2_PATH_MAP` is set, it is a list of `FROM=TO` pairs separated by blanks, and the stager replaces the first FROM that begins a host path with its TO. The root Makefile sets it for x86-64, so that `build/...` is read from `build/x86_64/...` and musl's install from `build/x86_64/vendor-work/musl/install`, except the rendered manual under `build/man/` and the suites' fixtures under `build/tests/system/`. `manifest_deps.sh` and the set-id guard map paths the same way.

A manifest needs an entry of its own name, less `.manifest`, in `tools/inspect/setid.allow`, and a script's private copy of a manifest must keep the name. The set-id guard, `tools/inspect/setidcheck.c`, which `mkimage.sh` and `mkdisk.sh` run on the manifest and on the staged image, refuses a manifest without an entry, an entry without a manifest, and a set-id `exec-dir` or `dir`, so a privileged program needs a `file` line. Each program that `os-real`, `os-simple`, `rach2-metal-disk` or `rach2-recovery` stages under `/bin` or `/sbin` needs a manual page or an alias ([`man/Readme.md`](../man/Readme.md)). [`tools/Readme.md`](../tools/Readme.md) describes the guard and the manual check.

### The manifests

The images are written under `build/images/`, or `build/x86_64/images/` for x86-64. The root Makefile and `tests/system/suites.mk` set their sizes.

| Manifest | Images | Contents |
| --- | --- | --- |
| `test` | `test.krffs` (`make test-image`) | the guest probes, the musl programs under `/musl`, `ish`, and the `/etc` files the probes read |
| `os-simple` | `os-simple.krffs` (`make os-simple-image`), `os-simple.ext2` | `ish`, the in-house utilities and account programs, `init-login` |
| `os-real` | `os-real.krffs`, `os-real.ext2`, `os-real.disk` (`make os-real-image`, `os-real-ext2`, `os-real-disk`), suites' disks and volumes | BusyBox, Dropbear, TinyCC, pdpmake, musl's headers and libraries, `mke2fs`, `bootctl`, `update`, the manual, Doom, `/src` |
| `os-real-gnu` | `os-real-gnu.ext2` (`make os-real-gnu-ext2`) | os-real without Dropbear, with `init-metal`; the GNU layer is installed over it |
| `rach2-metal` | `rach2-metal.krffs` (`make rach2-metal-image`) | the metal kernel's RAM root: a shell, Dropbear, the accounts and keys |
| `rach2-metal-disk` | `rach2-metal-disk.ext2`, the sealed disk's root (`make rach2-sealed-disk`), and `installer-root.ext2` | os-real with `init-metal` and `/demo` |
| `rach2-recovery` | `rach2-recovery.krffs`, or `rach2-recovery-with-key.krffs` in a tree with a recovery key (`make rach2-recovery-image`) | `init-recovery`, the repair programs, and `recovery_authorized_keys` if the tree has one |
| `grub-test` | `grub-test.ext2`, for `grub` and `bootorder` | os-real and two marked GRUB cores; `grub` stops if the other lines differ from os-real's |
| `fs-demo` | `fs-demo/ext2.img`, `fs-demo/krffs.img` | `demo/hello.txt` |
| `disk-test` | disks of `disk`, `crash` and the other disk suites | probes the debug shell runs from a disk root |
| `persist-test` | disks of `persist`, `persist-ahci` | `persistprobe` |
| `cred-persist-test` | the disk of `cred-persist` | `creddisk` and the two files it replaces |
| `stress-test` | disks of `stress`, `stress-ahci` | `stressprobe` |
| `shutdown-test` | the disk of `shutdown` | BusyBox, a set-user-ID `su`, `init-shell`, a test account |
| `login-test-strict`, `login-test-shell` | the two RAM roots of `login` | one system, with `init-login` or `init-shell`, and no `/bin/login` |
| `krffsvol-test` | the volume of `krffsmount` and `krffscheck` | three files, one in a directory |
| `loopimg-test` | `loopdev`'s loop image, and the KRFFS partition of `metal`'s stick | two files |
| `imagerace-tree` | the second image of `imagerace` | the programs of one directory |
| `rng-proof`, `usbdisk-proof` | private images of `tests/system/rng_mutate.sh` and `usbdisk_mutate.sh` | one probe, which each script replaces with its own build in a copy of the same name |

### Building an image

`mkimage.sh MANIFEST OUTPUT [SIZE_MIB [MIN_FREE_KIB]]` builds a whole-file image, the form of a RAM root. It fills a temporary file with SIZE_MIB mebibytes of zeros, 10 by default, runs `mkfs.krffs` and `stageimage.krffs` on it, fails if fewer than MIN_FREE_KIB kibibytes are left free, runs the set-id guard, and renames the file over OUTPUT only when every step has passed. A failed build leaves the previous image. The temporary names contain the process ID, so two builds of one image at once both finish.

`mkdisk.sh MANIFEST OUTPUT [DISK_MIB [PARTITION_MIB]]` builds a disk image: an MBR (`tools/image/mkmbr.c`) with one primary partition of type `0x7f` at LBA 2048, 1 MiB in, and a KRFFS staged in that partition through the region options. The defaults are 64 and 48 MiB. It refuses a partition that does not fit, or one above 512 MiB, the cap of the kernel's page-cache window (`PC_WINDOW_CAP` in `rach2/sys/mm/pagecache.c`). It runs the set-id guard and renames only a finished image into place, but has no free-space floor and writes no dependency file.

Both run from any directory and need `mkfs.krffs` and `stageimage.krffs` in `build/krffs/`. `create_file_system.krffs.sh [SIZE_MIB]` makes an empty `build/krffs/file_system.krffs`, 10 MiB by default.

### Credentials

The images' accounts use keys and passwords made in `krffs/` and kept out of version control; [`tools/Readme.md`](../tools/Readme.md) gives the procedures.

* `gen_demo_key.sh` writes `authorized_keys` and `test_authorized_keys` from the public halves of the committed key pairs `rach2_demo_ed25519` (root) and `rach2_test_ed25519` (the test account), followed by `authorized_keys.extra` and `test_authorized_keys.extra` if present, which must contain only public keys. It makes a key pair only where one is missing, or over the committed ones under `RACH2_REPLACE_KEYS=1`.
* `gen_demo_password.sh` writes the two accounts' passwords to `rach2_demo_password` and `rach2_test_password`, readable only by their owner, and their hashes, made by `mkhash.krffs` with random salts, to `shadow`, which the manifests stage as `/etc/shadow`. The passwords are fixed development values unless `RACH2_ROOT_PASSWORD` or `RACH2_TEST_PASSWORD` gives others. While `shadow` exists it makes nothing new, unless `RACH2_REPLACE_PASSWORDS=1` is set.
* `gen_recovery_key.sh BASE OUTPUT [EXTRA]`, run by `make recovery-key`, writes the recovery system's list `recovery_authorized_keys`: BASE, then each public key in EXTRA, which `ssh-keygen` must accept. Without EXTRA it keeps an existing list.

The two key scripts replace a list only when its contents change, so a rebuild with the same keys does not restage the images.

### Mounting an image on the host

```sh
make -C krffs mount
sh krffs/create_file_system.krffs.sh
mkdir -p build/krffs/mount
build/krffs/krffs build/krffs/file_system.krffs build/krffs/mount
```

The driver checks the whole ring first, backward links included, and runs FUSE single-threaded. It finds a path as the kernel does, a component at a time by its directory and its name, so a staged image and one made through the mount read alike. Create, read, write, truncate, rename, unlink, `mkdir`, `rmdir`, `chmod`, `chown`, `utimens`, `fsync` and `statfs` work, and there is no hard link or symbolic link. As on the kernel's boot root, a directory cannot be renamed, a rename does not replace a directory, and `mkdir` refuses a name whose identifier another directory has, with `EINVAL`. A write past the end of a file fills the gap with zeros. `chown` keeps an owner or group given as -1; `utimens` takes times, `UTIME_NOW`, `UTIME_OMIT`, or none, meaning now; and the root's dirty flag is neither shown in its mode nor changed by `chmod`. Unmount with `umount` on macOS or `fusermount -u` on Linux.

### The staged scripts

* `dmesg.script` is `/bin/dmesg`, which prints the kernel's log from `/dev/bootlog` ([dmesg(1)](../man/man1/dmesg.1)).
* `man.script` is `/bin/man` ([man(1)](../man/man1/man.1)).
* `udhcpc-default.script` is `/usr/share/udhcpc/default.script`, which BusyBox's `udhcpc` runs when a lease arrives, is renewed or ends, to set the address, the route and `/etc/resolv.conf` ([busybox-networking(1)](../man/man1/busybox-networking.1)).
* `rescue-static.script` is the recovery image's `/bin/rescue-static`, which takes the sealed disk's fixed address when no lease arrives ([rescue-static(8)](../man/man8/rescue-static.8)).

`demo/hello.txt` is the file in the FAT32, ext2 and KRFFS demonstration images that the sealed disk has in `/demo`, and on its two FAT partitions; `demo/Readme.txt` is the disk's `/demo/Readme.txt`.

## Design

* `krffs_node.c`: `struct krffs_node`, the packed 357-byte node header, laid out as the kernel's `krffs_node_t`, and the ring walk: the next node is at a node's address plus its size, and the end of the region wraps to the root.
* `krffs_file_system.c`, `krffs_platform.c`: the handle, a mapped region and its size; mapping it (`mmap`, or `MapViewOfFile` on Windows); the empty layout; the region options.
* `krffs_allocator.c`: first fit with split and coalesce, as in the kernel, for the stager, `fsck.krffs` and the FUSE driver.
* `krffs_node.c`: the node header, the ring walk, and the lookup of a path by directory and name. `krffs_utilities.c`: the djb2 hash of a name and the identifier of a directory. `krffs_reader.c`: the bounds-checked path walk of `readfile.krffs`.
* `NAME.krffs.c`: each tool. `krffs.c`, `krffs_fuse_operations.c`: the FUSE driver.
* `vendor/`: termbox2 2.7.0-dev (MIT license), its Windows form, and `windows.sh`, which makes one from the other. `vs-dokany/`: a Visual Studio solution with the FUSE driver, over Dokany's FUSE library, and five of the tools.

Every tool maps the region shared and works on the nodes in place, so a store into a node is a store into the file. No tool keeps the kernel's order of writes; the image scripts build in a temporary file that only a finished, checked image replaces.

### Identifiers

Each node has a 64-bit identifier, and a reserved node has its directory's in `parent_id`, 0 for the root directory. `krffs_calculate_djb_hash` is the djb2 hash of a name: `mkfs.krffs` gives it to the root and the FUSE driver to the files it creates, as the kernel does to the regular files it creates. `krffs_mix_id` in `krffs_utilities.c` mixes the parent's identifier with that hash, as the kernel's function of the same name does; the stager gives the result to every node, and the FUSE driver, as the kernel does, to the directories it makes. `tests/host/krffs_hash_vectors.h` has the values both must give, and the kernel's test checks its own functions against the same list. The kernel finds a file by its directory and name, never by its identifier, so a file with either kind works there.

### Staging and checking

`stageimage.krffs` gives the root the owner 0:0 and the staged time, then applies the manifest a line at a time (`apply_line`). `resolve_dir` finds or makes each directory of a path through `krffs_find_child`, which matches a reserved node's `parent_id` and name, and `stage_node` allocates the node, fills its header and copies the data after it. Each image is staged into an empty file system, so a name already there is an error, not an update. The bulk directives sort a directory's names before they stage them, since `readdir` order differs between hosts.

`fsck.krffs` asks of each node what the kernel's mount check asks, and judges a node's size before it steps by it. It writes nothing until the walk has returned to the root. It then rebuilds the backward links first, since the allocator follows them when it frees a node, takes the name off the loser of each repeated name, frees the nameless reserved nodes through the allocator, and clears the dirty flag last.

### Dependencies

An image is staged again when a file it stages changes, and only then. `manifest_deps.sh MANIFEST` prints the host paths a manifest stages, mapped as the stager maps them, leaving out an absent optional file and a `build/` path not built yet. The root Makefile runs it at start on `test`, `os-simple`, `os-real`, `os-real-gnu`, `rach2-metal`, `rach2-metal-disk`, `rach2-recovery` and `grub-test`, and makes the paths prerequisites of their images. A directory is printed as the files the stager takes from it, since a directory's time does not change when a program in it is relinked, and the source trees `utils`, `rach2` and `ish` that `tree` lines stage are also the names of phony targets, which make always remakes.

`mkimage.sh` sets `STAGEIMAGE_DEPS`, and the stager writes there every host path it reads, absent optional files included, and each directory an `exec-dir` line reads, marked apart. From the list `mkimage.sh` writes `OUTPUT.d`, which the root Makefile includes for the test, os-simple and os-real images. Each path that exists is a prerequisite with an empty rule, so that deleting it restages the image rather than stopping make; an absent one is replaced by its nearest existing directory, whose time changes when the file appears. An `exec-dir` directory's time changes when anything is made in it, and `build/tests` has the work directories of suites and proofs too, so `OUTPUT.programs` lists the programs in it instead (`programs.sh snapshot`), and `OUTPUT.d` lists that file. At each start the root Makefile runs `programs.sh check`, which rewrites a record, putting its image out of date, only when the programs have changed.

## Building

`make -C krffs` builds the eight tools into `build/krffs/`, whatever `ARCH` is, with `cc` and the host flags of `mk/host.mk` (`-O2 -Wall -Wextra -Werror`), `-std=gnu99` and `-D_FILE_OFFSET_BITS=64`; [`mk/Readme.md`](../mk/Readme.md) describes the header dependencies and the configuration fingerprint. The root `make` builds them before any image and after `check-libc`, because `mkhash.krffs` compiles `src/crypt/crypt_sha512.c` from the extracted musl 1.2.5 tree, `vendor/musl/musl-1.2.5/`, or its copy under `build/x86_64/vendor-work/musl/` for x86-64, without the warning flags. Where musl is not extracted, `make -C krffs` stops and says to run the root `make`. `fsviz.krffs` has `-Wno-error=unused-result`, since termbox2 ignores the results of a `read` and a `write` that the host's headers mark.

`make -C krffs mount` builds `build/krffs/krffs` against FUSE's 2.6 interface with the flags `pkg-config` gives for `fuse`, or else `fuse-t`; without either it stops and names the packages, which [`tools/Readme.md`](../tools/Readme.md) gives for each host. No build or suite needs it. `make -C krffs clean` removes `build/krffs/`.

## Limits

* The FUSE driver takes a whole-file image, and renames no directory.
* `defrag.krffs` does not pass the region options to `fsck.krffs`, so it works only on a whole-file image.
* `fsviz.krffs` and `edfs.krffs` follow the ring by each node's size without `fsck.krffs`'s checks, so an image `fsck.krffs` refuses can make them loop or read past the mapping.
* `edfs.krffs --set-name` on a directory replaces its identifier, which cuts it off from its children.
* Every tool that opens an image, except `readfile.krffs`, opens it for writing, even `fsviz.krffs` and `fsck.krffs --strict`.
* The stager refuses two directories with one identifier, not two files, which the kernel allows.
* Nothing builds or tests `vs-dokany/`. It has no project for `stageimage.krffs`, `readfile.krffs` or `mkhash.krffs`.

## Tests

* `tests/run host` runs `krffs_fuse_hosttest.c` (the FUSE driver over real images, with a stand-in `tests/host/fuse/fuse.h`), `krffs_hash_hosttest.c` (the stager's identifiers against the kernel's vectors), `readfile_hosttest.c` (the reader over regions damaged by hand) and `mkhash_hosttest.c` (musl's SHA-512 crypt against published vectors) under AddressSanitizer and UndefinedBehaviorSanitizer. It also runs `sh tests/host/krffs_tools_mutate.sh --check`: the stager's refusals, a failed `mkimage.sh` keeping the previous image, `fsviz.krffs -t`, `defrag.krffs` closing a hole `edfs.krffs` made, and `edfs.krffs --set-magic`.
* `tests/run style` builds the tools, runs the set-id guard over every manifest, and runs `tools/verify/check_repro.sh`, which stages one manifest twice and requires identical images with the bulk directives' nodes in name order.
* `tests/run krffscheck` (`tests/system/krffscheck.sh`, needs `python3`) damages images and reads `fsck.krffs`'s verdicts on each fault, then stops a model of the kernel's order of writes after each store and requires every result to be accepted or repaired.
* `tests/run imagerace` builds one image twice at once, and checks that a changed program restages its image while an unchanged one, or a file made beside it, does not.
* `crash`, `shutdown`, `krffsmount` and `loopdev` run `fsck.krffs` on volumes the kernel wrote, and `selfbuild` takes the kernel the guest built off its disk with `readfile.krffs`.
* `sh tests/host/krffs_tools_mutate.sh` puts thirteen faults into private copies of the stager, `fsviz.krffs`, `defrag.krffs`, `edfs.krffs` and `mkimage.sh`; `sh tests/host/krffs_fuse_mutate.sh` puts five faults into the driver and the library, among them the ring check taken out and a lookup that ignores the directory; and `sh tests/system/host_tools_mutate.sh` puts faults into the driver, the host allocator's truncate and the host hashes, among others. Each fault must fail its test.

[`tests/host/Readme.md`](../tests/host/Readme.md) and [`tests/system/Readme.md`](../tests/system/Readme.md) describe these. Run a suite with `tests/run NAME`, and a mutation script with `sh` from the repository root.
