# Notes for 79-assign.md

Drafter's working notes. The output is `79-assign.md` beside this file.

## Progress

* Read: brief.md, plan.md (whole), dev-log/70-wifi-chip/plan.md (the form).
* Next: round3 research on VT-d and VFIO, then the kernel code (pci, isr, acpi, mm).

## Facts gathered

(filled as read; each with its path:line)

### From the research (round3, comparison, Argon) and the Lenovo's Linux log
* DMAR (dmesg.txt:190-202): HAW 39; dmar0 at 0xfed90000 flags 0 cap c0000020660462 ecap f0101a (graphics, 00:02.0); dmar1 at 0xfed91000 flags 1 (INCLUDE_PCI_ALL) cap d2008020660462 ecap f010da; RMRR 0xaae11000-0xaae30fff and 0xab800000-0xaf9fffff; IOAPIC id 2 and HPET 0 under dmar1; Linux enabled IR in x2apic mode with queued invalidation.
* ecap bit 0 (C, coherent walks) clear on both: clflush after every table write. ecap bit 7 (SC) clear on dmar0: leave PTE SNP bit (11) clear there.
* IGD 8086:0a16 rev 0b at 00:02.0 (info.txt): BAR0 4 MiB at 0xc0000000, BAR2 256 MiB prefetchable at 0xb0000000, I/O 0x4000 size 64, Expansion ROM at 000c0000 [virtual] [disabled] 128K; MSI at cap 0x90 (1 vector, 32-bit); PM at 0xd0; AF at 0xa4 with FLR+. Host bridge 8086:0a04 (00:00.0); LPC 8086:9c43 (fable.md:401).
* Linux reserves Intel graphics memory at 0xaba00000-0xaf9fffff (dmesg.txt:125); BDSM 0xaba00000 inside the second RMRR. GMCH_CTRL 0x50: 64 MiB stolen, 2 MiB GTT (Argon item 6). ACPI video: rom: no, post: no (dmesg.txt:686). UEFI boot (INSYDE EFI 2.3.1).
* Argon item 8: MSI pool is vectors 48..63 (isr.c:21-22), 16 entries; plan says isr_free_vector exists (isr.c:502). xHCI takes 8 (info.txt: IRQ 44-48 on Linux). Widen the pool and free on SET_IRQS teardown.
* Argon item 5: VFIO region offsets index<<40 (VFIO_PCI_OFFSET_SHIFT 40); Rach2 mmap pgoff is 32-bit (vm.h:61, syscall_proc.c:6379-6386). Plan: Linux's offsets fit Rach2's mmap "(the region's index shifted by 40 bits)"... need to check: pgoff = offset>>12 = index<<28, fits in 32 bits for index < 16. Region 8+ (IGD quirk regions) index<<28 for index up to 15 fits. OK.
* Codex rebuttal :123: each RMRR mapped only for the devices its scope names; the Linux check records the association.
* Fable :401: QEMU legacy mode needs a ROM; romfile from the firmware image is the fallback; hw/vfio/igd.c maps 0x0a00 prefix to gen 7. Linux vfio_pci_igd.c needs OpRegion at config 0xfc and host bridge and LPC. vfio.h:351-353 the IGD region subtypes.
* Fable :117,:149: in the VM, vfio-pci noiommu for the bed's backend (80's); CONFIG_VFIO_NOIOMMU in Debian's kernel unverified.
* Plan 79-1 waits on 77-2's route; 79-5 waits on 79-3, 75-10, 79-1; 79-4 on 79-3, 78-6; 79-3 on 79-2, 73-2.
