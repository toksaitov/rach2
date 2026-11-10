# `vfio/`: PCI devices given to programs

The `vfio/` directory is the kernel's side of Linux's VFIO API on the x86-64 kernel: the groups of PCI functions the IOMMU tells apart, `/dev/vfio/vfio` and its containers with Linux's Type1 IOMMU driver, and the vfio-pci driver, which gives a program a function's regions, interrupts and reset through a descriptor. A program such as QEMU uses it as it uses Linux's, through `linux/vfio.h`, after binding the function to vfio-pci through `/sys`. The IOMMU is VT-d ([iommu](../arch/x86/iommu/Readme.md)); without a unit that translates there are no groups. [vfio(4)](../../../man/man4/vfio.4) is the user's contract.

## Use

```
echo vfio-pci > /sys/bus/pci/devices/0000:00:02.0/driver_override
echo 0000:00:02.0 > /sys/bus/pci/drivers/vfio-pci/bind
readlink /sys/bus/pci/devices/0000:00:02.0/iommu_group
```

The function must have no driver of the kernel's: bind is `EBUSY` while one holds it. Its group's node, `/dev/vfio/N`, can then be put in a container, and the function opened by name with `VFIO_GROUP_GET_DEVICE_FD`.

## Interfaces

* `/dev/vfio/vfio`, character device 10:196, mode 0666, and `/dev/vfio/N`, 242:N, mode 0600, one for each group, through the devfs registry and the `vfio` row of /dev's directories (`fs/synthfs.c`). The container's node is there with no IOMMU, as on Linux.
* `/sys/kernel/iommu_groups/N/` with `type` (`identity`), `reserved_regions` (each line the start, the end and `direct`, `direct-relaxable` or `msi`, sorted) and `devices/`, a link to each function; each function's `iommu_group` link and `driver_override`, which shows `(null)` unset; and `/sys/bus/pci/drivers/vfio-pci/` with `bind`, `unbind`, `new_id` and `remove_id`, and `/sys/bus/pci/drivers_probe`, through the registry of [sysfs](../fs/sysfs.h). A bound function has a `driver` link.
* The ioctls of a container, a group and a device, numbered and laid out as Linux's (`vfio_abi.h`): the container's `VFIO_GET_API_VERSION`, `VFIO_CHECK_EXTENSION`, `VFIO_SET_IOMMU`, `VFIO_IOMMU_GET_INFO`, `VFIO_IOMMU_MAP_DMA` and `VFIO_IOMMU_UNMAP_DMA`; the group's `VFIO_GROUP_GET_STATUS`, `VFIO_GROUP_SET_CONTAINER`, `VFIO_GROUP_UNSET_CONTAINER` and `VFIO_GROUP_GET_DEVICE_FD`; and the device's `VFIO_DEVICE_GET_INFO`, `VFIO_DEVICE_GET_REGION_INFO`, `VFIO_DEVICE_GET_IRQ_INFO`, `VFIO_DEVICE_SET_IRQS` and `VFIO_DEVICE_RESET`. An i386 program's ioctls pass to them unchanged, since every structure has one layout in both ABIs ([ia32](../../ia32.md)).
* A device's regions at index << 40 of its descriptor, read and written with `pread` and `pwrite`: the six BARs, the ROM, the configuration space, and for an Intel display the OpRegion (index 9) and the host and LPC bridges' configuration spaces (10 and 11), each read-only with a type capability. A memory BAR of a page or more is mapped with `mmap` at its region's offset, uncached.
* `vfio_start` (`kernel.c`) forms the groups and adds the nodes and entries at boot, after the PCI bus, the IOMMU and the display. It logs `vfio: ready: G groups of F functions`.
* `/proc/stat` counts attached groups (`vfio_groups`), referenced devices (`vfio_devices`), DMA mappings (`vfio_dma_mappings`) and their live pinned pages (`vfio_dma_pages`), under the big lock. A container shared by groups is counted once. `msi_vectors_free` counts the free vectors of the host's message pool.
* The boot option `vfio_iommu_type1.allow_unsafe_interrupts=1` ([cmdline](../../cmdline.md)).

## Sources and license

Rach2's own. The structures and numbers of `vfio_abi.h` follow Linux's `include/uapi/linux/vfio.h`, whose layout is the ABI; the group rules follow Linux's `pci_device_group` and `pci_acs_enabled`, and the configuration space, the interrupts and the IGD's regions follow `drivers/vfio/pci/`.

| File | What it holds |
| --- | --- |
| `vfio_abi.h` | the API's structures, ioctl numbers and flags |
| `vfio.h` | the records and each file's calls |
| `group.c` | the groups, their nodes and entries of `/sys`, binding, `vfio_start` |
| `container.c` | the container and the Type1 driver |
| `pci.c` | the device's descriptor: regions, mappings, information, reset, release |
| `config.c` | the configuration space as the owner sees it |
| `irq.c` | the line, MSI, MSI-X and the error and request eventfds |
| `igd.c` | the OpRegion and the bridges' regions of an Intel display |
| `hw.c` | what VFIO takes from the rest of the kernel, which a host test replaces |

## Design

### Groups

