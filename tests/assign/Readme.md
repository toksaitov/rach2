# Device assignment

The assignment cases boot the GNU root on q35 with an emulated Intel VT-d unit. Its root has the GNU product layers, with no test inventories or staged personal logins, and is selected by the fixed MBR identity `82274701,1` so disk enumeration cannot select the payload. A virtio NIC goes to Debian through Rach2's VFIO, while Rach2 keeps its e1000, NVMe root, AHCI payload disk and virtio-gpu display. The EDU device forces DMA and interrupt faults through the same unit. Four more cases give the function that Rach2's Intel driver has to a guest and take it back, with the script and QEMU's command line that the Lenovo's GPU gets, `gpu.sh`; here the function is EDU under the driver's fake chip. Each case has its own root overlay and powers its nested guest off or kills and reaps it; etude stops the outer machine.

## Running

`ARCH=x86_64 tests/run assign` runs the eleven cases in `tests/cases/machine.assign.json` on Linux. `RACH2_CASES=machine.assign.boot` selects just the boot. The remaining cases are `nic-tcg`, `dma`, `index`, `compat`, `kill` and `reassign`, and the driver's four, `gpu`, `gpu-uefi`, `gpu-grub` and `gpu-kill`, which boot the outer kernel with `rach2.i915fake`. TCG runs the nested guest; the outer machine uses KVM on WSL. The runner also has a `nic-kvm` mode for a comparison made by hand inside the same outer machine with the `host-svm` processor profile.

