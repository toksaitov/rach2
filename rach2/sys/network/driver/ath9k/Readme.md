# `driver/ath9k/`: the driver of Atheros' AR9003 Wi-Fi chips

This directory is the driver of Atheros' AR9003 family of Wi-Fi chips, whose one chip now is the Lenovo G50-70's, a Qualcomm Atheros QCA9565 (AR9565, PCI 168c:0036): a 2.4 GHz, one-chain 802.11b/g/n chip. The family's chips have no firmware, so the driver tunes, calibrates and runs the radio itself. It serves one card as a radio of [`../../wireless/radio.h`](../../wireless/radio.h), under the station of [`../../wireless/Readme.md`](../../wireless/Readme.md), and follows Linux's ath9k part by part. A chip is a row of the table in `chips.c`; the rest of the driver is the family's. The registers, the descriptors' fields and the EEPROM's layout are in `reg.h` and `ath9k.h`; the driver's state, the table's types and the calls between its parts are in `ath9k.h`; all register access goes through `io.h`'s indirection, with its shared helpers in `io.c`, so that each part's host test runs the part against the chip model of [`tests/host/ath9k_model.h`](../../../../../tests/host/ath9k_model.h). The parts are built as their files are written: the kernel builds `io.c` and `chips.c` now, and until a part's file exists its section below is the design its interface in `ath9k.h` states.

## Sources and license

The driver follows ath9k of Linux v6.16, commit `038d61fd642278bab63ee8ef722c50d10ab01e8f`, `drivers/net/wireless/ath/ath9k/`, which is the only public description of the chips; Atheros and Qualcomm published no datasheet. Where ath9k is terse about the descriptor rings it also reads FreeBSD's AR9300 HAL and OpenBSD's `ar9003.c`.

ath9k is under the ISC license, which allows its code to be taken if its copyright and permission notice are kept. `reg.h` is ath9k's register headers (`ath/reg.h`, `reg.h`, `ar9003_mac.h`, `mac.h` and `ar9003_phy.h`) resolved for the chips of the table, the EEPROM's layout in `ath9k.h` is `ar9003_eeprom.h`'s, and `ar9565_1p0_initvals.h`, `ar9565_1p1_initvals.h` and `ar9003_initvals.h` are ath9k's initial-value tables of the AR9565 and the two tables of other chips it shares; all keep ath9k's notice, and `reg.h`'s opening comment lists what was changed, none of it a value. A part that takes ath9k code says which functions in its comments and keeps the notice. The rest of the driver is Rach2's, under the GNU General Public License, version 2 only (`COPYING`).

## Design

The driver has ath9k's split, and each part owns a member of `struct ath9k`, named after it, and the calls declared under its file's name in `ath9k.h`. Every call runs with the kernel lock held, either on the wireless worker, which may sleep, or in the card's interrupt handler or the tick that stands in for it, which may not; `ath9k.h` says which for each.

### The table of chips (`chips.c`)

