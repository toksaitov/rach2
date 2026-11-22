# 5673: the disk suites that ran slower under KVM than under TCG

In task 5661's full run on the testing host, stress, stress-ahci, fatwrite, ext2 and metal took longer under KVM than under TCG. In each, the time goes to exits: every read or write of an emulated device's register leaves the guest for QEMU, and under the testing host's KVM, which runs nested under Hyper-V in WSL2, an exit costs the vCPU thread some 14 us out of the guest, where under TCG a register access is a call inside QEMU. fatwrite, ext2 and metal moved files and output through the 16550 serial console, a register access a byte; stress and stress-ahci make 19 to 37 register accesses for each disk command. Run alone, stress and stress-ahci take about as long under KVM as under TCG; in the full run they shared the host with KVM guests that exit as heavily.

Rach2 made more register accesses than it needs in four places, which this task changes:

* The serial console read the line status before every byte it sent.
* The ATA driver read the status eight times before it slept for a DMA command's or a flush's end, and the alternate status four times for each 400 ns pause.
* The AHCI driver read a port's interrupt status at every poll of a polled command.
* fatwrite's `volume` case and ext2's seven probe cases fetched files from the image helper over COM1, since they did not declare the helper's `image-transfer` disk.

With the changes, merged with 5672's change of the flush, fatwrite, ext2 and stress-ahci take less time under KVM than under TCG on i686, x86_64 and the ia32 profile, and stress the same to within a second. metal still takes longer under KVM; what is left of its cost and of the others' is below.

## How it was measured

Each suite ran alone, in this account's clone on the testing host, at origin/milestone/76-display (fade2fd53) and at the task's commits, with 8 workers and 16 GiB as the full run gives them. Times are each suite's sum of case times from etude's history, as in 5661.

QEMU 10.2.1 counted the exits itself. `tests/run` found a wrapper of `qemu-system-i386` first on its path, which added `-trace memory_region_ops_*` (every guest access of an emulated region, with its address and value) or `-trace kvm_run_exit` (every exit to QEMU, by reason) and wrote the trace beside the run. `perf kvm stat` and KVM's debugfs need root, which the account does not have. The vCPU thread's time in the guest and out of it comes from `/proc/PID/task/TID/stat`, fields 14, 15 and 43 (utime, stime, guest_time), sampled each second. strace of QEMU's flushes was tried and dropped: under KVM it slowed stress from 20 s to 87 s even with `--seccomp-bpf`, so its times mean nothing there. Its counts agreed with TCG's: 2,866 and 2,821 flushes of 2 ms each.

The other accounts' work kept the host's load between 1 and 68 during the runs, and a load above 8 moves a suite's time by as much as the change does. The times below therefore come in pairs run one after the other, and each pair started only once the load average was under 8: the old drivers and then the new, for each profile and accelerator; and at the task's head, KVM and then TCG. The counts of register accesses do not move with the load, and the conclusions rest on them; the times check them.

## fatwrite: the serial console

Alone, fatwrite took 150 s under KVM and 51 s under TCG on i686, in the first set of runs; its `volume` case took 125 s against 31 s. All but 25 s of the KVM time was one boot of the `volume` case: the image helper that reads `/bin/busybox` out of the volume and gives it to the host for `host-copy`. The case did not declare the helper's `image-transfer` disk, so etude-guest sent the file's 922,636 bytes over COM1, as 11,829 `data` frames of base64, 2.06 MB on the wire. The host answered each frame with an acknowledgement, 0.72 MB.

QEMU counted 5,847,784 exits in that boot, 5,845,315 of them port I/O (reason 2):

| Register | Accesses | Per byte sent |
|---|---|---|
| COM1 line status, 0x3FD, read | 2,854,970 | 1.37 |
| COM1 transmit holding, 0x3F8, write | 2,079,530 | 1 |
| COM1 receive buffer, 0x3F8, read | 716,105 | |
| VGA CRTC index and data, 0x3D4 and 0x3D5, write | 190,188 | |

