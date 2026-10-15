# The kernel command line

This page lists the words the Rach2 kernel reads from its command line, which the loader passes: QEMU's `-append`, or the `multiboot` and `multiboot2` lines of a GRUB menu. [The kernel's Readme](Readme.md) describes what the boot does with the root and the first program.

The line is a list of words separated by spaces and tabs (`sys/cmdline.c`). A flag matches a whole word, so `rach2.rescue` does not match `xrach2.rescue`. A `key=value` option is read from its first occurrence; `rach2.ifaddr` may be given once for each interface, and a second `rach2.ip` or `rach2.gateway` makes the network refuse both. The kernel keeps the first 255 bytes of the line, and a longer line is cut at the last space or tab that fits, so that no word is kept in part. A word the kernel does not read is ignored, as `rach2.disk=ahci`, which `run.sh` adds with `DISKBUS=ahci`, is.

| Option | Effect |
| --- | --- |
| `rach2.rescue`, `init=rescue` | Start the debug shell in place of `/sbin/init` ([run.Readme.md](run.Readme.md)). |
| `rach2.root=ram` | Mount the first boot module in place: the default. |
| `rach2.fsdisk`, `rach2.root=disk` | Mount the KRFFS partition among the first four entries of the boot disk's table, through the page cache. |
| `rach2.root=ext2` | Mount the first ext2 partition among the first four entries of the boot disk's table, through the buffer cache. |
| `rach2.root=SPEC` | Mount the one partition, on any disk, that `SPEC` matches: `PARTUUID=`, `MBRID=`, `GPTID=`, `UUID=`, `LABEL=` or `/dev/diskNpM`, as [disk(7)](../man/man7/disk.7) defines them. |
| `rach2.rootfstype=TYPE` | With `rach2.root=SPEC`, mount it as `krffs` or `ext2`; another value is refused. Without it the kernel tries KRFFS, then ext2. |
| `rach2.fswindow` | Mount the boot module through the page cache's window, as a disk root is mounted. |
| `rach2.boot=SPEC` | Name the boot partition, which must be entry 1 of an MBR or a GUID table, by the forms of `rach2.root=SPEC`. Without it the boot disk is the root's, when the root is named by identity, and disk 0 otherwise. |
| `rach2.nodisk`, `rach2.nousb`, `rach2.nonet`, `rach2.noaudio`, `rach2.nomouse` | Skip the probe of the disks, the USB controllers, the network cards, the sound cards or the PS/2 mouse. |
| `rach2.ahcipoll`, `rach2.nvmepoll` | Keep the AHCI or the NVMe disks polled, with no interrupts and no request queue. |
| `rach2.atapio` | Move ATA transfers through the data port and not through the controller's bus master. |
| `rach2.diskdiag` | Probe the disks once the root is mounted, when the boot has not, and log whether one answered; the whole-disk nodes are published either way ([disk(7)](../man/man7/disk.7)). |
| `rach2.fsck` | Read by the recovery image's `init`, not by the kernel: run `e2fsck -p` on every ext2 partition first. The kernel reads it only to keep its records on the screen while that runs. |
| `rach2.ip=A.B.C.D/PREFIX`, `rach2.ip=none`, `rach2.gateway=A.B.C.D`, `rach2.ifaddr=MAC,ADDR/PREFIX[,GATEWAY]` | The interfaces' addresses ([the network Readme](sys/network/Readme.md)). Without them the first interface takes QEMU's user-network defaults. |
| `rach2.netconsole` | Broadcast the log in Ethernet frames once a card is up. |
| `rach2.loglevel=N` | Show records of level `N`, one digit, and more urgent on the screen, in place of `INFO` and more urgent during the boot and `CRIT` alone once init starts. |
| `rach2.diag` | Print the boot's diagnostic report again before the first program, and wait for a key. |
| `rach2.apicdiag` | Print what the firmware left in the local APIC and the MSI capabilities of the xHCI and HDA controllers. |
| `rach2.nomsi`, `noapic`, `maxcpus=N` | Take every interrupt through the 8259 pair; keep the 8259 pair and the 8254's tick where there is an I/O APIC; start at most `N` processors, the boot processor counted ([the PC platform](sys/arch/x86/Readme.md)). |
| `rach2.nofxsr`, `rach2.noxsave` | Save the floating-point state with `fnsave` (i686 only) or with `fxsave`, in place of the richer form the processor has ([the PC platform](sys/arch/x86/Readme.md)). |
| `rach2.board=NAME`, `rach2.nosci`, `rach2.resetskip=N` | Try only the named board's power profile, which the chipset must still match; leave its system control interrupt unmasked; skip the first `N` reset methods ([the PC platform](sys/arch/x86/Readme.md)). |
| `rach2.trace=enosys` | Log each system call number the kernel does not have, once a process. |
| `rach2.usblatency` | Print the times of keystrokes against the drain of USB events, and count the pumps and `getrandom` bytes before a blocked console reader runs. |

These options exist for the suites, which use them to bring about a fault or a boundary on demand:

| Option | Effect |
| --- | --- |
| `rach2.blocklag=N` | Answer disk 0's requests `N` ticks late, through a wrapper that sleeps. Its partition nodes, and `/dev/bootpart` when the boot partition is on disk 0, are on the wrapper; the root's window and every other disk keep their own devices. |
| `rach2.lagdeadline=T`, `rach2.lagperiods=N` | With `rach2.blocklag`: the wrapper's deadline period in ticks and the periods a request waits before it fails, in place of 500 ticks and one period. |
| `rach2.blockfail=N` | With `rach2.blocklag`: the `N`th write through the wrapper fails with `EIO`. |
| `rach2.blockdead=P` | With `rach2.blocklag`: MBR partition `P` is built over a second device that takes every request and answers none. |
| `rach2.blockpool=N` | Allow `N` block requests in flight, 1 to 16, in place of 16. |
| `rach2.blockdeadline=S` | Give a block request's deadline period as `S` seconds, 1 to 600. |
| `rach2.framefail` | Enable `/dev/framefail`, which takes a count: the count-th frame allocation inside a system call from then on fails ([bootlog(4)](../man/man4/bootlog.4)). |
| `rach2.framehigh` | Take frames from 4 GiB up first, so that on an x86-64 machine with memory there the heap's pages lie above 4 GiB. |
| `rach2.dma32` | Treat the AHCI and xHCI controllers as able to reach only 32-bit addresses. |
| `rach2.stackoverrun=fork`, `rach2.stackoverrun=boot` | Before the first program, run a kernel stack down until it overflows: one `kstack_alloc` gives, or the boot stack. The kernel panics at the page fault or the double fault that the guard page raises ([the architectures' Readme](sys/arch/Readme.md)). |
| `rach2.strings` | Before the first program, check `k_memcpy`, `k_memset` and `k_memmove` over 68 lengths at four source and four destination offsets, with the bytes around each range required unchanged. |
| `rach2.hubslow=MS` | Wait `MS` milliseconds between two ports of a hub scan. |
| `rach2.msidrop=N` | Silence every USB controller's interrupts at the `N`th, so that the timer's pump alone serves them. |
| `rach2.hcfail=N`, `rach2.nicfail=N` | Refuse the `N`th xHCI controller or network card, counting from 0. |