The machine has `intel-iommu,intremap=on,caching-mode=on`, a `virtio-net-pci,disable-legacy=on,iommu_platform=on` at `0000:01:00.0` behind the PCIe root port at `0000:00:06.0` on the user network `10.0.3.0/24`, and EDU at `0000:00:07.0` with its DMA mask covering every address. The host's e1000 uses a separate user network, `10.0.2.0/24`. The two gateways end in `.2`. `iommu_platform=on` makes the NIC use the bus's DMA address space through VT-d. QEMU gives virtio its PCIe function-level reset capability only behind a root port, as `virtio_pci_realize` in [QEMU 10.2.1](https://github.com/qemu/qemu/blob/v10.2.1/hw/virtio/virtio-pci.c) defines it.

`run.sh` runs as root in the GNU guest. It binds the two functions to vfio-pci and sets the locked-memory limit to unlimited. During each case it writes random data to the ext2 root, flushes and verifies it, and pings the host's gateway. It compares the live VFIO, KVM and free interrupt-vector counts before and after the operation. No IOMMU fault may precede a case, and only the one deliberate fault of an injection case may follow it. The kill case requires live pinned DMA pages and a mapped NIC BAR before sending SIGKILL; the reassignment case then boots a second guest with the same function. The first guest's console is printed as it arrives and kept beside QEMU's diagnostics in the case's scratch directory, which remains after a failure or an interrupted run.

On the Lenovo, as root, with the guest's kernel and initramfs or its disk:

```sh
bash gpu.sh -k vmlinuz -i initrd -l 'root=/dev/sda1' -w 0
bash gpu.sh -f uefi -D debian.img -w 0
bash gpu.sh -k vmlinuz -i initrd -K 'TOKEN'
```

`gpu.sh` takes the function, 0000:00:02.0 unless `-d` names another, from the Intel driver through `/sys/bus/pci/drivers/i915/unbind`, gives it to vfio-pci, and runs

```sh
qemu-system-x86_64 -machine q35,kernel-irqchip=split -accel kvm -cpu host -m 2048 -smp 2 \
    -nodefaults -vga none -display none -no-reboot -serial file:DIR/console \
    -device vfio-pci,host=0000:00:02.0,bus=pcie.0,addr=02.0,x-igd-opregion=on
```

with the guest's kernel loaded by QEMU under SeaBIOS, the form with which Linux gave the Lenovo's GPU to a guest; `-f uefi` boots by OVMF, and `-D` a disk with the guest's own boot loader. `x-igd-opregion=on` is given only for an Intel display function. When QEMU ends, by the guest's power off, or by the script's SIGKILL at the token `-K` names or when the script is sent SIGUSR1, and after any failure once the function has left the driver, the script takes the function back from vfio-pci, restores its `driver_override` and writes its name to `/sys/bus/pci/drivers/i915/bind`, which resets it and binds the driver. It passes when the driver has the function again with a DRM card, logged the release, no IOMMU fault was counted, and the counts of VFIO, KVM and the free interrupt vectors are as they were. The script's opening comment has every option.

## Interfaces

`edu.c` uses Type1v2 VFIO and an eventfd. Its DMA control copies a sentinel into EDU and back, then unmaps the IOVA and requires a DMA fault without a change to the sentinel. Its interrupt control requires a delivered remapped MSI, then writes the real MSI configuration through the privileged `/sys/bus/pci/devices/BDF/config` file to send either an out-of-range remapping index or a compatibility-format message. Each must raise `iommu_irq_faults` without signalling the eventfd. The real MSI address is restored before VFIO releases the device.

In its `gpu` cases `run.sh` runs `gpu.sh` on EDU, which the fake chip of `rach2.i915fake` has bound as the Intel driver's device ([i915](../../rach2/sys/drm/i915/Readme.md)), under TCG, with the disk and network traffic and the counts around it as in every case. The guest, started with `assign.gpu`, prints the ids and class of the function at 00:02.0 and the first register of its first memory BAR, read through `/dev/mem`, which is EDU's identification, 0x010000ED; `gpu-grub` also requires the line the guest's GRUB menu prints, and `gpu-kill` kills QEMU while the guest holds the function.

The counters are the kernel's `/proc/stat` records, described in [vfio](../../rach2/sys/vfio/Readme.md#interfaces) and [VT-d](../../rach2/sys/arch/x86/iommu/Readme.md#use). The EDU register offsets and DMA commands follow [QEMU's EDU specification](https://www.qemu.org/docs/master/specs/edu.html). No elapsed time decides a result.

## Local changes

The outer QEMU needs `qemu-vtd-cfi.patch`. QEMU 10.2.1 does not acknowledge GCMD.CFI in GSTS.CFIS and forwards compatibility-format MSIs even when CFI is clear. The patch adds the status update and fault reason 0x25, preserving delivery when CFI is set or interrupt remapping is off. The patch changes the test host's emulator, not QEMU in the GNU guest.

## Building

`make ARCH=x86_64 build/tests/cases/assign/assign.ext2` builds the payload. `mkguest.sh` uses the Debian kernel and BusyBox packages pinned by `tests/kvm/guests/debian.txt`. The initramfs includes the NIC modules and their dependencies in load order, leaving out dependencies Debian built into its kernel. The guest uses a static address, pings its gateway eight times, prints `ASSIGNGUEST OK` and powers off. With `assign.hold` it keeps pinging and prints `ASSIGNGUEST TRAFFIC` after each completed batch. With `assign.gpu` it reports the function at 00:02.0 instead and powers off, or with `assign.hold` as well prints `ASSIGNGUEST HOLD` each second. `mkguest.sh` also writes `grub.img`, the guest's own disk: a 48 MiB FAT volume with Debian's `grubx64.efi`, the image a Debian installation boots, as `EFI/BOOT/BOOTX64.EFI`, its menu `grub.cfg` at the image's prefix, `EFI/debian`, and the same kernel and initramfs. The GRUB package is pinned in `guest.txt` from the kernel's snapshot, and the volume is made with mtools.

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

The boot case requires the unit, both disk controllers and the display interrupt. The NIC case requires the Debian guest's answered pings. The fault cases require their positive DMA or MSI control before their blocked request. The kill and reassignment cases require every live resource count to return to its initial value. The `gpu` cases require the Intel driver's release of its function, the guest's report of EDU at 00:02.0 with its identification register read through the BAR, the driver bound again with its DRM card, and every count as before: under SeaBIOS and under UEFI with the kernel QEMU loads, under UEFI from the guest's disk by the guest's GRUB, and with QEMU killed while the guest runs.
