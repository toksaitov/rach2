# 08-disk

Path B: lifting KRFFS off the 32 MB RAM-disk module and onto a real block device
through a kernel page cache and a disk-backed kernel-virtual window. KRFFS keeps
pointer-walking one contiguous window; an absent page faults, the handler polls the
sectors in from an ATA disk into a cached frame and maps it, dirty pages write back,
and pages evict under pressure, so only the working set is resident. Then the system
boots from that disk through GRUB.

`plan.md` is the sequenced, multiphase plan: the two load-bearing decisions (the
fault polls rather than sleeps, because it runs with interrupts off; the window's
page tables are pre-mapped supervisor-only in the kernel directory before the first
fork, because clone copies PDE pointers by value), the milestone scope decisions
(polled PIO ATA not AHCI, write-back with a real fsync, dirty-flag plus offline fsck
not a journal, the 4 GiB cap stays, no VFS rewrite, the module path stays the
default), and eight phases from the host-tool offset work through the GRUB disk boot,
each independently buildable and gated by a concrete probe.