`serial_out` read the line status before every byte and wrote the byte when the transmitter was empty. Linux's 8250 driver writes up to the FIFO's 16 bytes each time it finds the transmitter empty (`serial8250_tx_chars`, `tx_loadsz` in drivers/tty/serial/8250/8250_port.c). `serial_out` now does the same. Once `serial_init` sees, in the interrupt identification register, that the 16550A's FIFOs are on, it writes 16 bytes for each read that finds the transmitter empty; on a UART without FIFOs it writes one. The same boot then made 3,897,125 accesses, 904,984 of them line status reads, a third fewer. Traced, it took 118 s against 136 s.

The count of the FIFO's room is shared by every writer, and the console's callers do not serialize the port: a lock checker's report could interrupt a writer between its status read and its count, fill part of the FIFO, and leave the writer counting all 16 places free. `serial_out` now counts the room and writes each byte under a lock of its own with interrupts off, as Linux's console writer does under the port's lock (`serial8250_console_write`), and waits for a busy UART outside it. A wait that ends reads the status once more under the lock, since another writer may have filled the FIFO meanwhile. The serial host test's overlap row raises such a report after a writer's status read; against the code before the lock, ten of the writer's bytes reached a full FIFO.

What was left is what Linux makes too: a write for each byte sent, and a status read and a data read for each byte received. On that boot, with the change, the vCPU thread spent 22.7 s in the guest and 55.6 s out of it, 10.3 s in QEMU and 45.3 s in the host's kernel. It never blocked, since the guest always had a byte to send. 55.6 s over the boot's 3.9 million exits is 14 us an exit. Under TCG the case's five boots take 31 s together.

The helper already had a second way to move files, which the ext2 repair case uses (tools/etude/Readme.md): a 64 MiB IDE disk, `image-transfer`, on which the files travel as a tar archive, with only their digests on COM1. fatwrite's `volume` case now declares it. At the task's head the suite took 36 s under KVM against 38 s under TCG on i686, 36 s against 39 s on x86_64 and 37 s against 38 s on the ia32 profile.

## ext2 and metal

They pay the same cost in smaller amounts. In ext2, alone on i686, the probe cases took 2 to 3.6 times as long under KVM: mapmusl 18.6 s against 5.1 s, tmpfilemusl 14.9 against 4.8, inodechurnmusl 25.7 against 15.0. Each probe case reads its probe back out of the volume through the image helper's debugfs, and the file came back over COM1. The mapmusl boot made 572,512 accesses, 557,130 of them COM1's; with the serial change it made 384,817. The seven probe cases now declare the `image-transfer` disk. At the task's head ext2 took 118 s under KVM against 133 s under TCG on i686, 122 s against 138 s on x86_64, and 118 s against 135 s on the ia32 profile.

metal's machine made 105,000 accesses in its 209 s. Its three helper boots check the sticks and send what they find over COM1; they made 1,051,752, 932,234 and 1,057,294 accesses, COM1's and the AHCI polls below. Their output goes through etude-guest's command channel, not through the image adapter, so the transfer disk does not apply. At the task's head metal took 149 s under KVM against 145 s under TCG on i686, 157 s against 136 s on x86_64, and 114 s against 105 s on the ia32 profile.

## stress and stress-ahci: register accesses per disk command

stress writes 33 MiB through the page cache of a KRFFS root on IDE and reads it back. Its boot made 574,719 register accesses for 22,575 commands: 16,941 reads of 4 KiB, 3,213 writes and 2,421 cache flushes.

| Register | Before | After |
|---|---|---|
| status, 0x1F7, read | 180,278 | 153,109 |
| alternate status, 0x3F6, read | 100,001 | 25,001 |
| all | 574,719 | 473,554 |

A DMA command's wait read the status eight times before it slept, though the comment on `ATA_SPIN_BEFORE_SLEEP` says that a DMA command is never done within them, and the flush's wait did the same. Each pause after a select or a command read the alternate status four times. libata reads the status of a DMA or a non-data command only in its interrupt (`ata_bmdma_port_intr`, `__ata_sff_port_intr`, drivers/ata/libata-sff.c) and pauses with one read and `ndelay(400)` (`ata_sff_pause`). The owner's wait for a DMA command's or a flush's end now sleeps at once (`ata_wait_irq`). The pause is one read and `k_udelay(1)`, which spins on the TSC. A wait that cannot sleep polls as before, with its full bound.

