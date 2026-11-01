# The QCA9565 driver

Milestone 70 drives the Lenovo G50-70's own Wi-Fi card, a Qualcomm Atheros QCA9565 (AR9565, PCI 168c:0036, 2.4 GHz, 1x1 802.11b/g/n), under the station layer milestone 69 builds, so that the Lenovo joins the networks 69 joins in QEMU: WPA, WPA2 and WPA3 personal, and WPA2-Enterprise with the first-connection certificate pin, the owner's university network and eduroam among them. The airplane key turns the radio off and on. It is stable enough to demonstrate: a soak of hours with traffic, AP restarts and moves passes on the machine. It ends with the owner's one sitting, which also takes the steps of milestones 67 and 68 that need the owner at the Lenovo.

## The owner's words and decisions

The owner's four messages and the decisions are in `dev-log/69-wifi/plan.md`, which this plan shares; the study is `build/flow/coordinator/wifi/research.md` and the reviews are in `build/flow/coordinator/wifi/review/`. In short: the driver for this chip, learned from Linux's ath9k and the BSDs (GPL-2.0-only for Rach2's own code, so ath9k's ISC code may be taken and Linux's GPL code read and followed, each file naming what it follows); stable enough for conferences; the university network through the certificate accept; no captive portals and no USB networking.

* The sitting: every step that needs the owner at the machine waits for one sitting at the very end of all the open work, task 13 here. No other milestone waits for it, and every step a loop can do over SSH on a trial boot runs before it. This milestone ends with the sitting; the owner names its time once the machine is ready, and nobody asks sooner.
* Moved here from milestone 68 by the owner's order of 2026-11-02: #2023's in-person steps, which are cmus on the speaker (#2026), the internal mic (#2027), Doom played by hand (#2025), term's bell and flash (#1941) and OpenCode's look on the screen (#1938).

## What the plan takes from the study and the reviews, and what it sets aside

* Follows the study: ath9k as the reference (ISC, maintained, carries the AR9565's fixes), with FreeBSD's AR9300 HAL (the vendor's own description, ISC) and OpenBSD's `ar9003.c` for the descriptor rings where ath9k is terse; no firmware, so the driver does the radio itself; the hang checks and the RX-inactivity reset in the first driver, not a later one; MSI with the tick as the fallback, since Rach2 evaluates no `_PRT`; ASPM left as the firmware set it, with a switch to turn L0s and L1 off; power save, PAPRD and coexistence off, the antenna switch set for WLAN with Bluetooth unused; HT20 only; a minstrel_ht-style rate control from its published design; the ideapad driver for the airplane key.
* Takes from the reviews: this branch is built on 69's, since its tasks code against 69's headers; the parts close on their host tests and the Lenovo's proof is gathered in the trial tasks, so no part waits for the machine; the chip model is its own file with an interface, each part with a host test of its own; the trials split in two so that one card does not hold a loop all milestone; transmit aggregation after single frames work; the airplane key needs an action in `/etc/acpi.map`, not only an event; PCI capabilities are not readable from the shell today (`synthfs.c:1012` gives PCI devices a `uevent` only), so the facts task adds a read-only `config` file.
* Sets aside: Bluetooth coexistence, HT40, power save, S3, the AR9002 and USB parts of ath9k; VFIO passthrough, which needs Linux on the Lenovo with the card given to a guest and an IOMMU group nobody has measured (task 8 says whether it is possible; using it is a later milestone's).

## Measured

* From the study: no public datasheet; 168c:0036 seen on the Lenovo, its subsystem ID not yet read (linux-hardware.org reports 17aa:4026 for G50-70s, which ath9k gives `ATH9K_PCI_AR9565_2ANT | ATH9K_PCI_BT_ANT_DIV`); an i3-4030U with VT-d and a DMAR table; Linux runs this card on INTx.
* Rach2 at 9daa19e51: `pci_msi_enable` (`rtl8168.c:1444`, `pci.h:197`) and bridge scanning (`pci.c:376`) exist; `_PRT` routing does not; `VPC0`'s Notify is dropped (`acpi/Readme.md:97`); `/etc/acpi.map` has no Wi-Fi key (`krffs/acpi/acpi.map`).
* Task 8 measures the rest before the trials need it, over SSH, silently.

## The split

Fourteen tasks. The branch is cut from 69's at its start and takes 69's branch again by merge: task 1's worker merges `origin/milestone/69-wifi` into its task branch first, once 69's task 1 has merged, and the manager merges 69 into this branch again before tasks 9 and 10 start. Task 1 writes the driver's headers and the chip model; tasks 2 to 7 then run at once in separate files. Task 8 runs now. Tasks 9 and 10 try the merged parts on the Lenovo over SSH on trial boots, silently, as the `machine` skill says; task 12 does 67's and 68's silent steps on a trial boot of their joined head; task 14 boots the 32-bit build on the Lenovo by day; task 13 is the sitting. The four milestones 67 to 70 are released together at the end, as `dev-log/69-wifi/plan.md` says; the release does not wait for the sitting.

All of the driver is under `rach2/sys/network/driver/ar9565/`, which no task of 69 touches; each task here owns its files in it.

## Tasks

### 1. The driver's headers and the chip model

Owns `driver/ar9565/reg.h`, `ar9565.h`, `io.h`, `driver/ar9565/Readme.md` and `tests/host/ar9565_model.c` and `.h`. Waits on 69's task 1.

* 1a. Merge `origin/milestone/69-wifi` into the task branch, so that 69's `radio.h` and `wireless.h` are here.
* 1b. `reg.h`: the AR9003 MAC, PCU, baseband and radio registers the AR9565 path uses, from ath9k's `reg.h`, `ar9003_phy.h` and `ar9003_mac.h` (ISC, kept with its permission text).
* 1c. `io.h`: register reads and writes and the read-modify-write forms behind an indirection a host test replaces with the model, as `rtl8168.c` does.
* 1d. `ar9565.h`: the driver's state, the calls between its parts, the EEPROM's fields.
* 1e. The chip model: a register file with hooks a part's test sets (reset, the EEPROM's words, the rings' DMA), and its interface in `ar9565_model.h`.
* 1f. The Readme's summary, Sources and license, Design with a heading for each part.
* Done: the headers compile under `-ansi` with both cross compilers; the model builds as a host object.

### 2. Bring-up

Owns `ar9565/pci.c`, `hw.c`, `eeprom.c`, `initvals.h` and `tests/host/ar9565_hw_hosttest.c`. Waits on 1.

* 2a. Probe: 168c:0036 and the subsystem flags of ath9k's `pci.c` for the Lenovo IDs 17aa:3026 and 17aa:4026, the memory window, bus mastering, 32-bit DMA; registration as a radio of `radio.h`.
* 2b. Power-on, the RTC reset and force-wake, the chip revision (1.0 or 1.1), and a refusal that names the register when the card reads 0xffffffff.
* 2c. The EEPROM or OTP: the AR9300 compressed format unpacked over the board template; the MAC address, the regulatory code, antenna and switch tables, per-rate transmit power.
* 2d. The AR9565 register tables of `ar9565_1p0_initvals.h` and `ar9565_1p1_initvals.h`, and `ath9k_hw_init`'s order of writes.
* 2e. A boot line: `ar9565: probe: 02:00.0 rev 1.N mac XX:... subsys 17aa:NNNN`.
* 2f. The host test: the model answers a recorded chip's reset and EEPROM reads and the driver reaches its boot line.
* Done: the host test passes.

### 3. Radio and calibration

Owns `ar9565/phy.c`, `calib.c`, `ani.c` and `tests/host/ar9565_phy_hosttest.c`. Waits on 1.

* 3a. The synthesizer and channel for HT20 on channels 1 to 13; the mode and band tables at every channel change.
* 3b. Transmit power from the EEPROM, capped by the regulatory domain.
* 3c. Reset-time calibration: ADC and DC offsets, IQ mismatch, carrier leak, skipping an IQ update whose offset is too high (ath9k 6f37ff96d3bd).
* 3d. The noise floor and its history; a restart after a failed noise-floor calibration (7b8aaead958e).
* 3e. Periodic calibration and temperature compensation; ANI with a switch to turn it off, since a rate stuck at 1 Mbit/s is reported with ANI on.
* 3f. The host test of each sequence's order of writes on the model.
* Done: the host test passes.

### 4. DMA, interrupts and recovery

Owns `ar9565/recv.c`, `xmit.c`, `mac.c`, `key.c`, `intr.c`, `recover.c` and `tests/host/ar9565_mac_hosttest.c`. Waits on 1.

* 4a. The EDMA receive FIFOs (high and low priority) and the receive status at each buffer's head.
* 4b. Transmit queues per access category, descriptors with four rate and retry series, and the transmit status ring feeding rate control.
* 4c. Interrupts: MSI, with the tick servicing the rings when no message arrives, as the RTL8168's fallback does; the ISR bits and masking.
* 4d. The key cache of 128 entries (TKIP with its MIC slot, CCMP, unicast management frames in hardware on this chip), anything else to software, restored after a reset.
* 4e. TSF, the beacon timers and the beacon-miss interrupt.
* 4f. The hang checks with the AR9003 offsets (3e56c80931c7), the RX-inactivity reset (b5f871ab4913), a full chip reset, and the RX filter kept while RX is stopped (f0b2c30a2029).
* 4g. The host test: frames through the model's rings, a hang found and reset, the keys back after it.
* Done: the host test passes.

### 5. Antenna, power and the bus

Owns `ar9565/antenna.c`, `power.c` and `tests/host/ar9565_ant_hosttest.c`. Waits on 1.

* 5a. The AR9565's LNA combining and antenna diversity, from the subsystem flags.
* 5b. The antenna switch set for WLAN with Bluetooth unused; coexistence off.
* 5c. ASPM left as the firmware set it, with a boot option that turns L0s and L1 off on the link, the cheap test if the card drops off the bus.
* 5d. The GPIO hard block when the EEPROM enables RF silence, as an rfkill hard block (69's task 12).
* 5e. Stop and start around suspend to idle.
* 5f. The host test on the model.
* Done: the host test passes.

### 6. Rate control

Owns `rach2/sys/network/wireless/rc_minstrel.c` and `tests/host/rc_hosttest.c`. Waits on 1.

* 6a. A minstrel_ht-style algorithm from its published design, for MCS 0 to 7 and the legacy rates, behind 69's rate-control interface, in place of its fixed table on radios that report status.
* 6b. The host test on recorded transmit-status sequences: the rate climbs on a clean link and falls on a lossy one.
* Done: the host test passes.

### 7. The airplane key

Owns `rach2/sys/acpi/ideapad.c`, its entry in the ACPI device table, the Wi-Fi line in `krffs/acpi/acpi.map` and `acpid.conf`, `tests/host/ideapad_hosttest.c` and `man/man4/ideapad.4`. Waits on 69's task 12.

* 7a. The `VPC2004` driver on ACPICA: evaluate `VPCR`, `VPCW` and `_CFG`, write `W_RF` at boot as `ideapad-laptop` does.
* 7b. The notify read from VPC1 and VPC2, bit 13 reported as `KEY_WLAN` until the sitting measures the G50-70's bit, and bit 9's resync of the rfkill state.
* 7c. `/etc/acpi.map` and `acpid.conf` turn `KEY_WLAN` into `rfkill toggle wifi`.
* 7d. The host test with a model of the methods; the manual page.
* Done: the host test passes.

### 8. The Lenovo's facts

Labels: `owner-only`. Owns `dev-log/70-wifi-chip/lenovo.md` and a read-only `/sys/bus/pci/devices/*/config` in `synthfs.c`. Waits on nothing. Silent, over SSH, as the `machine` skill says.

* 8a. A read-only `config` file for each PCI device, as Linux has, so that BusyBox's `lspci` and a short reader show capabilities; tried on a trial boot.
* 8b. The card's subsystem ID, the MSI capability, the link's ASPM state, the interrupt line the firmware routes; whether the DMAR table enables remapping.
* 8c. The `VPC2004` device's methods and the EC's Wi-Fi bit, read without pressing a key.
* 8d. Whether a loop can boot a Linux kernel with ath9k on a trial boot without the owner (through the candidate slot or GRUB's next-boot entry) to record ath9k's register reads and a scan as the baseline; if it can, do it; if not, say why and leave the baseline to the study's reports.
* Done: `lenovo.md` has each fact with the command that read it, and the release back in both slots.

### 9. The driver on the Lenovo: bring-up and scan

Labels: `owner-only`. Owns the bring-up section of `lenovo.md`. Waits on 2, 3 and 4, and on the manager's merge of 69 into this branch. Silent, over SSH, on trial boots, the release back in both slots afterwards.

* 9a. The boot line and the MAC address, against the sticker or Linux's.
* 9b. A scan: the list compared with the networks the Mac sees, with signals; TDA-NET, the owner's home network (on 2.4 GHz, the owner confirms), is in it.
* 9c. A forced reset recovers and a second scan still sees them.
* Done: each result in `lenovo.md`; a failure filed as a fix to the part it names.

### 10. The driver on the Lenovo: traffic and soak

Labels: `owner-only`. Owns the traffic section of `lenovo.md`. Waits on 5, 6, 9, 11 and 69's task 16. Silent, over SSH, on trial boots.

* 10a. WPA2-Personal, and WPA3 if the AP offers it, to the owner's home network with DHCP, ping and a transfer to the Mac. The owner gave the network as a wpa_supplicant block in `build/flow/coordinator/wifi/home.conf` on the Mac (mode 600, outside git), ssid "TDA-NET", on 2.4 GHz, whose name 9b's scan confirms. Never print the passphrase or copy it into git, an issue or a log; copy the file to the trial boot only for the run and delete it from the Lenovo afterwards.
* 10b. The rate climbs past 1 Mbit/s in `iw dev wlan0 station dump`.
* 10c. A soak of hours with traffic to the Mac, judged by the kernel's counters: no hang, every reset recovered, the link back after the AP's restart.
* Done: each result in `lenovo.md`.

### 11. Transmit aggregation

Owns `ar9565/ampdu.c` and `tests/host/ar9565_ampdu_hosttest.c`. Waits on 9.

* 11a. Transmit A-MPDU with ADDBA and BlockAck status, off by a boot option.
* 11b. The host test on the model.
* Done: the host test passes; task 10's soak runs with it on.

### 12. 67's and 68's silent steps on a trial boot

Labels: `owner-only`. Waits on #2100, the fix of bwrap as root. Silent, over SSH, on a trial boot of 67's branch head, kernel and root: the installed system stays the last release, so the root is booted from the flash drive or a spare partition of the Lenovo's disk, as the `machine` skill allows, and the release is back in both slots afterwards.

* 12a. The agents' sandboxes as root and as a user (#1939), with #2100's fix.
* 12b. The agents' logins kept through a reinstall with `machine_logins.sh` (#1940), by `tests/system/accept/agents_accept.sh`, on the trial root; the machine has no `gh` sign-in yet, so that row moves to the sitting if it is still missing.
* 12c. OpenCode's acceptance without its screen (#1938's silent half).
* 12d. Doom's sound counters with the mixer at 0 at night and at 1 by day, the `-timedemo` run with nothing on the screen the owner would see (#2025's silent half).
* Done: each result recorded on its 68 issue and here.

### 14. The 32-bit build under UEFI on the Lenovo

Labels: `owner-only`. Waits on nothing. From milestone 66 (task 29's Done, `dev-log/66-terminal/plan.md:443`), which the owner's ledger found missing. It takes the screen, so it runs by day, between 09:00 and 21:00 UTC+6, with the ten-minute notice the `machine` skill gives; it stays in Backlog until 09:00. Nothing visible overnight.

* 14a. On a trial boot, the i686 kernel and root with term under UEFI on the Lenovo; record what works and what fails in `dev-log/70-wifi-chip/lenovo.md`.
* 14b. Fix what fails, each fix with its own check in QEMU under OVMF on i686.
* Done: the i686 build reaches term's login on the Lenovo under UEFI, recorded, and the release back in both slots.

### 13. The sitting

Labels: `owner-only`. Waits on every other open task of every milestone; it is the last.

* 13a. The airplane key: F7 blocks and unblocks the radio; the VPC bit it sets is recorded and the driver's bit corrected if it differs.
* 13b. The owner's university network: `wifi connect`, the certificate's subject and fingerprint shown before any password and accepted, the pin kept, a reconnection after a reboot without the question.
* 13c. eduroam, and WPA3 if task 10 could not try it.
* 13d. A demonstration run: the machine moved between networks and from the cable to Wi-Fi while a transfer runs.
* 13e. From milestone 68: cmus on the speaker (#2026), the internal mic (#2027), Doom played by hand (#2025), term's bell and flash (#1941) and OpenCode's look on the screen (#1938), each announced as the `machine` skill says; and from task 12, whatever needed the owner (`gh` and OpenCode signed in).
* Done: the owner says each step passed, and the issue records it.

## Done

The driver's host tests pass on both architectures; on the Lenovo, tasks 9 and 10 pass and the release's acceptance with them; the sitting's steps are recorded.
