# Device assignment

The assignment cases boot the GNU root on q35 with an emulated Intel VT-d unit. Its root has the GNU product layers, with no test inventories or staged personal logins, and is selected by the fixed MBR identity `82274701,1` so disk enumeration cannot select the payload. A virtio NIC goes to Debian through Rach2's VFIO, while Rach2 keeps its e1000, NVMe root, AHCI payload disk and virtio-gpu display. The EDU device forces DMA and interrupt faults through the same unit. Each case has its own root overlay and powers its nested guest off or kills and reaps it; etude stops the outer machine.

## Running

`ARCH=x86_64 tests/run assign` runs the cases in `tests/cases/machine.assign.json`. They run on Linux; `nic-kvm` needs the testing PC's nested SVM profile. `RACH2_CASES=machine.assign.boot` selects just the boot. The remaining cases are `nic-tcg`, `nic-kvm`, `dma`, `index`, `compat`, `kill` and `reassign`.

The machine has `intel-iommu,intremap=on,caching-mode=on`, a `virtio-net-pci,disable-legacy=on,iommu_platform=on` at `0000:01:00.0` behind the PCIe root port at `0000:00:06.0` on the user network `10.0.3.0/24`, and EDU at `0000:00:07.0` with its DMA mask covering every address. The host's e1000 uses a separate user network, `10.0.2.0/24`. The two gateways end in `.2`. `iommu_platform=on` makes the NIC use the bus's DMA address space through VT-d. QEMU gives virtio its PCIe function-level reset capability only behind a root port, as `virtio_pci_realize` in [QEMU 10.2.1](https://github.com/qemu/qemu/blob/v10.2.1/hw/virtio/virtio-pci.c) defines it.

`run.sh` runs as root in the GNU guest. It binds the two functions to vfio-pci and sets the locked-memory limit to unlimited. During each case it writes random data to the ext2 root, flushes and verifies it, and pings the host's gateway. It compares the live VFIO, KVM and free interrupt-vector counts before and after the operation. The kill case requires live pinned DMA pages before sending SIGKILL; the reassignment case then boots a second guest with the same function. The first guest's console is printed as it arrives and kept beside QEMU's diagnostics in the case's scratch directory, which remains after a failure or an interrupted run.

## Interfaces

`edu.c` uses Type1v2 VFIO and an eventfd. Its DMA control copies a sentinel into EDU and back, then unmaps the IOVA and requires a DMA fault without a change to the sentinel. Its interrupt control requires a delivered remapped MSI, then writes the real MSI configuration through the privileged `/sys/bus/pci/devices/BDF/config` file to send either an out-of-range remapping index or a compatibility-format message. Each must raise `iommu_irq_faults` without signalling the eventfd. The real MSI address is restored before VFIO releases the device.

The counters are the kernel's `/proc/stat` records, described in [vfio](../../rach2/sys/vfio/Readme.md#interfaces) and [VT-d](../../rach2/sys/arch/x86/iommu/Readme.md#use). The EDU register offsets and DMA commands follow [QEMU's EDU specification](https://www.qemu.org/docs/master/specs/edu.html). No elapsed time decides a result.

## Local changes

The outer QEMU needs `qemu-vtd-cfi.patch`. QEMU 10.2.1 does not acknowledge GCMD.CFI in GSTS.CFIS and forwards compatibility-format MSIs even when CFI is clear. The patch adds the status update and fault reason 0x25, preserving delivery when CFI is set or interrupt remapping is off. The patch changes the test host's emulator, not QEMU in the GNU guest.

## Building

`make ARCH=x86_64 build/tests/cases/assign/assign.ext2` builds the payload. `mkguest.sh` uses the Debian kernel and BusyBox packages pinned by `tests/kvm/guests/debian.txt`. The initramfs includes the NIC modules and their dependencies in load order, leaving out dependencies Debian built into its kernel. The guest uses a static address, pings its gateway eight times, prints `ASSIGNGUEST OK` and powers off. With `assign.hold` it keeps pinging and prints `ASSIGNGUEST TRAFFIC` after each completed batch.

### The outer emulator

Use the official [QEMU 10.2.1 archive](https://download.qemu.org/qemu-10.2.1.tar.xz), SHA-256 `a3717477d8e2c84d630bfffbc20f6cd3293eb45aa1e6dac6d0cc27689991c9e1`. Keep it and its extracted source under `build/cache/qemu`, apply `patch -p1 -d build/cache/qemu/qemu-10.2.1 < tests/assign/qemu-vtd-cfi.patch` from the repository root, and configure in a separate `build/cache/qemu/out` directory. Through the testing host's installed build admission, run:

```sh
../qemu-10.2.1/configure --target-list=x86_64-softmmu \
    --without-default-features --enable-system --enable-kvm --enable-tcg \
    --enable-slirp --enable-pixman --disable-docs --disable-guest-agent \
    --disable-rust --disable-download --with-pkgversion=rach2-vtd-cfi
ninja -j8 qemu-system-x86_64
```

The build needs GLib, pixman, slirp and their development headers, Ninja, Meson, Python pip and setuptools. On the testing host their Ubuntu packages are unpacked with `dpkg-deb -x` into `build/cache/qemu/deps`, without a package installation. Set `PATH` to its `usr/bin`, `PYTHONPATH` to its `usr/lib/python3/dist-packages`, and `PKG_CONFIG_LIBDIR` to its `usr/lib/x86_64-linux-gnu/pkgconfig` followed by the system pkg-config directories. Relocate `prefix=/usr` in the private `.pc` files to that private `usr` directory. Set `LD_LIBRARY_PATH` to its `usr/lib/x86_64-linux-gnu`. GLib's pkg-config dependencies also need the private `libpcre2-dev`, `libffi-dev` and `libsysprof-capture-4-dev` packages. Keep the private libraries with the emulator.

Put the absolute `build/cache/qemu/out` directory first on `PATH` for the assignment run, and retain that `LD_LIBRARY_PATH`. The build tree finds its shipped firmware through its `pc-bios` link. No global emulator is replaced. Record `qemu-system-x86_64 --version`, the binary's SHA-256 and the private dependency versions with the run.

## Tests

The boot case requires the unit, both disk controllers and the display interrupt. The NIC cases require the Debian guest's answered pings. The fault cases require their positive DMA or MSI control before their blocked request. The kill and reassignment cases require every live resource count to return to its initial value.