A chip is a row, a `struct ath9k_chip`: its name and PCI device ID, the MAC version its `AR_SREV` reads and the revisions the row knows, what it has as `ATH9K_CHIP_*` bits (the bands, LNA combining, Bluetooth coexistence by MCI, and two groupings ath9k tests by chip: the parts made for PC makers and those of the AR9462's design), its chain mask and GPIO lines, ath9k's per-card flags by subsystem ID, and one ops struct for the two steps a chip does its own way: choosing its initial-value tables by its revision and its gain tables by the EEPROM's gain modes, as ath9k's `ar9003_hw.c` does for it. `ath9k_chip_find`, `ath9k_chip_rev` and `ath9k_chip_flags` look a card up. The AR9565 is the one row: revisions 1.0, 1.0.1 and 1.1, of which 1.1 has its own tables; 2.4 GHz, one chain, LNA combining, MCI, and 14 GPIO lines; and the 39 cards of ath9k's table, the G50-70's 17aa:3026 and 17aa:4026 among them with `ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV`, any other card with `ATH9K_PCI_BT_ANT_DIV`.

Adding a chip, the AR9462, AR9485 or AR9580, is a row, its initvals and its IDs, and wherever ath9k tests for the chip in code the family shares, a test of the row's caps. `reg.h`'s registers are resolved for the AR9565; where the new chip's differ, as ath9k's `AR_SREV` test at the macro shows, the macro becomes a test of the row again.

### Register access

`io.h` gives every part ath9k's `REG_READ`, `REG_WRITE`, `REG_RMW`, `REG_SET_BIT`, `REG_CLR_BIT`, `REG_RMW_FIELD` and `REG_READ_FIELD`, and `SM` and `MS`, over a `struct ath9k_io` of read, write and wait functions that the card's state points at. A field is a mask `NAME` and its shift `NAME_S`, as in ath9k. `io.c` has the three helpers every part shares: `ath9k_rmw`, `ath9k_wait`, which polls a register every 10 microseconds up to a bound and returns `-ETIMEDOUT` without logging, and `ath9k_write_ini`, which writes a column of one of ath9k's initial-value tables. In the kernel the functions are `pci.c`'s `ath9k_mmio_io`, MMIO on the card's window and `k_udelay`; in a host test they are the model's.

### The bus (`pci.c`)

`ath9k_probe` finds a card of 168c and a device ID of the table, maps its window, turns on bus mastering with 32-bit DMA, and takes the card's flags from the row by its subsystem IDs. It attaches the parts and registers the radio with `ath9k_radio_ops`, and logs `ath9k: probe: BUS:DEV.FN AR9565 rev 1.N mac XX:... subsys 17aa:NNNN`.

### Power and reset (`hw.c`)

`ath9k_hw_attach` follows `ath9k_hw_init`: the RTC's power-on reset and force-wake, the version and revision from `AR_SREV`, which must be the row's, the initial-value tables the row's ops choose for the revision, the EEPROM, and the gain tables the ops choose for the EEPROM's gain modes. A card that reads all ones is refused with the register named. `ath9k_hw_reset` follows ath9k's function of that name.

### The EEPROM (`eeprom.c`)

The card keeps its calibration in an EEPROM or in OTP as compressed blocks laid over one of ath9k's board templates. `ath9k_eeprom_init` finds and checks the image, unpacks it into `struct ar9300_eeprom`, whose 1088-byte layout the build checks, and takes the MAC address, the regulatory domain and the country from it. It answers the target power of each rate, the conformance limits, the antenna tables, and writes the board's settings at each reset.

### The radio (`phy.c`)

`ath9k_phy_process_ini` writes the mode, band and gain tables for 2.4 GHz HT20 at each reset and channel change, `ath9k_phy_set_channel` programs the synthesizer for channels 1 to 13, and `ath9k_phy_set_txpower` sets each rate's power from the EEPROM, capped by the regulatory domain and the conformance limits.

### Calibration (`calib.c`)

The reset-time calibrations of `ar9003_hw_init_cal`: ADC and DC offsets, IQ mismatch, which skips an update whose offset is too high, and carrier leak. The noise floor is measured and kept in a history of five per reading, whose median is loaded back; a failed load asks for a reset. The periodic calibration and temperature compensation run on the worker every second, with a long calibration every 30 seconds and a full one every 20 minutes.

### Noise immunity (`ani.c`)

Adaptive noise immunity raises and lowers the OFDM and CCK immunity levels from the MIB's phy error counts each second, as ath9k's `ani.c` does. The boot option behind `ATH9K_OPT_NO_ANI` leaves it at its defaults, since a rate stuck at 1 Mbit/s has been reported with it on.

### Queues and DMA (`mac.c`)

The register-level work of the queues and the PCU, ath9k's `mac.c` and `ar9003_mac.c`: each access category's QCU and DCU, pushes to the transmit FIFOs and the receive FIFOs, stopping and aborting DMA, the receive filter, the station's and the BSSID's addresses, and the interrupt mask.

### Receive (`recv.c`)

The AR9003's EDMA receive: a high-priority FIFO of 16 buffers and a low-priority one of 128, each buffer 3904 bytes with the chip's 48-byte status at its head. `ath9k_rx_service` hands each completed frame to `wlan_radio_rx` and gives the buffer back.

### Transmit (`xmit.c`)

One queue per access category, a ring of 64 frames of which at most 8 are in the chip's FIFO, descriptors with four rate and retry series, and a status ring of 512 entries that `ath9k_tx_service` reads to report each frame with `wlan_radio_tx_status`, which rate control reads.

### Keys (`key.c`)

The 128-entry key cache: TKIP with its MIC keys, CCMP, and unicast management frames in hardware; any other cipher goes to software. The driver keeps a copy of each entry and writes the cache back after every reset.

### Interrupts (`intr.c`)

A message-signalled interrupt where the machine can route one, and the tick servicing the rings when no message arrives, as the RTL8168's driver does. `ath9k_intr` serves receive, transmit, beacon miss and the fatal causes, which ask for a reset.

### Recovery (`recover.c`)

The hang checks of ath9k for the AR9003: the baseband watchdog and a baseband or MAC that has stopped, a queue that holds a frame too long, and a receive path silent for too long. `ath9k_reset` resets the chip on its channel and restores the keys, the filter, the BSSID, the queues and receive, and counts the reason in `struct ath9k_stats`.

### Antennas (`antenna.c`)

LNA combining and antenna diversity on a chip whose row has `ATH9K_CHIP_ANT_COMB`, as the AR9565's does, chosen from the per-card flags and the EEPROM, and the antenna switch set for WLAN with Bluetooth unused and coexistence off.

### Power and the radio switch (`power.c`)

The link's ASPM is left as the firmware set it; the boot option behind `ATH9K_OPT_NO_ASPM` turns L0s and L1 off, the first thing to try if the card drops off the bus. When the EEPROM enables RF silence, its GPIO line is the hard rfkill block. The radio stops and starts around suspend to idle.

### Transmit aggregation (`ampdu.c`)

Transmit A-MPDU with ADDBA and the BlockAck status, by TID, off under `ATH9K_OPT_NO_AMPDU`.

## Tests

Each part's host test, `tests/host/ath9k_*_hosttest.c`, runs the part against the chip model of `tests/host/ath9k_model.c`, a chip of a row of the table, which keeps the register file, logs the driver's accesses in order, answers the registers ath9k's sequences poll, and fills receive buffers and the transmit status ring through a DMA hook ([`tests/host/Readme.md`](../../../../../tests/host/Readme.md)). The model is written from ath9k, so a chip that behaves otherwise passes the tests and fails on the Lenovo.
