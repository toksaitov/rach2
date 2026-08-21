# KRFFS File System

![KRFFS Visualization Utility's Output](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/fsviz.png)

A minimal educational FUSE file system in C. The project stores files inside a regular file using a flat structure inspired by the K&R first-fit memory allocator from "The C Programming Language." Files are memory-mapped via `mmap`/`MapViewOfFile`, allowing direct read/write access through virtual memory. The codebase uses a platform abstraction layer to support Unix, Linux, macOS, and Windows, making it suitable for porting to custom OS environments.

The code in the `krffs` directory HERE generates the file-system images Rach2 boots, both the RAM-disk modules and the partitioned disk images mounted from a real ATA disk, on host environments running the latest macOS, Ubuntu, or Windows.

## Features

* Hierarchical v2 directories (parent-id nodes) the kernel and the `stageimage` builder read and write; the interactive FUSE mount itself stays flat (one root)
* K&R first-fit allocator: block splitting, coalescing adjacent free blocks
* Memory-mapped I/O: changes written directly to backing file
* Packed metadata nodes with magic signatures for consistency checks
* File operations: create, read, write, truncate, rename, delete
* Permissions: chmod, chown, timestamps (atime, mtime, ctime)

## Utilities

* `mkfs.krffs`: create new file system
* `fsck.krffs`: check file system consistency (links, signatures, node types), and repair two things it finds. It writes to the image: it clears the dirty-mount flag a crash left set, and it frees every orphan, a reserved node whose name was cleared when the kernel took it out of the namespace while a descriptor still held it, which a shutdown that closed no descriptors can strand with nothing left to free it. The two repairs are gated differently, and the difference matters to anyone reading a failed run. The orphan free happens only after the structural walk has validated the chain end to end, so a corrupt image keeps every node it had. The dirty-mount flag is cleared as soon as the root node checks out, before the walk begins, and the mapping is shared and writable, so that clear reaches the image even when the walk then reports the file system corrupt. The kernel reaps orphans at mount as well, so running this is not required to recover the space, only to recover it offline
* `defrag.krffs`: defragment by moving free blocks to end
* `fsviz.krffs`: interactive terminal visualizer (termbox2)
* `edfs.krffs`: low-level node field editor for debugging/repair
* `readfile.krffs`: read one file out of an image by absolute path and write its bytes to a host file. This is the extraction half of the self-build proof: the guest builds a kernel on a disk copy, and this tool pulls those exact bytes out for the host to hash, check and boot. The path walk (`krffs_reader.c`, host-tested in `tests/host/readfile_hosttest.c`) validates every node it visits against the region, so a truncated or corrupted image is refused with the reason named, not read out of bounds
* `mkhash.krffs`: mint a SHA-512 crypt (`$6$`) password hash for `/etc/shadow`; the password comes on stdin, the `$6$salt` setting as the argument, and the crypt is the vendored musl implementation, so the host mints exactly what the guest's libc verifies

None of these reads a node's times, and that is a decision, not an omission. `fsck.krffs` checks invariants, and a time is not one: the format states no ordering between the three, so there is nothing for a check to find and nothing a repair could restore without inventing it. `fsviz.krffs` draws the layout, which is where the nodes are and how big they are. `edfs.krffs` edits the structural fields (magic, type, id, name, and the three sizes) and not the metadata a running kernel maintains, which is why it cannot set a mode or an owner either; a node whose times are wrong is repaired by the kernel that next writes it, or by staging the image again.

Every tool that opens a backing file (`mkfs.krffs`, `fsck.krffs`, `defrag.krffs`, `fsviz.krffs`, `edfs.krffs`, `stageimage.krffs`, `readfile.krffs`) accepts `--offset N --size N` to operate on a region inside a larger file, not the whole file. This is how the tools reach a KRFFS partition inside a disk image: the offset is the partition's byte offset (page-aligned), the size its byte length. A nonzero offset requires an explicit size; with no flags the file system spans the whole file, the RAM-disk case.

## Rach2 Build Scripts

* `mkimage.sh <manifest> <output.krffs> [size_mb]`: Builds one RAM-disk module image out of tree from a manifest (dd + mkfs.krffs + stageimage.krffs, no FUSE mount)
* `mkdisk.sh <manifest> <output.disk> [disk_mb] [partition_mb]`: Builds one bootable-layout disk image: a 512-byte MBR with one primary KRFFS partition (type 0x7F) at LBA 2048, then a fresh KRFFS laid down inside that partition with `mkfs.krffs --offset --size` and staged from the manifest. `disk_mb` defaults to 64 and `partition_mb` to 48, matching `make os-real-disk`. The partition is capped at 512 MiB, the kernel mount window (`PC_WINDOW_CAP` in `sys/mm/pagecache.c`)
* `manifests/<image>.manifest`: The build recipe for each image (test, os-simple, os-real, disk-test, persist-test, cred-persist-test, stress-test): the dirs and files to stage and their host sources
* `stageimage.krffs <manifest> <image> [--offset N --size N]`: Writes the whole tree (dirs and files) into a KRFFS image directly as v2 nodes
* `gen_demo_key.sh`: Provisions the os-real SSH demo keys when they are absent: `rach2_demo_ed25519` for root and `rach2_test_ed25519` for the unprivileged test account. It then composes each account's `authorized_keys` from the generated public half plus an optional `authorized_keys.extra` (or `test_authorized_keys.extra`) beside it, git-ignored, so whoever works on a machine can reach the images they build with their own key without the project's private one leaving the tree. It recomposes, not appends, so a rebuild never doubles a line and deleting the extra file removes the addition; a file holding a private key, or a line that is not a public key, fails the build instead of being staged into an image
* `gen_demo_password.sh`: Provisions the account passwords both shipped images share when `shadow` is absent: a fixed development password per account into the git-ignored `rach2_demo_password` (root, for `su`) and `rach2_test_password` (the SSH password login), hashed by `mkhash.krffs` into the git-ignored `shadow` the manifest stages as `/etc/shadow`. The passwords are constants in the script and are the same on every build, deliberately, because they are development credentials that have to be typed; only the `$6$` salt is drawn from `/dev/urandom`. A machine going somewhere real overrides them with `RACH2_ROOT_PASSWORD` and `RACH2_TEST_PASSWORD`, which is what `dev-log/25-sealed/checklist.md` does
* `create_file_system.krffs.sh [size_mb]`: Creates an empty KRFFS file system (helper script)

The GRUB-bootable disk lives one level up, in `tools/`, because it leans on the kernel and a Docker container, not on the KRFFS tools alone:

* `tools/mkdisk_grub.sh`: Builds a BIOS-bootable disk: a FAT32 boot partition holding the kernel, GRUB's immutable stub and the rewritable menu slot beside it, a KRFFS partition with the os-real tree, and optionally a third FAT32 partition for a recovery system, assembled in a throwaway Docker container (`make os-real-grub-disk`, `make rach2-metal-grub-disk`, `make rach2-sealed-disk`)
* `tools/grub_embed.py`: Writes GRUB's `boot.img` into the MBR and `core.img` into the post-MBR gap and patches their pointers, the install `grub-bios-setup` would do but cannot on a Docker overlay filesystem

## Structure

* `krffs.c`: FUSE driver, mounts file system from backing file
* `krffs_file_system.h/c`: File system struct, initialization (root + free node)
* `krffs_node.h/c`: Metadata node types (Root, Free, Reserved), traversal, search by name hash
* `krffs_allocator.h/c`: K&R allocator (allocate, resize, remove reserved nodes)
* `krffs_fuse_operations.h/c`: FUSE callbacks (getattr, readdir, read, write, truncate, rename, unlink, etc.)
* `krffs_platform.h/c`: Platform abstraction (mmap/munmap/msync, gettimeofday)
* `krffs_utilities.h/c`: DJB hash, size formatting

## Disk Layout

```
[Root Node][Free/Reserved Nodes...]
     |            |
     v            v
  (header)    (header + data)
```

Each node contains:
* `magic` (2 bytes): 0x4B52 ("KR")
* `type` (1 byte): Root=0, Free=1, Reserved=2
* `id` (8 bytes): name hash. In a v2 directory it mixes the containing directory's id with the name, so the same name under two directories hashes apart; the flat FUSE mount uses the plain djb2 hash of the name
* `name` (256 bytes): null-terminated filename
* `mode`, `uid`, `gid`: permissions/ownership
* `access_time`, `modification_time`, `attribute_change_time`: timestamps, in seconds and nanoseconds since the Unix epoch. The kernel writes them: creating, writing, truncating, renaming, unlinking and removing a directory entry all stamp what they changed, and a chmod or chown moves the attribute-change time alone. Access time is stored and reported but never advanced by a read, since this file system lives behind a write-back page cache and a read that stamped would be a read that wrote. The host stager writes one fixed instant, a second past the epoch, into every node it stages and into the root (mkfs stamps that from the building host's clock), for two reasons at once: constant, so a staged image is the same bytes on every build, which `tools/check_repro.py` has in the gate along with the sorted staging order both `tree` and `exec-dir` use; and nonzero, so a make run over the staged sources on the machine sees them as existing files, pdpmake reading a zero modification time as a file that is not there. Anything the machine builds is stamped by its real clock and is newer than any staged source
* `size` (8 bytes): total node size (header + data)
* `previous_node_size` (8 bytes): enables backward traversal
* `data_size` (8 bytes): actual file content size
* `parent_id` (8 bytes): id of the containing directory (the KRFFS v2 hierarchy)

Nodes form a circular linked list via size/previous_node_size fields.

## Limitations

* The interactive FUSE mount is flat (no `mkdir`); v2 directories are written by `stageimage`, not through the mount
* No hard links or symbolic links
* Single-threaded FUSE operation
* File names limited to 255 bytes

## Build

The Makefile in this `krffs` directory builds the host FUSE file system and its tools, which create the RAM-disk image files the Rach2 kernel boots.

Compile with:

    make

To remove compiled files:

    make clean

## Usage

Build the host tools first (`make` from the repo root, or `make -C krffs`); they land out of tree in `build/krffs/`. The steps below run in that directory, so the tools and the scratch image and mount point you create all stay in one git-ignored place:

    cd build/krffs

1. Create an empty 10-megabyte file `my.krffs` and initialize a new KRFFS file system on it with the compiled `mkfs.krffs` utility. You can also change the size to anything else reasonable. Remember that on the server you are using a shared environment.

        dd if=/dev/zero of=my.krffs bs=1048576 count=1
        ./mkfs.krffs my.krffs

   You can do the same by calling the helper script. It lives in the krffs source dir and writes the image into `build/krffs/` no matter where you run it from.

        ../../krffs/create_file_system.krffs.sh

2. Mount the file system from the `my.krffs` file into an empty directory `mount_point`.

        ./krffs my.krffs mount_point

   If the `mount_point` directory is not empty, remove the files from it.

        rm -rf mount_point/*
        ./krffs my.krffs mount_point

   One useful option is to tell the program to print debug information for each file system operation with the `-d` flag.

        ./krffs my.krffs mount_point -d

   You can also check the list of all FUSE options by passing just the `-h` flag.

        ./krffs -h

   You can see if your system is mounted by calling `df`. Your system should be somewhere at the bottom of the list.

        df -h

3. Go inside the directory `mount_point` and experiment with the file system by creating files and writing or reading data from them. Attempt to rename or remove files. Try to use unsupported operations. Find out what our file system is not capable of doing.

   Sample commands:

        cd mount_point
        touch hello.txt
        ls
        echo "hello, world" > hello.txt
        cat hello.txt
        echo "bye" >> hello.txt
        cat hello.txt
        rm hello.txt
        ls
        echo -e '#include <stdio.h>\n\nint main() {\n    printf("Hello, World!\\n");\n    return 0;\n}' > test.c
        mv test.c hello.c
        make hello
        ./hello
        SIZE=1M ; head -c $SIZE /dev/urandom > file_$SIZE.bin
        cp file_1M.bin file_1M_2.bin
        SIZE=4M ; head -c $SIZE /dev/urandom > file_$SIZE.bin
        rm file_1M_2.bin
        mkdir sample_dir # Huh?
        ...

4. Leave the `mount_point` directory and unmount it.

        cd ..
        sudo umount mount_point

   On some operating systems, a `fusermount` command can be used to unmount FUSE mount points without administrative privileges.

        fusermount -u mount_point

5. Visualize the file system structure with the `fsviz.krffs` program. The program uses the termbox2 [library](https://github.com/termbox/termbox2) to create an interactive interface. You can scroll with arrow keys if the visualization is too wide. Press CTRL+C to exit the program.

        ./fsviz.krffs my.krffs

## Rach2 Image Creation

Rach2 ships four purpose-built RAM-disk module images, each its own file under `build/images/`. They coexist; there is no single shared image.

* `test.krffs`: the freestanding syscall probes plus the musl integration programs. It stages no init, so the kernel falls to the debug shell, which `make test` drives to boot it headless and check every probe exits 0. Beside the probes it stages what a probe cannot mint for itself: the set-id fixtures (`suidprobe`'s helper at four modes and owners, `sudoprobe`'s reference sudo, `suidmusl`'s setuid copy, and the shipped setuid-root `su`), the permission trees `searchprobe` and `stickyprobe` assert before they touch anything, and a small `/etc`. That `/etc` now has three accounts: root, a `probe` account at uid 1000 whose shell is the `/idreport` helper and whose home is `/home/probe` (0700 and owned by the account), and a second unprivileged `probe2` at uid 1001, with a committed `/etc/shadow` line each and an `/etc/group` naming every group. The second account exists for one case and would be pointless without it: a lost update is only visible when two accounts change at once, so `passwdmusl` needs two callers with two passwords racing for one file. All three passwords are fixed test credentials, not secrets, and all three differ, so a probe offering one account's password for another is testing a refusal. `pwauthmusl` drives `su` through the first account and `passwdmusl` drives the shipped setuid `passwd` through both. The image also stages `/adduser` and, alone among the images, `/adduser-crash`: the same source built with `-DADDUSER_CRASH_POINT`, which takes a leading count and dies the instant that publication has landed, so `addusermusl` proves the publication order by injection, not by a reading of the source. It is a fixture like `idreport` and `suidhelper` and not a probe, os-real never has it, and neither artifact has a set-id bit, which the manifest sweep would refuse.
* `os-simple.krffs`: a small Unix built out of this project's own code, and the counterpart to os-real, which takes the same jobs from busybox and dropbear. Reading one against the other is the point of having both. `/sbin/init` here is the strict `init-login` artifact, so the console asks who is there before it offers a shell: `/bin/login` authenticates against the same `/etc/shadow` os-real stages (generated by `krffs/gen_demo_password.sh` when absent, which `make` runs), and execs the account's shell, which is the ISH shell at `/bin/ish`. Everything else is in `/bin` as well, because that is the `PATH` `login` and `su` hand an account. The account programs are the four in `utils/`: `su` and `passwd` carry the set-id bit, since they read a file that is root's alone, and `login` and `adduser` do not, being run by something that is root already. ISH is staged a second time as `/bin/sh`, since KRFFS has no symlinks and that is the shell path `adduser` writes into a new account's record. It stops with `halt` (`utils/halt.c`), which the `exec-dir` sweep stages for it; os-real does not carry that binary, since busybox answers those three names as applets before the file system is consulted.
* `os-real.krffs`: a real Unix-like environment: busybox (shell + coreutils), dropbear (SSH server and client), Doom, and an `/etc` + `/root` tree. The kernel boots `/sbin/init`, which here is the strict `init-login` artifact: its session child execs `/bin/login` and nothing else, so the console asks who is there before it offers a shell, and an image missing that program refuses instead of falling back to one. `rach2.rescue` is the way back into an image that will not offer a prompt; the SSH capstone lives here. It also has a compiler and a make: `/bin/tcc` (staged again as `/bin/cc`, the name a makefile's built-in rules reach for) and `/bin/make`, with the sysroot the compiler was built to look in (`/lib/libc.a` and the startup objects, `/lib/tcc/libtcc1.a` and tcc's own headers, and musl's headers at `/include`), and the sources the machine can rebuild itself from: this project's own tools at `/src/utils`, the kernel at `/src/rach2` and the ish shell at `/src/ish`, each tree carrying the `guest.mk` that pdpmake reads (`make selfbuild-test` builds the kernel and the userland from copies of these trees). `/src` is root-owned and read-only, so root can build in any of the three in place with `make -f guest.mk` and every other account copies its tree home first. The account tools are busybox's own, from a second build of the same source holding exactly `login`, `passwd` and `su`: it is staged three times under those names, since KRFFS has no symlinks and busybox dispatches on `argv[0]`, and it has the set-id bit that the main busybox must not have. `adduser`, `addgroup`, `deluser`, `delgroup` and `chpasswd` are applets of the main binary and need no bit, being root's.
* `rach2-metal.krffs`: what the Lenovo G50-70 boots from a USB stick (`dev-log/24-metal/plan.md`). GRUB reads the stick through the BIOS, but the kernel has no USB driver, so the module GRUB loads is the entire machine: nothing persists and every byte costs RAM under the 32 MiB direct map. It has busybox, dropbear, `ping`, the same account files and keys os-real uses, and `init-metal`, which is the strict `init-login` plus the two things a machine nobody is standing at needs: it brings the network up and supervises the SSH server. It leaves out the compiler and its sysroot, pdpmake, the `/src` trees, Doom and `bootctl`, which a machine with no writable disk cannot use. 4 MB has it with room to spare (about 2.4 MB used), and `make rach2-metal-iso` turns it into the bootable image the stick is written from, whose menu has three entries: the machine, a diagnostics boot (`rach2.diag`, which prints the boot layout and the Ethernet identity for a laptop with no serial port), and the kernel debug shell.

The top-level Makefile drives the build. Run from the repo root:

    make os-simple-image   # our login, shell and utilities (what plain `make` builds)
    make test-image        # the syscall probes + musl programs
    make os-real-image     # the busybox/dropbear/Doom environment, with tcc and make
    make rach2-metal-image # the RAM-only image the Lenovo G50-70 boots from a stick
    make images            # all three
    make full              # all three (ends at the os-real image the SSH login boots)

Each target builds its inputs through the component sub-makes, then stages the image from its manifest. The staging is manifest-driven: `mkimage.sh` zeroes a file of the requested size, lays down a fresh KRFFS with `mkfs.krffs`, then runs `stageimage.krffs <manifest> <image>` to write the whole tree (directories and files) directly as v2 nodes. There is no FUSE mount, so it works the same on macOS and Linux.

A manifest lives in `manifests/<image>.manifest` and lists the dirs and files to stage and their host sources. Each entry's mode field is octal permissions with an optional owner, `0755` or `0700:1000:1000`; without one the entry is root-owned (0:0), and `stageimage` also forces the root node itself to 0:0, whoever built the image. A path that has a setuid or setgid bit must also be declared, with that exact mode and owner, in the per-image allowlist `tools/check_setid.py` holds; an undeclared one fails the build, not a boot, as does a declared one nothing stages. The same allowlist holds a second list, the files an image must carry exactly as declared although they carry no set-id bit: the programs root runs (`/bin/sh` is the one login and dropbear exec for it) and the files root reads to decide who somebody is (`/etc/shadow` and the rest of `/etc`, and root's `authorized_keys`), pinned by mode, owner, host source, and for the ones staged inline their text as well. The `dir`, `exec-dir` and `tree` directives may carry no set-id bit at all: a set-id `exec-dir` or `tree` would privilege whatever happened to be in the host directory, which is not something a manifest can declare, so a program that must be privileged is named on its own `file` line. `exec-dir MODE GUESTDIR HOSTDIR` stages the regular executables of ONE host directory, in name order, so that one host tree always produces one image whatever order the host's own file system reports its entries in. `tree MODE GUESTDIR HOSTDIR` is the recursive one, for a tree of source, not a directory of build products: `MODE` is the mode of every file it stages, the directories it creates along the way get 0755 with the same owner, only regular files are staged and anything else under the host tree is an error, not a silent skip (KRFFS has no symlinks, and dropping one quietly would make the image differ from the tree it came from), a zero-byte file stays a zero-byte file, and the walk is sorted so one host tree always produces one image. A directory the tree finds already staged is left exactly as the line that declared it made it: `dir` says what a directory is, and a tree walking into one must not hand it a different owner. The host directory itself must be a directory and not a symlink to one, and a directory that cannot be read through is an error, not a short tree published as a whole one. Both halves of that guard run at every image build, before the file is moved into place: the sweep over the manifest about to be staged, and then the same guard over the staged image, comparing the nodes it finds against the same list. The second is what catches the two things the manifest text cannot show, what `exec-dir` expanded to and any drift between the guard's reading of the grammar and the stager's; the first is what catches everything a manifest declares and an image cannot record, such as which directive staged a path and what host file it came from. Both shipped images run `gen_demo_password.sh` first to provision the account passwords and their `/etc/shadow` when they do not exist, and `os-real` also runs `gen_demo_key.sh` for the SSH keys.

Boot a module image with the run script (it resolves `build/images/<image>.krffs`):

    cd ../rach2 && ./run.sh os-simple

### Disk Images

The same os-real tree also goes onto a real disk image, which the kernel mounts from a polled-PIO ATA disk through its page cache, not from a RAM module. `mkdisk.sh` builds the layout (an MBR with one KRFFS partition); the KRFFS bytes inside the partition are byte-identical to a module image.

    make os-real-disk        # build/images/os-real.disk: MBR + a 48 MiB KRFFS partition
    make os-real-grub-disk   # build/images/os-real-grub.disk: BIOS-bootable, GRUB + a FAT boot partition + KRFFS (needs Docker)
    make rach2-sealed-disk   # build/images/rach2-sealed.disk: the sealed laptop disk, 630 MiB (needs Docker)

The boot partition holds five files, all preallocated when the disk is built so no later write extends one or moves its cluster chain. `/boot/kernel` is the candidate slot `bootctl kernel` rewrites and `/boot/fallback` the untouched copy. `/boot/grub/grubenv` is the 1024-byte state block that selects between them. `/boot/grub/grub.cfg` is the immutable stub: it defines every entry the machine needs to boot, in a fixed order because `set fallback` names an index and not a name, and only then sources `/boot/boot.cfg`, the rewritable menu slot `bootctl config` overwrites in place. A staged menu that is missing, truncated mid-write, or simply wrong therefore costs nothing but the entries it meant to add: the stub's own are already defined and hold the low indices.

A staged menu is GRUB script that `source` runs, so whoever writes it can direct the boot: it can set the environment keys the stub reads, or load a kernel itself. That is the same authority replacing the candidate kernel already has. What the stub guarantees is different and narrower: every entry the machine needs is defined before the staged file is read, so a menu that is missing, truncated mid-write or unparseable leaves the machine booting.

Attaching that image is booting it: `DISK=` mounts KRFFS from the platter, so what comes up is the writable Rach2, not the disposable RAM module.

    cd ../rach2 && DISK=../build/images/os-real.disk ./run.sh

An account created or a password changed on this machine is still there after the power goes off, which is the whole difference between it and the module: the module is a copy of the image made at boot, so every change to it dies with the process. The page cache is write-back, so an edit is on the platter after a `sync` and not before; `passwd` and `adduser` flush for themselves, and anything else typed at the shell wants a `sync` before the machine is killed. Userland can ask for a clean unmount: `reboot(2)` is syscall 88 and root's alone, and a bare `halt`, `poweroff` or `reboot` reaches it through init, which is pid 1 and answers the signal those programs send; `-f` on any of them calls it directly instead, and the debug shell has the same three commands. Each flushes the cache and, only if that flush returned 0, clears the dirty-mount flag. A boot that is killed, not stopped still leaves the flag set, and the next boot still prints the fsck advice, which is now a statement about that boot, not about every boot.

To attach the disk without mounting it, which is the older recipe and now has to be asked for, add `DISKNOMOUNT=1`: the kernel boots the RAM module and the debug shell's `disk` command probes the platter as an unmounted device.

    cd ../rach2 && DISK=../build/images/os-real.disk DISKNOMOUNT=1 RESCUE=1 ./run.sh

The `os-real-grub-disk` image has its own kernel and GRUB, so it boots with no `-kernel`, the way a real machine would, BIOS to MBR to GRUB to kernel to KRFFS:

    cd ../rach2 && ./run_from_disk.sh

`run_from_disk.sh` forwards host port 2222 to the guest's SSH server by default (override with `NET_HOSTFWD`), so the SSH capstone reaches Dropbear over the disk boot: log in at `rach2 login:` as root and start `dropbearmulti dropbear -R -g -p 22` from that session (the `-g` bars root's password from the SSH authentication exchange; test also takes its generated password), then `ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost` (or `-i krffs/rach2_test_ed25519 test@localhost`, or `test@localhost` with the password in `rach2_test_password`) from the host.

The disk targets also back the integrity gate: `make test-disk`, `make persist-test`, `make cred-persist-test`, `make crash-test`, `make shutdown-test`, and `make stress-test` build disk images from their own manifests and boot them headless to prove cache eviction, fsync durability, the credential replacement's durability, and the dirty-mount flag. The `cred-persist-test` manifest stages the `creddisk` probe, the two files it replaces (root-owned at two different modes, so the inheritance is read, not assumed), and the `/etc` the credential lock lives in.
