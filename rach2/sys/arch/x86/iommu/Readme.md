# VT-d

The driver of Intel's VT-d remapping units, on x86-64: it reads the firmware's DMAR table, brings each unit up with its translation off, remaps the I/O APIC's interrupts and the devices' MSI and MSI-X messages, and reports the faults the units record. It gives a device to a domain of its own, an address space of second-level page tables, only when one is asked for, which is for a virtual machine; the host's own drivers keep physical DMA addresses. The interface is `sys/iommu.h`.

## Use

The boot brings the units up after ACPICA, in `platform_firmware_start` (`platform.c`), when the DMAR table names a unit of segment 0. The log says what it found, at `INFO` and, for the table itself and each interrupt remapping entry written, at `DEBUG`:

    vtd: dmar0: at 0xfed90000, version 1.0, cap 0x80d2008c222f0606, ecap 0xf00f4a
    vtd: irte: dmar0 index 2: msi of 0:1f.2, vector 40 to apic 0, edge, sid 0x00fa svt 1
    vtd: interrupt remapping: on, xapic format, compatibility format refused
    vtd: ready: 1 units, interrupt remapping on, translation on for assigned devices only

Words of the boot line change this, as Linux's do ([the command line](../../../../cmdline.md)):

* `intel_iommu=off`: no device is given to a domain (`iommu_present` is 0 and `iommu_attach` fails with `-ENODEV`); interrupt remapping stays on, as on Linux, where only `intremap=` turns it off. The word may be one of a list, as `intel_iommu=on,igfx_off`; the other values are accepted and change nothing, since translation is off for the host's devices anyway.
* `intremap=off`: interrupts are not remapped.
* `intremap=no_x2apic_optout`: the x2APIC format is taken although the DMAR table opts out of x2APIC mode.
* `nox2apic`: the entries keep the xAPIC format ([the PC platform](../Readme.md)).

With both words, as on Linux, no unit is programmed: each unit's version, capabilities and status are read and logged, translation is turned off only if the firmware left it on, and nothing else is written. The log says `vtd: off by intel_iommu=off intremap=off: N units read`.

A blocked DMA request or interrupt is logged with Linux's words for its reason, up to 32 for each unit, and every one recorded is counted in `/proc/stat` (`iommu_dma_faults`, `iommu_irq_faults`). The handler reads records while they stay full, as Linux's `dmar_fault`, so that a fault recorded while it runs is read in the same pass. A fault that finds every record full is lost; the unit says so only by its overflow bit, which counts as one DMA fault and is logged as `vtd: fault: dmarN: faults lost, every record full: K times, so F DMA faults is a floor`:

    vtd: fault: dmar0: dma write from 0:2.0 at 0x200000: reason 0x5, PTE Write access is not set
    vtd: fault: dmar0: interrupt from 0:1f.2, index 0x3: reason 0x26, Blocked an interrupt request due to source-id verification failure

## Interfaces

| Header | What it declares | Defined in |
| --- | --- | --- |
| `sys/iommu.h` | domains, mapping, attaching a device, reserved regions, the fault counts | `vtd.c` |
| `sys/pci.h` | `struct pci_msi_remap`, `pci_msi_remap_start` and `_stop`: every enabled MSI and MSI-X message rewritten for a remapper, and each one enabled later composed through it | `sys/pci.c` |
| `sys/arch/x86/ioapic.h` | `struct ioapic_remap`, `ioapic_remap_start` and `_stop`: the same for each routed redirection entry | `ioapic.c` |
| `sys/acpi/acpica.h` | `acpica_table`, a firmware table by its signature | `sys/acpi/boot.c` |
| `vtd.h` | the driver's own: the registers, the table's structures, the units, and `hw.c`'s calls into the machine | the files here |

A domain is made with `iommu_domain_alloc`, filled with `iommu_map` and `iommu_unmap`, and given a device with `iommu_attach`, which fails with `-EPERM` for a device a non-relaxable reserved region names, `-EBUSY` for a device another domain has, and `-EINVAL` for a domain the device's unit cannot walk. `iommu_reserved_regions` gives a device's regions as Linux's `intel_iommu_get_resv_regions` does: each reserved region its scope names, `IOMMU_RESV_DIRECT_RELAXABLE` for a USB controller's or a display's and `IOMMU_RESV_DIRECT` for the rest; the first 16 MiB for an ISA bridge, for a floppy controller's DMA; and the MSI window, 0xFEE00000 to 0xFEEFFFFF.

