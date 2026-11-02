# The Lenovo's facts

What the Lenovo G50-70 reported on 2026-11-02, from 03:32 to 04:31 UTC+6, about its Wi-Fi card, the QCA9565 at 02:00.0: the card's configuration space, its interrupt route, its link power states, the DMA remapping hardware, and the `VPC2004` device and the embedded controller's Wi-Fi bits that the airplane key works through. It closes with Linux's view of the card from one Alpine boot, and the hang that boot left at 04:08.

## How it was read

* Every read ran over SSH as root on release 3 (`2dd8dd1b`), whose kernel `8b9611b2...aecd7` (2,198,480 bytes) both boot slots held before and after, except the configuration space, which was read at 03:44 on one trial boot of task 8a's kernel (`540c1cdec`'s change applied to master) through `bootctl kernel`, and the Linux section.
* ACPI methods and objects were evaluated through `/proc/acpi/call`, with `c "\_SB.PCI0.LPCB.EC0.WLAN"` standing for `printf '%s' PATH > /proc/acpi/call; tr -d '\0' < /proc/acpi/call`. None of them writes the firmware's state, except the `VPCW` command writes below, which are how Linux's `ideapad-laptop` reads the controller too.
* The ACPI tables were copied from `/dev/mem` with `dd` at the addresses the kernel's `acpica:` lines give (the DMAR table at 0xAAFCB000, 0xB0 bytes; the DSDT at 0xAAFE4000) and disassembled on the Mac by `iasl -d` of ACPICA 20260930, the vendored tarball. A DSDT line number below is in that disassembly.
* A register of the DMA remapping hardware was read with `devmem ADDRESS 32`.

## The card's configuration space

`od -An -tx4 -w16 -v /sys/bus/pci/devices/0000:02:00.0/config` gave 4,096 bytes; the header and capabilities:

| Offset | Value | Meaning |
|---|---|---|
| 0x00 | 0x0036168C | 168c:0036 |
| 0x04 | 0x00100002 | memory decoding on, bus mastering off; a capability list |
| 0x08 | 0x02800001 | class 0x0280, revision 1 |
| 0x10 | 0xC0400004 | BAR 0, 64-bit memory at 0xC0400000; Linux sizes it 512 KiB |
| 0x2C | 0x402617AA | subsystem 17aa:4026 |
| 0x30 | 0xFFFF0000 | a 64 KiB ROM BAR, unassigned; Linux put it at 0xC0480000 |
| 0x3C | 0x00000100 | interrupt pin 1 (INTA), interrupt line 0 |
| 0x40 | 0xFFC25001 | power management, version 2; 0x44 is 0, so D0 |
| 0x50 | 0x01847005 | MSI, off |
| 0x70 | 0x00020010 | PCI Express, version 2, endpoint |
| 0x78 | 0x00192010 | max payload 128, max read request 512 |
| 0x7C | 0x00036C11 | link capabilities |
| 0x80 | 0x10110043 | link control and status |
| 0x100 | 0x14010001 | advanced error reporting, next at 0x140 |
| 0x140 | 0x16010002 | virtual channel, next at 0x160 |
| 0x160 | 0x00010003 | device serial number, all zeros, the last |

The subsystem 17aa:4026 is also in `/sys/bus/pci/devices/0000:02:00.0/uevent` on the release (`PCI_SUBSYS_ID=17AA:4026`). Linux's ath9k gives this ID `ATH9K_PCI_AR9565_2ANT | ATH9K_PCI_BT_ANT_DIV`, and Linux printed `WB335 2-ANT card detected` for it.

The device status at 0x7A, 0x0019, has correctable error and unsupported request detected set, from before any driver touched the card.

The capability list was walked with this reader, which BusyBox's `od` runs:

