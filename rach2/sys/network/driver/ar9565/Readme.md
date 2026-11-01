# `driver/ar9565/`: the QCA9565 Wi-Fi driver

This directory is the driver of the Lenovo G50-70's Wi-Fi card, a Qualcomm Atheros QCA9565 (AR9565, PCI 168c:0036): a 2.4 GHz, one-antenna-chain 802.11b/g/n chip with no firmware, so the driver tunes, calibrates and runs the radio itself. It serves one card as a radio of [`../../wireless/radio.h`](../../wireless/radio.h), under the station of [`../../wireless/Readme.md`](../../wireless/Readme.md), and follows Linux's ath9k part by part. The registers, the descriptors' fields and the EEPROM's layout are in `reg.h` and `ar9565.h`; the driver's state and the calls between its parts are in `ar9565.h`; all register access goes through `io.h`'s indirection, with its shared helpers in `io.c`, so that each part's host test runs the part against the chip model of [`tests/host/ar9565_model.h`](../../../../../tests/host/ar9565_model.h). The parts are built as their files are written: the kernel builds `io.c` now, and until a part's file exists its section below is the design its interface in `ar9565.h` states.

## Sources and license

The driver follows ath9k of Linux v6.16, commit `038d61fd642278bab63ee8ef722c50d10ab01e8f`, `drivers/net/wireless/ath/ath9k/`, which is the only public description of the chip; Qualcomm published no datasheet. Where ath9k is terse about the descriptor rings it also reads FreeBSD's AR9300 HAL and OpenBSD's `ar9003.c`.

ath9k is under the ISC license, which allows its code to be taken if its copyright and permission notice are kept. `reg.h` is ath9k's register headers (`ath/reg.h`, `reg.h`, `ar9003_mac.h`, `mac.h` and `ar9003_phy.h`) resolved for the AR9565, and the EEPROM's layout in `ar9565.h` is `ar9003_eeprom.h`'s; both keep ath9k's notice, and `reg.h`'s opening comment lists what was changed, none of it a value. A part that takes ath9k code says which functions in its comments and keeps the notice. The rest of the driver is Rach2's, under the GNU General Public License, version 2 only (`COPYING`).

## Design

The driver has ath9k's split, and each part owns a member of `struct ar9565`, named after it, and the calls declared under its file's name in `ar9565.h`. Every call runs with the kernel lock held, either on the wireless worker, which may sleep, or in the card's interrupt handler or the tick that stands in for it, which may not; `ar9565.h` says which for each.

### Register access

`io.h` gives every part ath9k's `REG_READ`, `REG_WRITE`, `REG_RMW`, `REG_SET_BIT`, `REG_CLR_BIT`, `REG_RMW_FIELD` and `REG_READ_FIELD`, and `SM` and `MS`, over a `struct ar9565_io` of read, write and wait functions that the card's state points at. A field is a mask `NAME` and its shift `NAME_S`, as in ath9k. `io.c` has the three helpers every part shares: `ar9565_rmw`, `ar9565_wait`, which polls a register every 10 microseconds up to a bound and returns `-ETIMEDOUT` without logging, and `ar9565_write_ini`, which writes a column of one of ath9k's initial-value tables. In the kernel the functions are `pci.c`'s `ar9565_mmio_io`, MMIO on the card's window and `k_udelay`; in a host test they are the model's.

### The bus (`pci.c`)

`ar9565_probe` finds 168c:0036, maps its window, turns on bus mastering with 32-bit DMA, and takes ath9k's per-card flags from the subsystem IDs: 17aa:3026 and 17aa:4026, the G50-70's, are `ATH9K_PCI_AR9565_2ANT | ATH9K_PCI_BT_ANT_DIV`. It attaches the parts and registers the radio with `ar9565_radio_ops`, and logs `ar9565: probe: BUS:DEV.FN rev 1.N mac XX:... subsys 17aa:NNNN`.

### Power and reset (`hw.c`)

`ar9565_hw_attach` follows `ath9k_hw_init`: the RTC's power-on reset and force-wake, the revision from `AR_SREV` (1.0, 1.0.1 or 1.1), the initial-value tables that revision selects from ath9k's `ar9565_1p0_initvals.h` and `ar9565_1p1_initvals.h`, and the EEPROM. A card that reads all ones is refused with the register named. `ar9565_hw_reset` follows `ath9k_hw_reset`.

### The EEPROM (`eeprom.c`)