About half of the status reads that are left come from the page cache, which cannot sleep for a read. `pc_copy` enters the cache with `pc_enter`, which turns interrupts off and takes the big lock, and `pc_bring` reads a missing page from the disk inside it. A record in the polled wait, written to port 0x402 and read back from the trace (not committed), showed the probe's reads in that state: pid 6, in a system call, interrupts off, by way of `filemap_read`, `vfs_read`, `krffs_read_at`, `krffs_window_read`, `pc_copy_out`, `pc_copy`, `pc_bring`, `krffs_disk_read`, `ata_read` and `ata_dma_transfer`. Such a wait polls the status until the DMA ends. The 16,942 reads of that boot found the drive busy 83,239 times, 3 or 4 times for most and up to 69. The writes, which `fsync` sends from a context that can sleep, found it busy once each. Gating the sleeper's reads to an interrupt or a tick was tried and dropped, since those reads were not the sleeper's: the boot made 160,019 status reads with it.

stress-ahci runs the same probe on an AHCI disk, whose writes and reads go through the polled path (`ahci_command_claimed`). Each step of that wait read PxCI and PxIS and waited 10 us. Its boot made 819,524 accesses for 21,789 commands, 307,000 of them PxCI's and 329,000 PxIS's. Linux's polled AHCI command reads PxCI alone (`ahci_exec_polled_cmd`, drivers/ata/libahci.c). The wait now reads PxIS at every 16th step, for a fatal error that leaves the slot set, and the boot made 585,491 accesses.

Alone, neither suite took longer under KVM than under TCG in the first set of runs: 13 s and 11 s against 13 s and 13 s on i686, and 20 s and 17 s against 20 s and 20 s on x86_64. In 5661's full run they took 38 s and 35 s, beside the install, account, drill, ext2 and exfat cases, which under KVM were exit-bound as fatwrite was. In the TCG full run of 5657 their neighbours were selfbuild, doom and save-update, which are not exit-bound. Beside ext2 and fatwrite, stress took 28 s under KVM against 20 s alone, and 18 to 19 s under TCG either way. At the task's head, stress took 11, 10 and 10 s under KVM against 10, 10 and 10 s under TCG on i686, x86_64 and the ia32 profile, and stress-ahci 8 s against 10 s on each.

## Times

Each suite's sum of case times in seconds, with the host's load average in parentheses: at the run's end, or at its start and its end. Every case passed in every run. metal's two host-side cases, which no accelerator runs, are left out; on the ia32 profile metal runs one case.

The old drivers against the new, at fade2fd53 and at the task's driver commits, with fade2fd53's manifests. Thirteen pairs that first ran under the load of other accounts' proofs ran again later; the table gives the later pair, and the first is in the last table. In the new x86_64 KVM fatwrite run the load rose from 2.5 to 15.0.

