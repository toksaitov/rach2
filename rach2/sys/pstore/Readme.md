# pstore

A crash record that survives a reset, as Linux 6.12's pstore with its ramoops back end keeps one (`fs/pstore/`). With the `ramoops.` options the kernel takes a region of RAM from its allocator at boot; a panic writes the end of the log into it, and the console writes every record of the log into it as the log takes it. A warm reset keeps RAM, so the next boot finds what the region holds, shows each record as a file under `/sys/fs/pstore`, and erases a record when its file is removed. Nothing is written to the firmware's variables.

## Use

Give the region on the kernel's line, as on Linux:

```
ramoops.mem_address=0x40000000 ramoops.mem_size=0x100000
ramoops.record_size=0x10000 ramoops.console_size=0x10000 ramoops.ecc=1
```

After a panic and a reset, `/sys/fs/pstore/dmesg-ramoops-0` holds the end of the log at the panic and `console-ramoops-0` the last boot's log. `rm` of a file erases its record. [pstore(5)](../../../man/man5/pstore.5) has every option and file, and [cmdline.md](../../cmdline.md) the line's other options. The region must lie in RAM that the firmware keeps across a warm reset and that no boot module or the kernel uses; the kernel refuses one that is partly usable memory or holds the kernel or a module, with a record of why.

## Interfaces

* `pstore.h`: `ramoops_reserve`, called by `kernel_main` before `init_paging`, reads the options and takes the region out of the frame map; `ramoops_init`, once paging and the heap are up, maps it, lays out its zones and keeps their old contents; `pstore_start`, once `/sys` is mounted, makes `/sys/fs/pstore` and mounts the records there; `pstore_console_write`, from `klog`'s ring with interrupts off; and `pstore_panic_dump`, which `ramoops_init` sets as `k_panic_dump`, so that `k_panic` calls it after its record and its report says "crash record saved" when it returns 1.
* `ram_core.h`: a persistent RAM zone, `struct prz`, and the Reed-Solomon code that guards it.
* `vfs_pstore_type`: the file system, which `mount(2)` also takes as `pstore`.

## Design

The region is laid out as Linux's `ramoops_probe` lays it out (`ram.c:741`, `ramoops_init_przs` at `ram.c:524`), so that both kernels find the same zones at the same options: the dmesg zones first, as many as the region less the console, ftrace and pmsg zones holds of `record_size`, each the same even size; then the console zone; then the pmsg and ftrace zones, which this kernel leaves unwritten. Each zone is Linux's `persistent_ram_buffer` (`ram_core.c:33`): a 12-byte header of the signature 0x43474244, the offset of the next byte to write and the bytes held, then the data, which wraps. With `ramoops.ecc`, the data is cut into blocks of 128 bytes, and each block and the header have Linux's Reed-Solomon parity (`ram_core.c:209`, `init_rs(8, 0x11d, 0, 1, nroots)` at `ram_core.c:230`), written after the data, so a region written by either kernel reads in the other. The decoder is the textbook one: syndromes, Berlekamp and Massey's locator, Chien's search and Forney's values.

The header's fields are read and written a byte at a time through the mapping, and the parity of a block is computed from the mapping's bytes, so that what the next boot reads is what is in RAM. The region is mapped write-combining by default, as Linux maps it (`ram_core.c:414`), and each write drains the write buffers (`arch_dma_fence`), since a reset discards what a buffer still holds.

A panic writes once: Linux's header, `====SECONDS.MICROSECONDS-D` (`ram.c:333`), with the time read without the clock's lock, which the panicking processor may hold; then `Panic#1 Part1` (`platform.c:323`); then the newest whole records of the log that fit, from the ring. It allocates nothing and takes no lock. The console zone takes each record as `klog` puts it in the ring, in the same call with interrupts off, so it has the ring's one writer at a time; a panic writes only a dmesg zone, so two writers never meet in a zone.

At boot, `prz_init` mends each block and the header by the parity, keeps the data, oldest byte first, when the header holds the signature and a size and start within the data, and otherwise writes the signature and empties the zone, as `persistent_ram_post_init` does (`ram_core.c:528`). A dmesg zone whose data begins with the header becomes a record, the header parsed for its time and taken off the record (`ram.c:149`, `ram.c:178`); the console zone, if it held anything, becomes `console-ramoops-0` and is then emptied for this boot (`PRZ_FLAG_ZAP_OLD`). A record read with parity ends with Linux's notice (`ram_core.c:258`). Removing a file erases its zone (`inode.c:200`); a panic takes the dmesg zones in turn from the first, as Linux's `dump_write_cnt` does, so a record not removed lasts until a later panic writes over it.

The file system is one directory of files that are records in kernel memory, made at boot and freed when their last holder lets go after an unlink. Modes and names are Linux's (`inode.c:360` to `inode.c:424`). The kernel mounts it inside `/sys`, as debugfs is, by name and not over a directory, since `/sys` is synthetic; unlike debugfs the mount is not permanent, as systemd's on Linux is not.

## Limits

* The kernel has no oops it survives, so only a panic writes a dmesg record; there are no ftrace or pmsg records and no compression.
* The region must lie below 4 GiB and within what the kernel maps.

## Tests

`tests/host/pstore_hosttest.c` checks the Reed-Solomon code against blocks with errors in data and parity, a zone's header, wrap and second-boot copy, and the parity's mending across a reset. `machine.watchdog.pstore` boots q35 with a region and the TCO armed by the kernel, panics, lets the TCO reset the guest, and reads the record back on the second boot.