## Design

### Sources

`sources.mk` sets `IOMMU_C_SRC`, which `x86_64/sources.mk` alone includes: `dmar.c` reads the table, `vtd.c` has the units, the domains and invalidation, `intremap.c` interrupt remapping, `fault.c` the fault reports, and `hw.c` the calls into the machine (registers, pages, the bus, the boot line, the fault vector), which a host test replaces with a model.

### The table

`dmar_parse` checks the signature, the length and the checksum, and reads the units (DRHD) and the reserved regions (RMRR); the other structures are passed over. A device scope's path is walked through the bridges as the firmware numbered them, so a scope through a bridge that is absent names nothing, as Linux leaves it. A device belongs to the unit whose scope names it, a bridge's scope covering its buses, or else to the unit with INCLUDE_PCI_ALL. QEMU's intel-iommu lists each device by its slot and has no INCLUDE_PCI_ALL unit; the Lenovo's graphics unit lists the GPU alone and its other unit has the flag. The I/O APIC's requester id is its scope's.

### A unit's bring-up

`unit_start` reads the capabilities, takes four levels of tables where the unit has them and three otherwise, and turns off what the firmware left on: translation, interrupt remapping, the invalidation queue once it is empty, and the protected memory regions. It sets an empty root table and, where the unit has one (ECAP.QI), the invalidation queue: one page of 256 descriptors, each followed by a wait descriptor whose status write the driver polls. A descriptor the unit refuses (FSTS.IQE) is logged, overwritten by the wait and the queue goes on, as Linux's `qi_check_fault` does. A unit without the queue is invalidated through its registers. The global command register is written from the persistent bits the driver keeps, never from the status read back, which holds the one-shot bits too.

### Domains and translation

A domain's tables have four levels when a unit walks four; a unit of three walks it from the top table's first entry, which every domain makes at once, and a domain given to it is held to its 39 bits. Domain id 1 is the host's, and the domains of `iommu_domain_alloc` take ids from 2, each unit's own, at most 256.

Translation is per unit, so a unit's translation stays off until one of its devices is given to a domain. The first one given makes every function of every bus present in the unit's scope a context entry of the host's: pass-through, or, on a unit without it (ECAP.PT clear, as the Lenovo's graphics unit), an entry of an identity domain that maps every address below the end of RAM, and at least the first 4 GiB, to itself, in 2 MiB pages where the unit has them, and each reserved region beside. The identity domain is built before the lock is taken: on the Lenovo's graphics unit, without large pages, it is a million entries. Translation is turned on after that and off again when the last device is given back, each device returning to the host's entry.

The units obey their capabilities as Linux does:

* A unit whose walks are not coherent with the processor's caches (ECAP.C clear, as both of the Lenovo's are) has each table entry written back from the cache after it is written (`arch_dma_flush`), and a domain's tables written back whole when such a unit first takes it.
* A unit without snoop control (ECAP.SC clear, as the graphics unit) gets no snoop bit in its domains' leaves. The first unit to take an empty domain decides whether its leaves snoop, and a unit without snoop control refuses a domain whose leaves do.
* In caching mode (CAP.CM, as QEMU's with `caching-mode=on`), where a unit may hold an entry that was not present, a context entry or a page made present is invalidated; otherwise the write buffer is flushed where the unit asks for it (CAP.RWBF).
* A present context entry is cleared and its caches invalidated before a new one is written, the high half first.

A device that a reserved region names is refused (`-EPERM`) unless it is a USB controller or a display, the classes Linux's `device_rmrr_is_relaxable` lets go to a domain without the region (`drivers/iommu/intel/iommu.c`, line 2051 of Debian's 6.12.113). That is how the Lenovo's GPU, whose region holds its GTT and stolen memory, can go to a VM, and why its USB controllers can.

### Interrupt remapping

`intremap_start` turns remapping on when the table offers it, every unit has it and the queue, and every I/O APIC the MADT lists is in a unit's scope. Each unit gets a table of 256 entries in one page. The units at first let compatibility-format interrupts by (GCMD.CFI); `ioapic_remap_start` and `pci_msi_remap_start` then give every routed redirection entry and every enabled message an entry of its own and rewrite it in the remappable format, and then the compatibility format is refused, as Linux has it. A message is remapped with no subhandle, so that its data is not read and the one write of its address moves it from one format to the other; a redirection entry moves with the one write of its high half. A message enabled afterwards is composed through the remapper, and one disabled has its enable bit cleared before it is written back and its entry given back. `intremap_stop` reverses the steps.