| Suite | Profile | Old, KVM | New, KVM | Old, TCG | New, TCG |
|---|---|---|---|---|---|
| stress | i686 | 12 (7.7 to 6.1) | 12 (6.1 to 4.6) | 20 (7.1) | 16 (6.5) |
| stress | x86_64 | 13 (5.4) | 12 (5.0) | 12 (3.6) | 14 (5.9) |
| stress | ia32 | 12 (2.0 to 1.8) | 12 (1.8 to 1.6) | 12 (1.8) | 12 (1.5) |
| stress-ahci | i686 | 11 (4.6 to 3.9) | 11 (3.7 to 3.2) | 18 (7.2) | 13 (6.4) |
| stress-ahci | x86_64 | 11 (5.2) | 10 (5.0) | 12 (3.3) | 13 (6.5) |
| stress-ahci | ia32 | 11 (1.5 to 1.6) | 10 (1.6 to 1.6) | 12 (1.5) | 12 (1.4) |
| fatwrite | i686 | 142 (3.0 to 2.4) | 122 (2.3 to 1.9) | 53 (6.5) | 53 (6.0) |
| fatwrite | x86_64 | 148 (5.3 to 2.5) | 132 (2.5 to 15.0) | 51 (3.4 to 1.8) | 51 (1.7 to 1.8) |
| fatwrite | ia32 | 152 (4.4) | 122 (1.7) | 51 (1.9) | 50 (1.7) |
| ext2 | i686 | 153 (1.8 to 2.5) | 142 (2.3 to 2.9) | 143 (7.3) | 155 (5.6) |
| ext2 | x86_64 | 155 (5.7 to 4.1) | 144 (3.9 to 3.4) | 137 (1.8 to 2.1) | 136 (1.9 to 1.7) |
| ext2 | ia32 | 158 (1.6 to 6.8) | 148 (7.0 to 6.3) | 139 (2.3) | 138 (2.5) |
| metal | i686 | 152 (2.8 to 1.8) | 150 (1.7 to 5.7) | 159 (6.5) | 156 (5.5) |
| metal | x86_64 | 175 (4.9) | 145 (3.9) | 138 (1.7 to 1.7) | 138 (1.6 to 2.1) |
| metal | ia32 | 127 (2.8) | 109 (1.1) | 110 (1.4) | 114 (4.9) |

The task's head, 85d9b25c8: the new drivers, the transfer disk and the milestone branch with 5672's flush, under KVM and then TCG.

| Suite | Profile | KVM | TCG |
|---|---|---|---|
| stress | i686 | 11 (1.8 to 3.6) | 10 (3.6 to 3.2) |
| stress | x86_64 | 10 (2.2 to 2.0) | 10 (2.0 to 2.1) |
| stress | ia32 | 10 (1.9 to 1.8) | 10 (1.8 to 1.6) |
| stress-ahci | i686 | 8 (3.2 to 3.0) | 10 (3.0 to 2.8) |
| stress-ahci | x86_64 | 8 (2.1 to 2.0) | 10 (2.0 to 1.8) |
| stress-ahci | ia32 | 8 (1.6 to 1.6) | 10 (1.6 to 1.8) |
| fatwrite | i686 | 36 (2.8 to 1.4) | 38 (1.4 to 1.2) |
| fatwrite | x86_64 | 36 (1.8 to 1.4) | 39 (1.4 to 5.9) |
| fatwrite | ia32 | 37 (1.8 to 13.9) | 38 (13.9 to 9.1) |
| ext2 | i686 | 118 (1.2 to 1.9) | 133 (1.9 to 1.8) |
| ext2 | x86_64 | 122 (5.9 to 9.6) | 138 (9.6 to 15.7) |
| ext2 | ia32 | 118 (3.7 to 3.1) | 135 (3.1 to 2.6) |
| metal | i686 | 149 (1.8 to 1.9) | 145 (1.9 to 2.2) |
| metal | x86_64 | 157 (3.7 to 1.3) | 136 (1.3 to 1.9) |
| metal | ia32 | 114 (2.6 to 1.3) | 105 (1.3 to 1.5) |

The runs kept apart, since a load above 8 makes them useless for comparison: the first runs of the thirteen pairs, the transfer disk's first runs at the task's commits before the merge, and two more runs of stress and stress-ahci on i686.