```sh
# pcicaps.sh DEVICE: list the capabilities in /sys/bus/pci/devices/DEVICE/config.
f=/sys/bus/pci/devices/$1/config
b() { od -An -tu1 -j"$1" -N1 "$f" | tr -d ' '; }
w() { od -An -tx4 -j"$1" -N4 "$f" | tr -d ' '; }
p=$(b 52)
while [ "${p:-0}" -ne 0 ]; do
    echo "cap 0x$(printf %02x "$(b "$p")") at 0x$(printf %02x "$p"): $(w "$p")"
    p=$(b $((p + 1)))
done
p=256
while [ "$(wc -c < "$f")" -gt 256 ] && [ "$p" -ne 0 ]; do
    v=$(w "$p")
    [ "$v" = 00000000 ] && break
    echo "ext 0x$(printf %04x $((0x$v & 0xffff))) at 0x$(printf %03x "$p"): $v"
    p=$(((0x$v >> 20) & 0xffc))
done
```

`busybox lspci` lists the card as `02:00.0 Class 0280: 168c:0036`; it does not decode capabilities.

## MSI

The capability at 0x50 has message control 0x0184: MSI off, four vectors requestable (multiple message capable 2), 64-bit addresses and per-vector masking. Its address and data are zero. Linux's ath9k left it off and ran on INTx; it uses MSI only with its `use_msi` parameter.

## The interrupt route