An entry delivers the vector, at fixed delivery and edge or level as the source had them, to the boot processor's APIC id, and takes the interrupt only from the source's requester id. The id is in the xAPIC format, eight bits, or in the x2APIC format, 32 bits with IRTA.EIME set, which Linux's `intel_prepare_irq_remapping` chooses (`irq_remapping.c`, lines 738 to 754) when the local APIC may run in x2APIC mode, the DMAR table does not opt out of it and every unit has the format (ECAP.EIM). With the x2APIC format, `intremap_start` moves the boot processor's local APIC to x2APIC mode before it writes the first entry, as Linux's `enable_IR_x2apic` enables x2APIC mode once remapping is on, since an x2APIC on bare metal is not supported without it; while the units still let compatibility-format interrupts by, those reach the processor by its 8-bit id. A start that fails after the move moves the APIC back to xAPIC mode. The Lenovo's two units both have the format and its table does not opt out, so it runs in x2APIC mode, as Linux does there; QEMU's unit has it only with `eim=on`. For a device behind bridges that id is Linux's `set_msi_sid` (`drivers/iommu/intel/irq_remapping.c`, line 373): a conventional PCI bridge, or a PCI-to-PCI Express bridge, puts its own id on what passes it and is matched whole; a PCI Express-to-PCI bridge puts its secondary bus with devfn 0, and the entry checks the range of buses from there to the device's.

QEMU's intel-iommu does not reflect GCMD.CFI in its status register, so the driver waits a millisecond for that bit and no longer, without a record.

### Fault reports

Each unit's fault event is a message on one vector of the boot processor, whose handler reads every unit's fault recording registers from the one the status register gives until one is empty, clearing each and dropping the unit's lock to log it, as Linux's `dmar_fault` does. A fault lost while every record was full (FSTS.PFO) is counted and logged once each time. The Lenovo's units have one record each, which a device that keeps faulting refills as fast as the handler reads it; so a pass reads at most 32 records, unlike `dmar_fault`, and leaves the rest to the next tick (`iommu_tick`), since a record left full keeps PPF set and the unit raises no new event for it.

### Why Debian translates DMA on the Lenovo without `intel_iommu=on`

Debian 13's kernel, 6.12, sets `CONFIG_INTEL_IOMMU_DEFAULT_ON_INTGPU_OFF=y`. Debian's patch `debian/patches/features/x86/intel-iommu-add-kconfig-option-to-exclude-igpu-by-default.patch` (Debian bug 935270) replaces upstream's `CONFIG_INTEL_IOMMU_DEFAULT_ON` with a choice of three, "Enable", "Enable, excluding integrated GPU" and "Disable", so that leaving `CONFIG_INTEL_IOMMU_DEFAULT_ON` unset no longer means off. In Debian's `drivers/iommu/intel/iommu.c` of 6.12.113:

* Line 208, `int dmar_disabled = IS_ENABLED(CONFIG_INTEL_IOMMU_DEFAULT_OFF);`: `CONFIG_INTEL_IOMMU_DEFAULT_OFF` is not set, so `dmar_disabled` is 0 and translation is on by default. `intel_iommu=on` only clears what is already clear.
* Line 214, `static int dmar_map_intgpu = IS_ENABLED(CONFIG_INTEL_IOMMU_DEFAULT_ON);`: 0, so lines 2385 and 2386 add `IDENTMAP_INTGPU` to `iommu_identity_mapping`, to give the integrated GPU an identity domain and leave it untranslated.
* That identity domain comes from `device_def_domain_type`, whose test at line 2083 is reached only past lines 2074 and 2075, `if (!ecap_pass_through(iommu->ecap)) return IOMMU_DOMAIN_DMA;`. The Lenovo's graphics unit, dmar0, has ecap 0xf0101a, whose bit 6, pass-through, is clear, so the GPU gets a translated DMA domain too.

So every device of the Lenovo is translated, the GPU in a group of its own with its reserved region, and `intel_iommu=off` sets `dmar_disabled` and removes the groups, as measured.

## Limits

