# Milestone 46 notes

## Phase 1: the frame map

* QEMU's maps at the five sizes, as the kernel printed them, are in `tests/host/frame_hosttest.c`; the boot's record of the map is what milestone 50 reads off the Lenovo's netconsole. QEMU's map has three fixed holes, the kilobyte at 0x9FC00, the 384 KiB below 1 MiB and the 128 KiB at the end of low RAM, and puts what is above 3 GiB at 4 GiB.
* The frame table is one 32-bit word per frame below the map's top and lives below the window with the other boot tables: 4 MiB on a 4 GiB machine, inside the 32 MiB with the 16 MiB os-real image. A frame the map offers above the window is managed and marked not yet reachable; phase 2 makes it reachable.
* The boot reports managed from the table's count and not from the map's arithmetic, so the memory suite's check that managed is usable less reserved compares two computations. The mutation that catches it leaves the boot's own tables unreserved; the mutation that treats every frame as usable is invisible on QEMU's maps, whose holes lie below the break or past the table's top, and is the host test's to catch.