A group is the functions the IOMMU cannot tell apart, formed as Linux's `pci_device_group` forms them: a function joins the group of the first bridge above it whose path to the root does not isolate its peers by ACS (source validation, request and completion redirect and upstream forwarding, as far as the bridge offers them), and a function of a slot with several that has no ACS joins the lowest such function of the slot. A conventional or a PCI Express to PCI bridge isolates nothing. The Intel PCH root ports of Linux's `pci_quirk_intel_pch_acs`, the Lenovo's Lynx Point among them, are taken as isolating. The groups are numbered as their first function is met, unit by unit and each unit's functions in bus order, so the HD 4400, alone on its unit, is group 0 on the Lenovo. A function no unit has is in no group.

A group is viable, and goes into a container, while none of its functions but a bridge has a driver of the kernel's. A driver's claim is the owner `pci.c` records when the driver first turns on decoding, bus mastering, the line or messages; vfio-pci claims its functions by name. A device's descriptor holds its group, and the group its container; the last group out of a container drops its mappings and its domain, as Linux's.

### Type1

`VFIO_SET_IOMMU` makes one IOMMU domain for the container and gives it every function of every group in it. `VFIO_IOMMU_MAP_DMA` pins the caller's pages (`mm/pin.h`), as Linux pins them, and maps each run of physically contiguous pages at once. A mapping must fall within one valid IOVA range: the domain's addresses less each function's reserved regions but the relaxable ones, as `GET_INFO`'s capability reports them. An unmap of the pinned memory, or the end of the address space, takes the pages back; the mapping's holder then unmaps the IOVA, so the device no longer reaches them, and the mapping goes at its UNMAP_DMA or with the container. Version 2 refuses an unmap that would split a mapping; version 1 removes each mapping the range touches.

### The device

The first descriptor of a function makes it ready, as Linux's `vfio_pci_core_enable`: a reset if the function has one, its state kept, its BARs, ROM, capabilities and MSI-X table read, and an Intel display's OpRegion mapped. A memory BAR is read and written through a window mapped for the access; the MSI-X table's bytes read as all ones and take no write, since VFIO writes the table. A BAR's mapping is a device mapping (`vm_device_map`) that holds the device, and its pages fault in only while memory decoding is on and no reset runs; turning decoding off or a reset takes them from every mapping.

The configuration space is the device's but for what VFIO keeps, as Linux's `vfio_pci_config.c`: the BARs and the ROM as the owner wrote them, cut to their sizes; the cache line, latency and interrupt line; the Interrupt Disable bit, whose real bit is on while the owner sets it, while the line is masked and while the line is not the device's interrupt; the MSI capability whole; and the MSI-X enable and mask bits. The Initiate Function Level Reset and the AF capability's FLR bits are taken out of writes, ASLS takes none, and a capability VFIO does not know reads through and takes no write. Bytes outside the header and every capability go through. The extended space reads through and takes no write.

### Interrupts

The line's handler is on the device's platform line from the first time it is used and stays there, passing over a device that is not VFIO's now. At each interrupt it masks the asserting device by Interrupt Disable and signals its eventfd; an unmask, by ioctl or by the unmask eventfd, lifts the mask unless the device still asserts the line. Each message and each MSI-X entry given an eventfd takes a vector of its own, whose handler signals the eventfd; with interrupt remapping each goes through a remapping entry of its own (`pci_msix_entry_set`). MSI-X entries are set and cleared while MSI-X is on, as Linux's dynamic MSI-X lets them be. A reset loses the messages, which VFIO programs again after it.

### Release

The last descriptor and mapping of a function release it, and the owner's death closes both: every interrupt off and its vectors and eventfds given back, bus mastering off, a reset when one worked at the open, and the state kept then written back. The identity and the header are read back and a difference is logged. The function stays bound to vfio-pci.

## Limits

* MSI of more than one message is answered with 1, the count Linux answers when it allocates fewer, since a remapped MSI is one vector; QEMU then asks for one. MSI-X has every entry.
* No VGA region, as Linux built without `CONFIG_VFIO_PCI_VGA`, no hot reset, which is `ENODEV` as on a root bus, no `VFIO_DEVICE_IOEVENTFD` and no `VFIO_DEVICE_FEATURE`, both `ENOTTY`, so no migration and no dirty page tracking. The error and request eventfds are kept and never signalled, since the kernel has no AER and asks for no device back.
* The line is offered only for a function that masks it by Interrupt Disable; Linux can mask another at the interrupt controller, given the line to itself.
* At most 64 groups, 128 functions in groups, 65535 mappings in a container, as Linux's `dma_entry_limit`, and 16 ids for `new_id`. A device name longer than 31 bytes is `ENODEV`.
* The device's descriptor answers `lseek` from the start or the position, where Linux's is not seekable.
* The extended configuration space takes no write.

## Tests

* `tests/host/vfio_hosttest.c` links the directory's files but `hw.c` over a model of the Lenovo's bus in small and fakes of the IOMMU, pins, eventfds, vectors, `/sys` and `/dev`: the groups, the PCH quirk among them; the nodes and entries; binding; the group and container ioctls and Type1's checks, mappings and capabilities; the regions and their information; the configuration space; the BAR mappings; the line, MSI and MSI-X; and the reset at the open, at `VFIO_DEVICE_RESET` and at the release, with every vector and eventfd given back.
* `tests/host/synthfs_hosttest.c` classifies `/dev/vfio`, its nodes and VFIO's entries of `/sys`.
* `tests/musl/vfiomusl.c`, a native case on the Lenovo: the container's ioctls, each group's node, `type`, `reserved_regions` and devices' links and `driver_override`, and vfio-pci's files.