* Units of segment 0 alone, as configuration space here is segment 0's; another segment's unit is logged and passed over.
* Legacy-mode tables alone: no scalable mode, no first-level tables, no PASIDs, no device TLBs (ATS) and no page requests.
* A device's context entry is its own requester id; a device behind a PCIe-to-PCI bridge, whose DMA carries the bridge's id, keeps the host's entry under that id when it is given to a domain: devices that share a requester id are not yet grouped.
* Interrupt remapping needs the invalidation queue on every unit. Every remapped interrupt goes to the boot processor, whose id must be below 256 to be named by the compatibility format the entry is made from. The HPET's scope is read but its interrupts are not remapped, since the HPET raises none here.
* A table of 256 remapping entries for each unit; a message or a line past that is refused, and keeps the format it had.
* USB controllers' reserved regions are relaxable, as Linux has them, beside the display's.
* A domain mapped before it is first given to a unit with snoop control has no snoop bit in its leaves.

## Tests

`tests/host/vtd_hosttest.c` ([host tests](../../../../../tests/host/Readme.md)) includes `dmar.c`, `vtd.c`, `intremap.c` and `fault.c` and links `sys/pci.c` and `pciconf.c` over a configuration space in memory. Its model of a unit has the register file, the invalidation queue, and context, IOTLB and interrupt-entry caches that only an invalidation empties and that in caching mode hold what was not present. A unit whose walks are not coherent reads a copy of memory that only a write-back updates and that starts as stale bytes, so an entry not written back reads as garbage. The model counts each command the specification forbids: two changes in one write of the command register, translation on before the root table's invalidation, remapping on before its table's, the queue turned off with work in it, an entry cleared while a source still names it, and remapping on with the compatibility format refused while a source still sends in it. Each row ends with every page given back and no such command. The rows:

* QEMU's table, captured from QEMU 10.2.1 (`tests/host/acpi_qemu_tables.h`), and the Lenovo's, rebuilt from Linux's boot log to its 176 bytes; malformed tables, and scopes through absent and present bridges.
* On QEMU's unit: the I/O APIC's lines, AHCI's MSI and NVMe's MSI-X remapped at the start and delivered; a message from the wrong requester and one in the compatibility format refused and reported; messages enabled, enabled again and disabled while remapping is on, an entry the unit held as not present taken again; and remapping stopped, each source back in the compatibility format.
* On the Lenovo's two units: the GPU's message through dmar0, the others through dmar1, the I/O APIC's from its own requester id alone, every entry read through the stale copy of memory and logged.
* With a processor that offers x2APIC mode: on the Lenovo's units the local APIC moved once and every message delivered to an id read from bits 63:32; a table that opts out, and QEMU's unit without the format, kept in the xAPIC format with the APIC where it was; and a start whose second table cannot be had moving the APIC back.
* The requester ids behind a root port, a PCI bridge, a PCIe-to-PCI bridge and a PCI-to-PCIe bridge.
* The Lenovo's reserved regions, as Linux reports them.
* The GPU given to a domain: dmar0 translating through tables written back, without the snoop bit, its identity domain of a million 4 KiB pages, a fault past the mapping, a page mapped for reading alone, an unmap the IOTLB forgets, and the GPU physical again once given back.
* The AR9565 given to a domain on dmar1: the leaves snoop, the unit's other devices pass through, the domain is refused by dmar0, and xHCI, whose region is relaxable, joins it.
* A coherent unit without pass-through: no write-back at all, an identity domain of 2 MiB pages to the end of RAM and over a reserved region past it, and AHCI, whose region is not relaxable, refused.
* Caching mode: a page made present is seen after the unit cached its absence.
* A unit of three levels and one of four walking one domain, and a domain mapped past the smaller one's reach refused there.
* The domain calls' refusals, an overlap that leaves nothing of itself, and a page for a table that could not be had.
* A refused invalidation descriptor reported and the queue going on.
* `intel_iommu=off` and `intremap=off`, and both: the Lenovo's units read and no register written, or one command where the firmware left translation on.
* 42 faults: one lost while the record was full, 32 logged and the rest counted.
* A device that refills the one record as fast as it is read: a pass reads 32 records and leaves the full one, and the next tick reads the other 9.
* A full remapping table: the message refused and not enabled.

The boot checks were run by hand in QEMU 10.2.1 with `-machine q35 -device intel-iommu,intremap=on` and `rach2.rescue`, under the x86-64 kernel: with an AHCI disk, an NVMe disk and an e1000, every message and line was remapped; disk reads through the request queue raised the AHCI controller's MSI and the NVMe controller's MSI-X once each, pings over the e1000 were answered on its I/O APIC line, and no unit recorded a fault.
