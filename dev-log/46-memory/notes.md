# Milestone 46 notes

## Phase 1: the frame map

* QEMU's maps at the five sizes, as the kernel printed them, are in `tests/host/frame_hosttest.c`; the boot's record of the map is what milestone 50 reads off the Lenovo's netconsole. QEMU's map has three fixed holes, the kilobyte at 0x9FC00, the 384 KiB below 1 MiB and the 128 KiB at the end of low RAM, and puts what is above 3 GiB at 4 GiB.
* The frame table is one 32-bit word per frame below the map's top and lives below the window with the other boot tables: 4 MiB on a 4 GiB machine, inside the 32 MiB with the 16 MiB os-real image. A frame the map offers above the window is managed and marked not yet reachable; phase 2 makes it reachable.
* The boot reports managed from the table's count and not from the map's arithmetic, so the memory suite's check that managed is usable less reserved compares two computations. The mutation that catches it leaves the boot's own tables unreserved; the mutation that treats every frame as usable is invisible on QEMU's maps, whose holes lie below the break or past the table's top, and is the host test's to catch.

## Phase 2: the layout and the heap

* The kernel's gigabyte gets all 256 page tables at boot, a megabyte below the window, so a clone of the directory shares them and a mapping made later is seen by every process; the boot self-test maps a page after cloning a directory and reads it through the clone.
* `map_mmio` returns an address in the MMIO region and eight drivers use what it returns; the diagnostic, the two network drivers, AHCI and HDA used the physical address as a pointer.
* The physical-address writers were the lazy zero in `vm_resolve`, the copy-on-write copy, the loader's four sites, and the initial user stack in `execve` and `exec_program`; the last two were found by the 64 MiB probe faulting in kernel mode at 45 MiB.
* A scan for a free frame from the bottom of the table on every page fault is the table's length squared over a large mapping; a hint from which no frame below is free keeps it linear.
* `munmap` leaves the mapping's page tables with the process until it exits, one page per 4 MiB; the probe's leak check allows them.
* The `rach2.framefail` count counts only allocations made inside a system call, so a page fault of the process arming it is not the one that fails; at 64 MiB the sweep of sixty-four arms fired ten times, since a count past an operation's allocations fires on nothing.
* The modules are mapped into the module region on every boot, so QEMU's placement below the window exercises the path; a module above the window is a case no QEMU boot here makes.