* The card's interrupt pin register reads 1, INTA, both in Rach2's `config` file and in Linux's (`od -An -tx1 -j60 -N4 .../config` gave `00 01 00 00`). The firmware writes no interrupt line: the register is 0.
* The card sits behind root port 4, `\_SB.PCI0.RP04`, which is 00:1c.3 (`c "\_SB.PCI0.RP04._ADR"` gives 0x1c0003; the bridge's secondary bus is 2).
* Its `_PRT` (DSDT line 15535) returns `AR07` when `PICM` is set and `PR07` otherwise. `c "\PICM"` gives 1 on the release, which Rach2's `_PIC(1)` sets, and `c "\_SB.PCI0.RP04._PRT"` gives `[[0xffff, 0x0, 0x0, 0x13], [0xffff, 0x1, 0x0, 0x10], [0xffff, 0x2, 0x0, 0x11], [0xffff, 0x3, 0x0, 0x12]]`: INTA to GSI 19, INTB to 16, INTC to 17, INTD to 18. In PIC mode INTA goes to the link `LNKD`.
* So the card's interrupt is GSI 19 on the I/O APIC, level-triggered, as Linux had it: `19: ... IR-IO-APIC 19-fasteoi ath9k`, 227 interrupts over the driver's load and two scans.
* Linux's `lspci -vvv` printed `Interrupts: pin B routed to IRQ 19`, which disagrees with the register it read; the route it used is the `_PRT`'s entry for INTA.

## The link and ASPM

* Link capabilities 0x00036C11: 2.5 GT/s, x1, port 0; ASPM L0s and L1 supported; L0s exit 2 to 4 us, L1 exit 32 to 64 us.
* Link control 0x0043: ASPM L0s and L1 both enabled, common clock. Link status 0x1011: 2.5 GT/s, x1, slot clock.
* The root port 00:1c.3 also has L0s and L1 enabled (its link control at 0x50 reads 0x70110043; the link is up). Its L1 PM substates capability at 0x200 has control 1 at 0x208 of 0x00000010, so L1.1 and L1.2 are off.
* The firmware enables ASPM and leaves it so: its FADT says the OS may not change it. Linux printed `ACPI FADT declares the system doesn't support PCIe ASPM, so disable it` and `FADT indicates ASPM is unsupported, using BIOS configuration`, and ath9k printed `ASPM enabled: 0xc43`, the root port's link control as Linux left it.

## DMA remapping

* The DMAR table's flags are 0x01: interrupt remapping is supported; neither x2APIC opt-out nor DMA control opt-in is set. The host address width is 39 bits.
* Unit 0, at 0xFED90000, covers only the graphics device 00:02.0. Unit 1, at 0xFED91000, has `INCLUDE_PCI_ALL` and so covers the card; it also scopes I/O APIC 2 and HPET 0.
* Two reserved regions: 0xAAE11000 to 0xAAE30FFF for the USB controllers 00:1d.0 and 00:14.0, and 0xAB800000 to 0xAF9FFFFF for the graphics device.
* The global status register of both units reads 0 on the release (`devmem 0xFED9001C 32` and `devmem 0xFED9101C 32`): the firmware hands over with translation and interrupt remapping both off, and Rach2 turns neither on, so the card's DMA addresses are physical addresses.
* Linux turned on interrupt remapping (`DMAR-IR: Enabled IRQ remapping in x2apic mode`) but not translation: Alpine's kernel is built with `CONFIG_INTEL_IOMMU=y` and without `CONFIG_INTEL_IOMMU_DEFAULT_ON`, and the boot made no IOMMU groups. Whether the card works under translation, and its IOMMU group, are left to the later task that takes up Linux on this machine.

## The VPC2004 device and the controller's Wi-Fi bit

The device is `\_SB.PCI0.LPCB.EC0.VPC0` (`c "\_SB.PCI0.LPCB.EC0.VPC0._HID"` gives `"VPC2004"`), with `_STA` 0xF. Its methods, from DSDT line 3176: `_STA`, `_CFG`, `VPCR`, `VPCW`, `SVCR` (empty), `HALS`, `SALS`, `GBMD`, `SBMC`, `MHCF`, `MHPF`, `MHIF`, `HODD`, `SODD`, `GBID`, `APPC`, `DBSL`, `SBSL`, `STHT`, `_REG`, `SERF` and `GCBL`.

* `VPCR (1)` returns the controller's command register `VCMD`, `VPCR (0)` its data register `VDAT`; `VPCW (1, x)` writes a command and `VPCW (0, x)` data. A read is: write the command, wait for `VPCR (1)` to read 0, read `VPCR (0)`.
* `_CFG` is not a read: under Windows 7 (`OSYS` 0x7D9) it sets the controller's `WEXT`, `WLAN` and `BLUE` bits and the Wi-Fi and Bluetooth GPIOs from `CMWL` and `CMBL`, and under every OS it sets `APFG` to 1, then returns `_VPC`. It was not called. `c "\APFG"` gives 0: nothing has called it since boot.
* `c "\_SB.PCI0.LPCB.EC0.VPC0._VPC"` gives 0xFDE104, the value `_CFG` returns. In `ideapad-laptop`'s bits, 16 (Bluetooth), 18 (Wi-Fi) and 19 (camera) are set and 17 (3G) is clear.
* `HALS` gives 0x608: bits 3, 9 and 10; Linux names 9 Fn-lock support and 10 its state, which is on while `HKDB` is 0.
* `OSYS` is 0x7DD, which the DSDT sets when `_OSI ("Windows 2013")` is true, as Rach2's ACPICA answers it; 0x7D9 is "Windows 2009". So the Windows 7 branches of `_CFG` and of the controller's query `_Q42` do not run.

The commands, with the script that read them:

```sh
V='\_SB.PCI0.LPCB.EC0.VPC0'
c(){ printf '%s' "$1" > /proc/acpi/call; tr -d '\0' < /proc/acpi/call; }
rd(){ c "$V.VPCW 1 $1" > /dev/null; i=0; while [ "$(c "$V.VPCR 1")" != 0x0 ] && [ $i -lt 100 ]; do i=$((i+1)); done; printf 'cmd %s: %s (polls %d)\n' "$1" "$(c "$V.VPCR 0")" $i; }
for k in 0x14 0x16 0x23 0x10 0x1a; do rd $k; done
```

| Command | Linux's name | Value |
|---|---|---|
| 0x10 | `VPCCMD_R_VPC1`, the event bits | 0x10, bit 4: a brightness event |
| 0x14 | `VPCCMD_R_WIFI` | 0 |
| 0x16 | `VPCCMD_R_BT` | 0 |
| 0x1A | `VPCCMD_R_VPC2` | 0 |
| 0x23 | `VPCCMD_R_RF`, the radio switch | 1, not blocked |

The controller's own bits are fields of `EC0` at byte 0xBB of its address space: `WLAN` bit 0, `BLUE` 1, `WEXT` 2, `BEXT` 3, `KILL` 4, `WLOK` 5 (DSDT line 2927). `c "\_SB.PCI0.LPCB.EC0.WLAN"` and the rest gave `WLAN` 0, `BLUE` 0, `WEXT` 0, `BEXT` 0, `KILL` 1, `WLOK` 0, at 03:39 and `WLAN` 0, `KILL` 1 again at 04:31.

What powers the radio is GPIO 0x58, which `_CFG`, `_Q42` and `_PTS` drive through `\_SB.WTGP`; Bluetooth's is 0x57. `c "\_SB.RDGP 0x58"` gives 1, the output level (bit 31 of the GPIO's register at `GPBS` + 0x100 + 0x58 * 8, with `GPBS` 0x800); `c "\_SB.RDGI 0x58"`, the input level, gives 0; `c "\_SB.RDGP 0x57"` gives 1. So the card is powered while the controller's `WLAN` bit and `VPCCMD_R_WIFI` both read 0. Linux agreed: `ideapad-laptop` made its `ideapad_wlan` switch soft-blocked from that 0, yet ath9k's own `phy0` was not blocked and both scans ran.