The card keeps its calibration in an EEPROM or in OTP as compressed blocks laid over one of ath9k's board templates. `ar9565_eeprom_init` finds and checks the image, unpacks it into `struct ar9300_eeprom`, whose 1088-byte layout the build checks, and takes the MAC address, the regulatory domain and the country from it. It answers the target power of each rate, the conformance limits, the antenna tables, and writes the board's settings at each reset.

### The radio (`phy.c`)

`ar9565_phy_process_ini` writes the mode, band and gain tables for 2.4 GHz HT20 at each reset and channel change, `ar9565_phy_set_channel` programs the synthesizer for channels 1 to 13, and `ar9565_phy_set_txpower` sets each rate's power from the EEPROM, capped by the regulatory domain and the conformance limits.

### Calibration (`calib.c`)

The reset-time calibrations of `ar9003_hw_init_cal`: ADC and DC offsets, IQ mismatch, which skips an update whose offset is too high, and carrier leak. The noise floor is measured and kept in a history of five per reading, whose median is loaded back; a failed load asks for a reset. The periodic calibration and temperature compensation run on the worker every second, with a long calibration every 30 seconds and a full one every 20 minutes.

### Noise immunity (`ani.c`)

Adaptive noise immunity raises and lowers the OFDM and CCK immunity levels from the MIB's phy error counts each second, as ath9k's `ani.c` does. The boot option behind `AR9565_OPT_NO_ANI` leaves it at its defaults, since a rate stuck at 1 Mbit/s has been reported with it on.

### Queues and DMA (`mac.c`)

The register-level work of the queues and the PCU, ath9k's `mac.c` and `ar9003_mac.c`: each access category's QCU and DCU, pushes to the transmit FIFOs and the receive FIFOs, stopping and aborting DMA, the receive filter, the station's and the BSSID's addresses, and the interrupt mask.

### Receive (`recv.c`)

The AR9003's EDMA receive: a high-priority FIFO of 16 buffers and a low-priority one of 128, each buffer 3904 bytes with the chip's 48-byte status at its head. `ar9565_rx_service` hands each completed frame to `wlan_radio_rx` and gives the buffer back.

### Transmit (`xmit.c`)

One queue per access category, a ring of 64 frames of which at most 8 are in the chip's FIFO, descriptors with four rate and retry series, and a status ring of 512 entries that `ar9565_tx_service` reads to report each frame with `wlan_radio_tx_status`, which rate control reads.

### Keys (`key.c`)

The 128-entry key cache: TKIP with its MIC keys, CCMP, and unicast management frames in hardware; any other cipher goes to software. The driver keeps a copy of each entry and writes the cache back after every reset.

### Interrupts (`intr.c`)

A message-signalled interrupt where the machine can route one, and the tick servicing the rings when no message arrives, as the RTL8168's driver does. `ar9565_intr` serves receive, transmit, beacon miss and the fatal causes, which ask for a reset.

### Recovery (`recover.c`)

The hang checks of ath9k for the AR9003: the baseband watchdog and a baseband or MAC that has stopped, a queue that holds a frame too long, and a receive path silent for too long. `ar9565_reset` resets the chip on its channel and restores the keys, the filter, the BSSID, the queues and receive, and counts the reason in `struct ar9565_stats`.

### Antennas (`antenna.c`)

The AR9565's LNA combining and antenna diversity, chosen from the per-card flags and the EEPROM, and the antenna switch set for WLAN with Bluetooth unused and coexistence off.

### Power and the radio switch (`power.c`)

The link's ASPM is left as the firmware set it; the boot option behind `AR9565_OPT_NO_ASPM` turns L0s and L1 off, the first thing to try if the card drops off the bus. When the EEPROM enables RF silence, its GPIO line is the hard rfkill block. The radio stops and starts around suspend to idle.

### Transmit aggregation (`ampdu.c`)

Transmit A-MPDU with ADDBA and the BlockAck status, by TID, off under `AR9565_OPT_NO_AMPDU`.

## Tests

Each part's host test, `tests/host/ar9565_*_hosttest.c`, runs the part against the chip model of `tests/host/ar9565_model.c`, which keeps the register file, logs the driver's accesses in order, answers the registers ath9k's sequences poll, and fills receive buffers and the transmit status ring through a DMA hook ([`tests/host/Readme.md`](../../../../../tests/host/Readme.md)). The model is written from ath9k, so a chip that behaves otherwise passes the tests and fails on the Lenovo.
