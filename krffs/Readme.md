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
* `fsck.krffs`: check file system consistency (links, signatures, node types), and repair two things it finds. It writes to the image: it clears the dirty-mount flag a crash left set, and it frees every orphan, a reserved node whose name was cleared when the kernel took it out of the namespace while a descriptor still held it, which a shutdown that closed no descriptors can strand with nothing left to free it. Both repairs happen only after the structural walk has validated the chain end to end, so an image it reports as corrupt is left exactly as it was found. The kernel reaps orphans at mount as well, so running this is not required to recover the space, only to recover it offline
* `defrag.krffs`: defragment by moving free blocks to end
* `fsviz.krffs`: interactive terminal visualizer (termbox2)
* `edfs.krffs`: low-level node field editor for debugging/repair
* `mkhash.krffs`: mint a SHA-512 crypt (`$6$`) password hash for `/etc/shadow`; the password comes on stdin, the `$6$salt` setting as the argument, and the crypt is the vendored musl implementation, so the host mints exactly what the guest's libc verifies

Every tool that opens a backing file (`mkfs.krffs`, `fsck.krffs`, `defrag.krffs`, `fsviz.krffs`, `edfs.krffs`, `stageimage.krffs`) accepts `--offset N --size N` to operate on a region inside a larger file rather than the whole file. This is how the tools reach a KRFFS partition inside a disk image: the offset is the partition's byte offset (page-aligned), the size its byte length. A nonzero offset requires an explicit size; with no flags the file system spans the whole file, the RAM-disk case.

## Rach2 Build Scripts

* `mkimage.sh <manifest> <output.krffs> [size_mb]`: Builds one RAM-disk module image out of tree from a manifest (dd + mkfs.krffs + stageimage.krffs, no FUSE mount)
* `mkdisk.sh <manifest> <output.disk> [disk_mb] [partition_mb]`: Builds one bootable-layout disk image: a 512-byte MBR with one primary KRFFS partition (type 0x7F) at LBA 2048, then a fresh KRFFS laid down inside that partition with `mkfs.krffs --offset --size` and staged from the manifest. `disk_mb` defaults to 64 and `partition_mb` to 48, matching `make os-real-disk`. The partition is capped at 256 MiB, the kernel mount window (`PC_WINDOW_CAP` in `sys/mm/pagecache.c`)
* `manifests/<image>.manifest`: The build recipe for each image (test, os-simple, os-real, disk-test, persist-test, cred-persist-test, stress-test): the dirs and files to stage and their host sources
* `stageimage.krffs <manifest> <image> [--offset N --size N]`: Writes the whole tree (dirs and files) into a KRFFS image directly as v2 nodes
* `gen_demo_key.sh`: Provisions the os-real SSH demo keys when they are absent: `rach2_demo_ed25519` for root and `rach2_test_ed25519` for the unprivileged test account
* `gen_demo_password.sh`: Provisions the os-real account passwords when `shadow` is absent: a random password per account into the git-ignored `rach2_demo_password` (root, for `su`) and `rach2_test_password` (the SSH password login), hashed by `mkhash.krffs` into the git-ignored `shadow` the manifest stages as `/etc/shadow`
* `create_file_system.krffs.sh [size_mb]`: Creates an empty KRFFS file system (helper script)

The GRUB-bootable disk lives one level up, in `tools/`, because it leans on the kernel and a Docker container rather than on the KRFFS tools alone:

* `tools/mkdisk_grub.sh`: Builds a BIOS-bootable disk: a FAT boot partition holding the kernel and GRUB, plus a KRFFS partition with the os-real tree, assembled in a throwaway Docker container (`make os-real-grub-disk`)
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
* `access_time`, `modification_time`, `attribute_change_time`: timestamps
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

The Makefile of this `krffs` directory builds with host version of the FUSE files system to allow to create RAM disk files to be used in the Rach2 kernel.

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

Rach2 ships three purpose-built RAM-disk module images, each its own file under `build/images/`. They coexist; there is no single shared image.

