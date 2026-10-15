# Running the kernel

The scripts in `rach2/` boot the kernel under QEMU: with a RAM root, with a disk root, through GRUB from a CD or a disk image, or stopped for GDB. They share their device options through `qemu_devices.sh`, and each puts COM1 on the terminal with `-serial stdio`. This document lists what each script boots, the environment variables that change the machine, and the commands of the kernel debug shell. [The kernel's Readme](Readme.md) describes the kernel itself, and [cmdline.md](cmdline.md) the words of its command line.

## Use

### The scripts

| Script | Boots |
| --- | --- |
| `run.sh [IMAGE] [AUDIO]` | `build/rach2/kernel` by `-kernel`, with `build/images/IMAGE.krffs` as its boot module by `-initrd`. With no image the kernel has no root and halts, unless `RESCUE` asks for the debug shell or `DISK` gives it a root. |
| `run_from_iso.sh IMAGE [AUDIO]` | `build/images/IMAGE.iso` from the CD drive, through GRUB. |
| `run_from_disk.sh [AUDIO]` | `build/images/os-real-grub.disk`, or `os-real-grub-with-key.disk` in a tree with a recovery key, with no `-kernel`: the firmware runs the disk's boot code and GRUB's BIOS core, which loads the kernel from the boot partition. |
| `debug.sh [AUDIO]` | `build/rach2/kernel` alone, stopped at its first instruction with a GDB server on port 1234, and GDB attached to `kernel.debug` through `.gdbinit`. Run it from `rach2/`. |

`AUDIO` is `speaker`, the default, `sb16` or `hda`. The host's audio backend is CoreAudio on macOS, PulseAudio on Linux when it or PipeWire's server for it is installed, DirectSound on Windows, and SDL otherwise. `grub_install.sh DIR`, run from `rach2/`, copies the i686 kernel and the os-simple image, or the image given in `RACH2_IMAGE`, into an existing GRUB directory; it writes each file whole, since some GRUB builds refuse a sparse file.

`IMAGE` is `test`, `os-simple` or `os-real`. The test image has no `/sbin/init`, so it boots to the debug shell; os-simple and os-real start at a login prompt.

```sh
make
cd rach2
./run.sh os-simple              # RAM root, /sbin/init, console login
RESCUE=1 ./run.sh os-simple     # the same root, the kernel debug shell
RESCUE=1 ./run.sh               # no root, the kernel debug shell
./run.sh test                   # the test image: the debug shell
```

### The environment

| Variable | Read by | Default | Effect |
| --- | --- | --- | --- |
| `ARCH` | every script | `i686` | `x86_64` boots `build/x86_64/rach2/kernel` and the images under `build/x86_64/images/` on `qemu-system-x86_64`. |
| `CPU` | `run.sh`, `run_from_iso.sh`, `run_from_disk.sh` | `max` | The QEMU processor model. `max` has `RDRAND` and `RDSEED`, so the random generator has a trusted seed ([krandom](sys/lib/krandom.Readme.md)); without one, `getrandom` fails and Dropbear cannot make its keys. |
| `NIC` | every script | `rtl8139` | The network card, `rtl8139` or `e1000`, on QEMU's user network. |
| `NET_HOSTFWD` | every script | `hostfwd=tcp::8080-:80` | The user network's forwarding rule. `run_from_disk.sh` forwards host port 2222 to the guest's SSH server instead. |
| `NET` | every script | `on` | `off` leaves the network options out, so QEMU adds its default e1000 with no port forwarded. |
| `DISPLAY_MODE` | every script | unset | Passed to QEMU's `-display`. Unset, it is `none` on a host other than macOS and Windows when neither `DISPLAY` nor `WAYLAND_DISPLAY` is set, as in an ssh session, and QEMU opens no window; the console is then the terminal alone. |
| `DISK` | `run.sh` | none | A raw disk image, attached on the IDE bus, and `rach2.fsdisk` added to the command line. |
| `DISKBUS` | `run.sh` | IDE | `ahci` attaches `DISK` to an ICH9 AHCI controller instead, and adds `rach2.disk=ahci`, which the kernel ignores. |
| `DISKNOMOUNT` | `run.sh` | none | `1` attaches `DISK` without `rach2.fsdisk`, so that the kernel mounts the module and the debug shell's `disk` reads the disk. |
| `RESCUE` | `run.sh` | none | `1` adds `rach2.rescue`. |

### A disk root

A persistent root is a disk image. Its page cache is written back, so a change is on the disk after `sync`, and `halt -f`, `poweroff -f` and `reboot -f` write it before they stop the machine; a QEMU that is killed leaves the dirty-mount flag set.

```sh
make os-real-disk
cd rach2
DISK=../build/images/os-real.disk ./run.sh
DISK=../build/images/os-real.disk DISKBUS=ahci ./run.sh
```

### The loader paths

```sh
make os-simple-iso && (cd rach2 && ./run_from_iso.sh os-simple)
make os-real-grub-disk && (cd rach2 && ./run_from_disk.sh)
```

The GRUB disk has the layout an installed machine has: a GUID table with the boot partition, the recovery partition and the ext2 root ([disk(7)](../man/man7/disk.7)). Its kernels are its own, in the boot partition's slots, so a newer kernel on the host does not reach it until `make os-real-grub-disk` makes the disk again, which discards what was installed on it.

The stick image, which `make rach2-stick-image` builds, boots through the firmware with no kernel given, as a machine boots from a USB socket:

```sh
qemu-system-i386 -cpu max -device qemu-xhci,id=xhci \
    -drive if=none,id=stick,format=raw,file=../build/images/rach2-stick.img \
    -device usb-storage,bus=xhci.0,drive=stick,bootindex=0
```

### The debug shell

The kernel debug shell is the supervisor of a boot with `rach2.rescue`, and of a root with no `/sbin/init`, such as the test image. It authenticates nobody, which is why a boot that mounts no root halts instead of starting it. It reads lines from the keyboard and COM1 and prints `Type 'help' for available commands.`, which the suites wait for. Its commands, as `help` lists them:

* Inspection: `help`, `info`, `modules`, `test` (the boot's self-tests again) and `clear`.
* The mounted root: `ls`, `cat FILE`, `touch FILE`, `rm FILE`, `mv OLD NEW` and `write FILE DATA`.
* Programs: `elf FILE` prints an executable's header, and `exec FILE [ARGS...]` runs it as a transient init, which returns to the shell when it ends.
* Storage: `disk` probes the boot disk and dumps sector 0, and `ahci` probes the AHCI controller and reads LBA 0. `disk write`, in the recovery kernel alone, writes the last sector and reads it back; it refuses when an MBR entry covers that sector, or when LBA 1 or the last sector begins with the GPT signature `EFI PART`.
* Exercises: `switch`, `preempt`, `isolate`, `usermode`, `netping` (the gateway, 10.0.2.2), `netbad` (an address that does not answer) and the `audio` commands of [the audio Readme](sys/audio/Readme.md).
* Stops: `halt`, `poweroff` and `reboot` write the mounted disk root's dirty pages before they stop the machine ([the PC platform](sys/arch/x86/Readme.md) has the power off and reset methods), and `fault` panics the kernel by a page fault.

The shell's prompts and output go to the screen and COM1 through `console_putc`, `console_puts` and `console_printf`, and never to the log ring or the network: they are not diagnostics, and netconsole broadcasts the log to everyone on the segment.
