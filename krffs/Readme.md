# KRFFS File System

![KRFFS Visualization Utility's Output](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/fsviz.png)

A minimal educational FUSE file system in C. The project stores files inside a regular file using a flat structure inspired by the K&R first-fit memory allocator from "The C Programming Language." Files are memory-mapped via `mmap`/`MapViewOfFile`, allowing direct read/write access through virtual memory. The codebase uses a platform abstraction layer to support Unix, Linux, macOS, and Windows, making it suitable for porting to custom OS environments.

The code in the `krffs` directory HERE is for generating RAM disk images for Rach2 development and testing on host environments running the latest macOS, Ubuntu, or Windows.

## Features

* Hierarchical v2 directories (parent-id nodes) the kernel and the `stageimage` builder read and write; the interactive FUSE mount itself stays flat (one root)
* K&R first-fit allocator: block splitting, coalescing adjacent free blocks
* Memory-mapped I/O: changes written directly to backing file
* Packed metadata nodes with magic signatures for consistency checks
* File operations: create, read, write, truncate, rename, delete
* Permissions: chmod, chown, timestamps (atime, mtime, ctime)

## Utilities

* `mkfs.krffs`: create new file system
* `fsck.krffs`: check file system consistency (links, signatures, node types)
* `defrag.krffs`: defragment by moving free blocks to end
* `fsviz.krffs`: interactive terminal visualizer (termbox2)
* `edfs.krffs`: low-level node field editor for debugging/repair

## Rach2 Build Scripts

* `mkimage.sh <manifest> <output.krffs> [size_mb]`: Builds one RAM-disk image out of tree from a manifest (dd + mkfs.krffs + stageimage.krffs, no FUSE mount)
* `manifests/<image>.manifest`: The build recipe for each image (test, os-simple, os-real): the dirs and files to stage and their host sources
* `stageimage.krffs <manifest> <image>`: Writes the whole tree (dirs and files) into a KRFFS image directly as v2 nodes
* `gen_demo_key.sh`: Provisions the os-real SSH demo key (`rach2_demo_ed25519`) when none exists
* `create_file_system.krffs.sh [size_mb]`: Creates an empty KRFFS file system (helper script)

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
* `id` (8 bytes): DJB hash of filename
* `name` (256 bytes): null-terminated filename
* `mode`, `uid`, `gid`: permissions/ownership
* `access_time`, `modification_time`, `attribute_change_time`: timestamps
* `size` (8 bytes): total node size (header + data)
* `previous_node_size` (8 bytes): enables backward traversal
* `data_size` (8 bytes): actual file content size

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

## Rach2 RAM Disk Creation

Rach2 ships three purpose-built RAM-disk images, each its own file under `build/images/`. They coexist; there is no single shared image.

* `test.krffs`: the freestanding syscall probes plus the musl integration programs. `make test` boots it headless and checks every probe exits 0.
* `os-simple.krffs`: the ISH shell (booted as init) and the hand-written Unix utilities. The small, readable teaching image.
* `os-real.krffs`: a real Unix-like environment: busybox (shell + coreutils), dropbear (SSH server and client), Doom, and an `/etc` + `/root` tree. The kernel boots `/bin/sh` as init; the SSH capstone lives here.

The top-level Makefile drives the build. Run from the repo root:

    make os-simple-image   # the ISH shell + utilities (this is what plain `make` builds)
    make test-image        # the syscall probes + musl programs
    make os-real-image     # the busybox/dropbear/Doom environment
    make images            # all three
    make full              # all three (ends at the os-real image the SSH login boots)

Each target builds its inputs through the component sub-makes, then stages the image from its manifest. The staging is manifest-driven: `mkimage.sh` zeroes a file of the requested size, lays down a fresh KRFFS with `mkfs.krffs`, then runs `stageimage.krffs <manifest> <image>` to write the whole tree (directories and files) directly as v2 nodes. There is no FUSE mount, so it works the same on macOS and Linux.

A manifest lives in `manifests/<image>.manifest` and lists the dirs and files to stage and their host sources. `os-real` also runs `gen_demo_key.sh` first to provision the SSH demo key when none exists.

Boot an image with the run script (it resolves `build/images/<image>.krffs`):

    cd ../rach2 && ./run.sh os-simple