* `test.krffs`: the freestanding syscall probes plus the musl integration programs. It stages no init, so the kernel falls to the debug shell, which `make test` drives to boot it headless and check every probe exits 0. Beside the probes it stages what a probe cannot mint for itself: the set-id fixtures (`suidprobe`'s helper at four modes and owners, `sudoprobe`'s reference sudo, `suidmusl`'s setuid copy, and the shipped setuid-root `su`), the permission trees `searchprobe` and `stickyprobe` assert before they touch anything, and a small `/etc`. That `/etc` now carries two accounts: root, and a `probe` account at uid 1000 whose shell is the `/idreport` helper and whose home is `/home/probe` (0700 and owned by the account), with a committed `/etc/shadow` line each and an `/etc/group` naming both groups. Both passwords are fixed test credentials rather than secrets, and they are different from each other, so a probe offering one account's password for the other is testing a refusal. `pwauthmusl` drives `su` through that account.
* `os-simple.krffs`: `/sbin/init` (which forks the ISH shell) and the hand-written Unix utilities. The small, readable teaching image.
* `os-real.krffs`: a real Unix-like environment: busybox (shell + coreutils), dropbear (SSH server and client), Doom, and an `/etc` + `/root` tree. The kernel boots `/sbin/init`, which forks busybox `/bin/sh`; the SSH capstone lives here.

The top-level Makefile drives the build. Run from the repo root:

    make os-simple-image   # the ISH shell + utilities (this is what plain `make` builds)
    make test-image        # the syscall probes + musl programs
    make os-real-image     # the busybox/dropbear/Doom environment
    make images            # all three
    make full              # all three (ends at the os-real image the SSH login boots)

Each target builds its inputs through the component sub-makes, then stages the image from its manifest. The staging is manifest-driven: `mkimage.sh` zeroes a file of the requested size, lays down a fresh KRFFS with `mkfs.krffs`, then runs `stageimage.krffs <manifest> <image>` to write the whole tree (directories and files) directly as v2 nodes. There is no FUSE mount, so it works the same on macOS and Linux.

A manifest lives in `manifests/<image>.manifest` and lists the dirs and files to stage and their host sources. Each entry's mode field is octal permissions with an optional owner, `0755` or `0700:1000:1000`; without one the entry is root-owned (0:0), and `stageimage` also forces the root node itself to 0:0, whoever built the image. A path that carries a setuid or setgid bit must also be declared, with that exact mode and owner, in the per-image allowlist `tools/check_setid.py` holds; an undeclared one fails the build rather than a boot, as does a declared one nothing stages. The `dir` and `exec-dir` directives may carry no set-id bit at all: a set-id `exec-dir` would privilege whatever happened to be built into the host directory, which is not something a manifest can declare, so a program that must be privileged is named on its own `file` line. The same guard reads the staged image afterwards and compares the set-id nodes it finds against the same list, which is what catches the two things the manifest text cannot show: what `exec-dir` expanded to, and any drift between the guard's reading of the grammar and the stager's. `os-real` also runs `gen_demo_key.sh` and `gen_demo_password.sh` first to provision the demo credentials (the SSH keys and the account passwords with their `/etc/shadow`) when they do not exist.

Boot a module image with the run script (it resolves `build/images/<image>.krffs`):

    cd ../rach2 && ./run.sh os-simple

### Disk Images

The same os-real tree also goes onto a real disk image, which the kernel mounts from a polled-PIO ATA disk through its page cache rather than from a RAM module. `mkdisk.sh` builds the layout (an MBR with one KRFFS partition); the KRFFS bytes inside the partition are byte-identical to a module image.

    make os-real-disk        # build/images/os-real.disk: MBR + a 48 MiB KRFFS partition
    make os-real-grub-disk   # build/images/os-real-grub.disk: BIOS-bootable, GRUB + a FAT boot partition + KRFFS (needs Docker)

Attach the `os-real-disk` image as the primary ATA disk and drop to the debug shell, where the `disk` command probes it:

    cd ../rach2 && DISK=../build/images/os-real.disk RESCUE=1 ./run.sh

The `os-real-grub-disk` image carries its own kernel and GRUB, so it boots with no `-kernel`, the way a real machine would, BIOS to MBR to GRUB to kernel to KRFFS:

    cd ../rach2 && ./run_from_disk.sh

`run_from_disk.sh` forwards host port 2222 to the guest's SSH server by default (override with `NET_HOSTFWD`), so the SSH capstone reaches Dropbear over the disk boot: start `dropbearmulti dropbear -R -g -p 22` at the guest shell (the `-g` keeps root pubkey-only; test also takes its generated password), then `ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost` (or `-i krffs/rach2_test_ed25519 test@localhost`, or `test@localhost` with the password in `rach2_test_password`) from the host.

The disk targets also back the integrity gate: `make test-disk`, `make persist-test`, `make cred-persist-test`, `make crash-test`, and `make stress-test` build disk images from their own manifests and boot them headless to prove cache eviction, fsync durability, the credential replacement's durability, and the dirty-mount flag. The `cred-persist-test` manifest stages the `creddisk` probe, the two files it replaces (root-owned at two different modes, so the inheritance is read rather than assumed), and the `/etc` the credential lock lives in.
