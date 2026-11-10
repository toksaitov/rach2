# Device assignment

The assignment cases boot the GNU root on q35 with an emulated Intel VT-d unit. Its root has the GNU product layers, with no test inventories or staged personal logins, and is selected by the fixed MBR identity `82274701:1` so disk enumeration cannot select the payload. A virtio NIC goes to Debian through Rach2's VFIO, while Rach2 keeps its e1000, NVMe root, AHCI payload disk and virtio-gpu display. The EDU device forces DMA and interrupt faults through the same unit. Each case has its own root overlay and powers its nested guest off or kills and reaps it; etude stops the outer machine.

## Running

`ARCH=x86_64 tests/run assign` runs the cases in `tests/cases/machine.assign.json`. They run on Linux; `nic-kvm` needs the testing PC's nested SVM profile. `RACH2_CASES=machine.assign.boot` selects just the boot. The remaining cases are `nic-tcg`, `nic-kvm`, `dma`, `index`, `compat`, `kill` and `reassign`.

The machine has `intel-iommu,intremap=on,caching-mode=on`, a `virtio-net-pci,iommu_platform=on` at `0000:00:06.0` on the user network `10.0.3.0/24`, and EDU at `0000:00:07.0` with its DMA mask covering every address. The host's e1000 uses a separate user network, `10.0.2.0/24`. The two gateways end in `.2`. `iommu_platform=on` makes the NIC use the bus's DMA address space through VT-d.

`run.sh` runs as root in the GNU guest. It binds the two functions to vfio-pci and sets the locked-memory limit to unlimited. During each case it writes random data to the ext2 root, flushes and verifies it, and pings the host's gateway. It compares the live VFIO, KVM and free interrupt-vector counts before and after the operation. The kill case requires live pinned DMA pages before sending SIGKILL; the reassignment case then boots a second guest with the same function.

## Interfaces

`edu.c` uses Type1v2 VFIO and an eventfd. Its DMA control copies a sentinel into EDU and back, then unmaps the IOVA and requires a DMA fault without a change to the sentinel. Its interrupt control requires a delivered remapped MSI, then writes the real MSI configuration through x86's PCI configuration ports to send either an out-of-range remapping index or a compatibility-format message. Each must raise `iommu_irq_faults` without signalling the eventfd. The real MSI address is restored before VFIO releases the device.

The counters are the kernel's `/proc/stat` records, described in [vfio](../../rach2/sys/vfio/Readme.md#interfaces) and [VT-d](../../rach2/sys/arch/x86/iommu/Readme.md#use). The EDU register offsets and DMA commands follow [QEMU's EDU specification](https://www.qemu.org/docs/master/specs/edu.html). No elapsed time decides a result.

## Building

`make ARCH=x86_64 build/tests/cases/assign/assign.ext2` builds the payload. `mkguest.sh` uses the Debian kernel and BusyBox packages pinned by `tests/kvm/guests/debian.txt`. The initramfs includes the NIC modules and their dependencies in load order, leaving out dependencies Debian built into its kernel. The guest uses a static address, pings its gateway eight times, prints `ASSIGNGUEST OK` and powers off. With `assign.hold` it keeps pinging and prints `ASSIGNGUEST TRAFFIC` after each completed batch.

## Tests

The boot case requires the unit, both disk controllers and the display interrupt. The NIC cases require the Debian guest's answered pings. The fault cases require their positive DMA or MSI control before their blocked request. The kill and reassignment cases require every live resource count to return to its initial value.