The controller's queries that notify `VPC0` with 0x80 are `_Q11` and `_Q12` (brightness down and up, which also notify the graphics output) and `_Q1C` and `_Q44`, which do nothing else. One of the last two is the airplane key, and `VPCCMD_R_VPC1` then names it by its bit; which one needs a press of the key, which this task does not make. Task 7 and the sitting settle it.

## Linux with ath9k

Before task 8d was struck, one boot of Alpine's netboot kernel 6.18.58-0-lts ran on the Lenovo from 04:05 to 04:08, with ath9k loaded under mmiotrace. Its logs are in `linux/`: `dmesg.txt`, `info.txt` (`lspci -nnvvv`, `/proc/interrupts`, `iw list`, `iw reg get`) and `ath9k-mmiotrace.txt.gz`. The two scans, which list the neighbours' networks, are not kept.

* ath9k printed `Atheros AR9565 Rev:1 mem=..., irq=19`, `WB335 2-ANT card detected`, `Set BT/WLAN RX diversity capability`, `Enable LNA combining` and `ASPM enabled: 0xc43`; the EEPROM's regulatory domain is 0x6a, country 00.
* `iw list`: antennas TX 0x1, RX 0x3; HT capabilities 0x116e, HT20 and HT40, short guard interval at both, RX STBC of one stream; MCS 0 to 7; channels 1 to 13 at 18 dBm, 12 without initiating radiation, 14 disabled. The interface came up at 15 dBm.
* The trace has 35,246 lines: 13,255 register reads and 21,975 writes, over the driver's load and two scans. mmiotrace took the other three CPUs offline while it ran.
* The scans found 21 and 30 networks. The owner's TDA-NET was on 2442 MHz, channel 7, at -27 and -16 dBm.

## The hang at 04:08

What was done, from the worker's session log, in UTC+6:

* 03:43 to 03:49: task 8a's kernel ran on a trial boot through `bootctl kernel`; the release went back with `bootctl kernel`, a boot, `bootctl commit` and `bootctl fallback`.
* 03:53 to 04:03: the Linux boot was rehearsed in QEMU under UEFI from a copy of the Lenovo's boot partition, and passed.
* 04:04: the Lenovo's boot partition was mounted read-write on Rach2 and given `/boot/t2251/` (Alpine's `vmlinuz-lts` and `initramfs-lts`, and an overlay archive) and `/boot/grub/x86_64-efi/linux.mod` from Ubuntu's `grub-efi-amd64-bin` 2.12-1ubuntu7.3. `bootctl config` staged a menu whose one entry booted Linux when `next_entry` was `linux2251`, clearing it first. `bootctl` refused to arm that entry, so `next_entry` was written into the grubenv's two sectors with `dd` to `/dev/bootpart`, outside `bootctl`.
* 04:04:51: `reboot` on Rach2. GRUB took the entry and cleared it; Linux answered SSH at 04:06:38.
* 04:06:56 to 04:07:41: the trace and the scans, and the logs copied to the Mac.
* 04:08:04: `reboot` on Linux, a warm reboot. The machine did not come back: no ping and no ARP entry by 04:17. The firmware stood at "EFI boot failed", and the owner power-cycled it. The release booted from the candidate slot at about 04:20.