| Run | Suite | Profile | Accelerator | Time | Load at the end |
|---|---|---|---|---|---|
| old drivers | stress | i686 | KVM | 43 | 5.7 |
| new drivers | stress | i686 | KVM | 21 | 9.3 |
| old drivers | stress | ia32 | KVM | 20 | 9.9 |
| new drivers | stress | ia32 | KVM | 12 | 3.5 |
| old drivers | stress-ahci | i686 | KVM | 39 | 6.8 |
| new drivers | stress-ahci | i686 | KVM | 22 | 9.5 |
| old drivers | stress-ahci | ia32 | KVM | 18 | 8.1 |
| new drivers | stress-ahci | ia32 | KVM | 10 | 3.1 |
| old drivers | fatwrite | i686 | KVM | 184 | 15.1 |
| new drivers | fatwrite | i686 | KVM | 143 | 8.6 |
| old drivers | fatwrite | x86_64 | KVM | 172 | 11.8 |
| new drivers | fatwrite | x86_64 | KVM | 132 | 4.5 |
| old drivers | fatwrite | x86_64 | TCG | 52 | 4.0 |
| new drivers | fatwrite | x86_64 | TCG | 66 | 28.7 |
| old drivers | ext2 | i686 | KVM | 183 | 13.8 |
| new drivers | ext2 | i686 | KVM | 164 | 9.9 |
| old drivers | ext2 | x86_64 | KVM | 159 | 8.8 |
| new drivers | ext2 | x86_64 | KVM | 146 | 5.0 |
| old drivers | ext2 | x86_64 | TCG | 138 | 4.2 |
| new drivers | ext2 | x86_64 | TCG | 184 | 38.6 |
| old drivers | ext2 | ia32 | KVM | 160 | 10.7 |
| new drivers | ext2 | ia32 | KVM | 143 | 2.3 |
| old drivers | metal | i686 | KVM | 253 | 9.0 |
| new drivers | metal | i686 | KVM | 189 | 7.5 |
| old drivers | metal | x86_64 | TCG | 146 | 4.5 |
| new drivers | metal | x86_64 | TCG | 203 | 14.4 |
| transfer disk | fatwrite | i686 | KVM | 39 | 12.7 |
| transfer disk | fatwrite | i686 | TCG | 42 | 22.6 |
| transfer disk | fatwrite | x86_64 | KVM | 38 | 31.1 |
| transfer disk | fatwrite | x86_64 | TCG | 39 | 54.5 |
| transfer disk | fatwrite | ia32 | KVM | 37 | 65.1 |
| transfer disk | fatwrite | ia32 | TCG | 38 | 67.1 |
| transfer disk | ext2 | i686 | KVM | 120 | 7.0 |
| transfer disk | ext2 | i686 | TCG | 160 | 31.7 |
| transfer disk | ext2 | x86_64 | KVM | 120 | 42.9 |
| transfer disk | ext2 | x86_64 | TCG | 139 | 62.8 |
| transfer disk | ext2 | ia32 | KVM | 123 | 67.5 |
| transfer disk | ext2 | ia32 | TCG | 140 | 39.0 |
| new drivers, again | stress | i686 | KVM | 12 | 29.9 |
| new drivers, again | stress-ahci | i686 | KVM | 14 | 32.9 |
| new drivers, again | stress | i686 | TCG | 18 | 35.6 |
| new drivers, again | stress-ahci | i686 | TCG | 16 | 42.4 |
| new drivers, again | stress | i686 | KVM | 26 | 42.6 |
| new drivers, again | stress-ahci | i686 | KVM | 15 | 42.3 |
| new drivers, again | stress | i686 | TCG | 19 | 41.9 |
| new drivers, again | stress-ahci | i686 | TCG | 15 | 42.1 |

## What is left, and whose

The cost of each exit is the host's. A register access that leaves a guest of WSL2's nested KVM for QEMU costs some 14 us of the vCPU thread outside the guest. That is not a fault of QEMU's or KVM's code, so it has no entry in `upstream.md`, whose index leaves out a platform's limits; it is recorded here.

Rach2 could take three more steps, each beyond this task:

* The page cache reads a page from the disk with interrupts off and the big lock held (`pc_enter`, `pc_bring`), so the ATA driver polls each read to its end. On any machine the processor spins for the length of every page read, and under KVM each poll is an exit: some 83,000 of stress's accesses. Linux reads a page with no spinlock held and sleeps on the page's lock until the interrupt ends the read. A page read at a time is also slow on its own: stress read its 33 MiB back in 16,941 commands of 4 KiB, where Linux reads a file read in sequence in readahead windows of up to 128 KiB.
* The machine scenarios send their commands' output through COM1, at a register access a byte, where the guest suites use virtio-serial (`console=vport0p1`), whose transfers take an exit a buffer. metal's helper boots made a million accesses each.
* The AHCI driver polls every command, even one from a caller that could sleep: some 15 reads of PxCI a command, where Linux sleeps for the port's interrupt.