The menu would have booted the release, since GRUB had cleared `next_entry`; the firmware failed before GRUB ran, and why is not known. After it the manager removed `/boot/t2251` and `linux.mod`, restored the menu with `bootctl config`, and checked both slots. The `machine` skill now has a step on the Lenovo boot only Rach2 and write the boot partition only through `bootctl`, keep a way back that needs no person, and leave another system by power-off and the RTC alarm, never a warm reboot.

## The release in both slots

At 04:31, with the boot partition mounted read-only on the release:

```sh
for f in kernel fallback; do head -c 2198480 /boot/$f | sha256sum; done
cmp /boot/kernel /boot/fallback
```

gave `8b9611b26d79114725d65955e2ff2fbc4a28dce16dca531530348a8da99aecd7` for both, the release's kernel, and the two files are identical. `/boot/grub/grub.cfg` and `/boot/boot.cfg` have the SHA-256 they had at 03:49, before the Linux files went on, and `/boot/t2251` and `/boot/grub/x86_64-efi` are gone. `bootctl status` gives `saved_entry: candidate`, `next_entry: empty` and `booted: candidate`, and `/etc/rach2/release` names `2dd8dd1b`.

A raw read of each slot from `bootctl status`'s first sector does not check this: the slot files are not contiguous on the partition, so 8,192 sectors from the first one differ after 2 MiB.

## Bring-up and scan

Rach2's ath9k driver on the card, over SSH on trial boots through `bootctl kernel`, silent, with `mixer vol 1` after each boot. The first trial, from 07:23 UTC+6, booted kernel `bce906cf...01ad` (811fa66af, x86_64, 2,662,344 bytes); both slots held the release before it, read through the FAT. On the trial boot `bootctl status` gave `saved_entry: fallback`, `next_entry: empty` and `booted: candidate`, so a hang would have given the release at the next power-up. The kernel log was streamed over SSH throughout, from `/dev/bootlog`, which does not follow, with this reader:

```sh
# follow.sh: print what the kernel log gains, five times a second.
n=$(wc -c < /dev/bootlog)
while :; do
    m=$(wc -c < /dev/bootlog)
    if [ "$m" -gt "$n" ]; then
        tail -c +$((n + 1)) /dev/bootlog | head -c $((m - n))
        n=$m
    fi
    usleep 200000
done
```

### The boot line and the MAC address

```
ath9k: interrupts: messages on vector 52
ath9k: probe: 02:00.0 AR9565 rev 1.0.1 mac b8:ee:65:32:89:15 subsys 17aa:4026
```

`ip link` shows `wlan0` down with `link/ether b8:ee:65:32:89:15`. Linux's ath9k wrote the same address to the card: `AR_STA_ID0` 0x3265eeb8 and the low half of `AR_STA_ID1` 0x1589, at lines 4566 and 4987 of the mmiotrace. The worker kwlan is pid 3 and init pid 1. The command word reads 0x0006 with messages on: memory decoding and bus mastering, Interrupt Disable clear.

### The radio's start

`ip link set wlan0 up` returned 0 at once, and `wlan0` read `<NO-CARRIER,BROADCAST,MULTICAST,UP>`. The log gained only the interface's IPv6 address; no `polled` line, no reset. `/proc/interrupts` counted 2,207 messages on vector 52 in the first 37 s and then a steady 59 a second, the beacons heard on channel 1.

### A scan

`iw dev wlan0 scan`, with a static iw 6.17 copied to `/dev/shm`, returned in under a second with 26 BSSes: 12 on channel 1, 1 each on 2, 3, 5, 10, 11 and 13, 2 on 4, 4 on 6 and 3 on 8. Linux's two scans had found 21 and 30.

* TDA-NET, the owner's network, was BSS 60:a4:b7:a4:eb:63 on 2427 MHz, channel 4, at -15 dBm; Linux had found it on channel 7 at -27 and -16 dBm, so the router has changed channel since. A second scan, 7 minutes later, gave it at -57 dBm among 19 BSSes.
* The Mac (`system_profiler SPAirPortDataType`, which hides the names of networks it has not joined) saw 2.4 GHz networks on channels 1 (two), 4, 5, 6, 8 and 10, and gave a signal only for its channel 4 network, TDA-NET, at -25 dBm. Each of those channels has networks in Rach2's list.
* Signals run from -15 to -92 dBm, except nine BSSes on channel 1 at -99 to -114 dBm, each heard in the scan itself. The driver reports the noise floor plus `AR_RxRSSICombined`, as ath9k does, so these are frames whose combined RSSI read negative. Only channel 1, where the radio had listened for 80 s before the scan, shows them.

### A stuck queue

* `AR_Q_TXD` cannot hang a queue: on this chip it makes a queue finish what it holds, as ath9k's `ath9k_hw_stop_dma_queue` relies on. With all ten bits set and a scan of the channel the radio sat on, the probe request was sent and no reset followed.
* Clearing the ten `AR_DnQCUMASK` words (0x1000 to 0x1024) does hang it: a scan of the same channel left `AR_Q_TXE` at 0x8, a frame in QCU 3, the VO queue that probe requests take, for two minutes.
* No reset came, because the hang check ran only once, a second after the start: it queued its own record again from inside its callback, which the worker refuses (11b2c9de4 fixes it).
* A full scan's first retune then aborted TX, with its bounded waits, and reset the chip: `AR_Q_TXE` read 0, the masks 1 << n again, and the scan found 19 BSSes, TDA-NET among them. Messages stayed on throughout, 24,911 of them by 07:32, with no `polled` line.

### A forced reset and a second scan

The second trial, from 07:37 UTC+6, booted kernel `5b503978...eb72` (11b2c9de4, 2,662,384 bytes) under the same conditions, and `bootctl status` on it gave `saved_entry: fallback` and `next_entry: empty` again.

* The start, a full scan (22 BSSes) and a scan of channel 1 ran as on the first trial, with the hang check now running each second; it found nothing in the 36 s before the masks were cleared.
* At 07:38:12 the ten `AR_DnQCUMASK` words read 1 << n and were written 0, with `AR_Q_TXE` at 0. A scan of channel 1 then left a frame in QCU 3, and the log gave `<6>[76.685000] ath9k: reset: tx-hang: recovered`, the check's second look at the queue. After it `AR_Q_TXE` read 0 and the masks 1 << n, and no other reset followed.
* At 07:38:32 a full scan returned 20 BSSes, 15 of them heard in the scan itself, TDA-NET among them at -57 dBm on channel 4.
* At 07:38:57 `ip link set wlan0 down` returned 0, and no message came in the 3 s after it. The trial sent 3,583 messages in all, with no `polled` line.

### The release back

The release's kernel was copied from the fallback slot through the FAT, armed with `bootctl kernel`, booted (`/etc/rach2/release` names 2dd8dd1b), committed and copied to the fallback slot. Through the FAT, both `/boot/kernel` and `/boot/fallback` then read `8b9611b26d79114725d65955e2ff2fbc4a28dce16dca531530348a8da99aecd7` over the release's 2,198,480 bytes, zero after them, and `cmp` finds them identical; `bootctl status` gives `saved_entry: candidate`, `next_entry: empty` and `booted: candidate`.
