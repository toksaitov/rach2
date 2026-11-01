# `driver/ath9k/`: the driver of Atheros' AR9003 Wi-Fi chips

This directory is the driver of Atheros' AR9003 family of Wi-Fi chips, whose one chip now is the Lenovo G50-70's, a Qualcomm Atheros QCA9565 (AR9565, PCI 168c:0036): a 2.4 GHz, one-chain 802.11b/g/n chip. The family's chips have no firmware, so the driver tunes, calibrates and runs the radio itself. It serves one card as a radio of [`../../wireless/radio.h`](../../wireless/radio.h), under the station of [`../../wireless/Readme.md`](../../wireless/Readme.md), and follows Linux's ath9k part by part. A chip is a row of the table in `chips.c`; the rest of the driver is the family's. The registers, the descriptors' fields and the EEPROM's layout are in `reg.h` and `ath9k.h`; the driver's state, the table's types and the calls between its parts are in `ath9k.h`; all register access goes through `io.h`'s indirection, with its shared helpers in `io.c`, so that each part's host test runs the part against the chip model of [`tests/host/ath9k_model.h`](../../../../../tests/host/ath9k_model.h). The parts are built as their files are written: the kernel builds `io.c`, `chips.c`, `antenna.c` and `power.c` now; `phy.c`, `calib.c` and `ani.c` are written and join the kernel's list with `pci.c`, since they call `eeprom.c` and `recover.c`; and until a part's file exists its section below is the design its interface in `ath9k.h` states.

## Sources and license

The driver follows ath9k of Linux v6.16, commit `038d61fd642278bab63ee8ef722c50d10ab01e8f`, `drivers/net/wireless/ath/ath9k/`, which is the only public description of the chips; Atheros and Qualcomm published no datasheet. Where ath9k is terse about the descriptor rings it also reads FreeBSD's AR9300 HAL and OpenBSD's `ar9003.c`.

ath9k is under the ISC license, which allows its code to be taken if its copyright and permission notice are kept. `reg.h` is ath9k's register headers (`ath/reg.h`, `reg.h`, `ar9003_mac.h`, `mac.h`, `ar9003_phy.h` and `reg_mci.h`) resolved for the chips of the table, the EEPROM's layout in `ath9k.h` is `ar9003_eeprom.h`'s, and `ar9565_1p0_initvals.h`, `ar9565_1p1_initvals.h` and `ar9003_initvals.h` are ath9k's initial-value tables of the AR9565 and the two tables of other chips it shares; all keep ath9k's notice, and `reg.h`'s opening comment lists what was changed, none of it a value. A part that takes ath9k code says which functions in its comments and keeps the notice. The rest of the driver is Rach2's, under the GNU General Public License, version 2 only (`COPYING`).

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

`ath9k_phy_process_ini` writes, at each reset, the row's initial-value tables for 2.4 GHz HT20 in ath9k's order: each stage's SOC, MAC, baseband and radio tables, then the receive and transmit gain tables and, on channel 14, Japan's CCK filter. It then holds receive off until the MAC enables it, gives a chip of the AR9462's design the regulator mode it shares with Bluetooth, sets HT20 and the chains, notes which transmit calibrations the tables enable, and sets the transmit power. `ath9k_phy_set_channel` programs the synthesizer for channels 1 to 14 in fractional mode from the 40 MHz reference, and refuses any other frequency with `-EINVAL`. `ath9k_phy_spur_mitigate` puts the OFDM spur filter and mask A on the first spur of the EEPROM's list within 10 MHz of the channel, and mask B on the next if it is near too, as ath9k does for the AR9565 (`ATH9K_CHIP_SPUR_AB`). `ath9k_phy_set_rfmode`, `ath9k_phy_set_delta_slope`, `ath9k_phy_init_bb`, `ath9k_phy_mark_inactive` and the radio bus calls are ath9k's.

`ath9k_phy_set_txpower` sets each rate's power in half dB: the EEPROM's target, at most the smaller of the caller's limit and the channel's maximum less the antenna's gain and 3 or 5 dB for a second or third chain, and at most the CCK, OFDM and HT20 conformance limits. It writes the open-loop correction and the temperature of the EEPROM's calibration piers interpolated at the channel and the EEPROM's temperature slope, from which the chip compensates the power for temperature itself, and keeps the piers' noise floor for `calib.c`. The limit stays in `phy.power_limit` for the next reset and channel change.

`ath9k_phy_channel_change` retunes a running chip without a reset when no queue holds a frame: it takes the radio bus, retunes, sets the power, delta slope and spurs, gives the bus back, and loads and restarts the noise floor. A change that fails leaves the channel and its calibration data as they were, and the caller resets the chip.

A reset of `hw.c` calls the radio's parts in ath9k's order: `ath9k_nf_read` before the chip's reset, to keep the last measurement; then `ath9k_cal_set_channel`, `ath9k_phy_process_ini`, `ath9k_phy_set_rfmode`, `ath9k_phy_set_delta_slope`, `ath9k_phy_spur_mitigate`, `ath9k_phy_set_channel`, `ath9k_ani_reset`, `ath9k_phy_init_bb`, `ath9k_cal_init`, `ath9k_nf_load` and `ath9k_nf_start`; and `ath9k_cal_start` once the chip runs.

### Calibration (`calib.c`)

`ath9k_cal_init` runs the reset-time calibrations as ath9k's `ar9003_hw_init_cal_pcoem` does on a chip without RTT and with coexistence off: the AGC's offset calibration, with the TX IQ calibration in it and the carrier leak calibration around it unless the channel's results are kept, then the manual peak calibration of each chain, and the RX IQ calibration started over. An offset calibration that does not finish in 100 ms is logged and returns `-EIO`. `ath9k_cal_set_channel` forgets the kept results and the noise floor's history when the channel is new. The AR9565 1.1's RTT is not used: the G50-70's chip is a 1.0.1, and without RTT the 1.1 calibrates as the 1.0 does.

The noise floor is measured per chain into a history of five readings: a new history takes its readings whole and then their median, and a reading outside the chip's bounds is corrected. `ath9k_nf_read` takes a finished measurement and reports the floor and the noise level in `cal.noise_floor` and `cal.noise`. `ath9k_nf_load` loads each median back, waits up to 22.2 ms for the chip to take it, and releases the registers to -50 dBm; `ath9k_nf_bstuck` takes stuck beacons for interference and stops capping the floor at its maximum until a reading is normal again.

`ath9k_cal_start` starts the periodic work on the wireless worker, as ath9k's `ath_ani_calibrate` does: ANI every second; the RX IQ calibration every second until it is done, and again 20 minutes after; and the long calibration, which reads, loads and restarts the noise floor, every 30 seconds, or every second while interference keeps the floor uncapped. A load the chip does not take asks `recover.c` for a reset (`ATH9K_RESET_CAL`, as ath9k's 7b8aaead958e does) and ends the work until the reset starts it again. On a chip with `ATH9K_CHIP_IQ_BOUND`, the AR9565, an RX IQ coefficient at either bound leaves the correction as it was, since applying it drops the best receive rate to MCS 4 (ath9k's 6f37ff96d3bd). The thermometer's latest reading is kept in `cal.temp`; the compensation is the chip's, from what `phy.c` writes. The work takes turns between two records, since a record cannot be queued while its callback runs.

### Noise immunity (`ani.c`)

`ath9k_ani_reset`, called in each reset after the tables, reads the tables' settings of the fields it moves and sets the levels: OFDM 3 and CCK 2 the first time, which are the tables' own settings, and after that the higher of those and the levels held. `ath9k_ani_monitor`, which the periodic work calls each second, reads the MIB's cycle and phy error counters, and once the receiver has listened for 300 ms raises OFDM's level for more than 3500 OFDM errors a second (1000 below the default level), raises CCK's for more than 600 CCK errors, or lowers one of the two in turn when both rates are low. A level sets the spur immunity's cycle power threshold, the FIR step and the detection of weak OFDM signals, which stays on while the beacons' RSSI in `ani.beacon_rssi` is 40 or less; `recv.c` keeps it, and until it does it reads 0. `rach2.ath9k_noani` on the boot line sets `ATH9K_OPT_NO_ANI`, which leaves the levels at their defaults, since a rate stuck at 1 Mbit/s has been reported with ANI on.

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

`ath9k_ant_init` chooses diversity from `ATH9K_CHIP_ANT_COMB`, the EEPROM and the per-card flags. The chip row may override the antenna-control byte: the AR9565 uses `0xc9`, as ath9k does even when its EEPROM byte is zero. A known one- or two-antenna card without `ATH9K_PCI_BT_ANT_DIV` has combining disabled. The Lenovo's two-antenna row enables it. The row also holds the bias for each pair of LNA configurations and the threshold for switching the main LNA, 3 RSSI units for the AR9565.

`ath9k_ant_set` restores the board's common, chain and SPDT switch tables at reset. It gives WLAN the SPDT switch, disables MCI arbitration and both MCI interrupt masks, and sets LNA diversity, receive protection, fast CCK diversity and the restart's diversity flag. Bluetooth is unused. A reset starts a fresh antenna scan on LNA1 and LNA2.

`ath9k_ant_rx` collects 512 valid main/alternate RSSI samples per window, ignoring failed frames, absent signals and status from an earlier configuration. It waits for an aggregate's end before changing a pair, with the sample count and sums bounded while it waits. It compares LNA1 and LNA2, their sum and their difference in three windows, chooses the main physical LNA and the best alternate, and scans again after 16 windows. This is a packet-based scan using ath9k's configurations and bias table; it does not implement ath9k's short scans.

### Power and the radio switch (`power.c`)

`ath9k_power_init` leaves the PCIe link as firmware set it. `rach2.ath9k_noaspm`, or `ATH9K_OPT_NO_ASPM`, clears only L0s and L1 in the upstream port's and the card's link control words, the first thing to try if the card drops off the bus. It skips absent, truncated and unreadable capabilities. The saved `power.aspm` is the card's firmware setting; the diagnostic option stays in force after resume.

When the EEPROM enables RF silence, `ath9k_power_init` makes its GPIO an input. `ath9k_rfkill_poll` restores the GPIO input direction if a reset changed it, compares the GPIO to the EEPROM's polarity and reports a hard block through `wlan_radio_rfkill`. A GPIO outside the chip's range or an unreadable card is blocked. The first sample is reported after radio registration; a later identical sample emits nothing. The bus part must poll after registration and periodically while the radio is stopped as well as while it runs, so a hard block can clear.

`suspend_to_idle` calls `ath9k_suspend_all` before freezing processes and `ath9k_resume_all` after thawing them, on a wake or a signal. The one fixed card is borrowed by these hooks at attach. `ath9k_suspend` stops the station through `wlan_stop`, clearing association and pending transmission, and remembers whether the station ran or awaited rfkill. Its start operation must refuse a start while `power.suspended` is set. Resume samples the switch again and restarts only a wanted, unblocked station; a block defers restart to the station's rfkill worker. A start error is logged and leaves the restart request set for a later `ath9k_resume` call. Neither step preserves an association.

### Transmit aggregation (`ampdu.c`)

Transmit A-MPDU with ADDBA and the BlockAck status, by TID, off under `ATH9K_OPT_NO_AMPDU`.

## Tests

`ath9k_ant_hosttest.c` runs the antenna and power parts on the chip model with PCIe and station models, and the real `proc/suspend.c` on a modeled scheduler wait. It checks the full switch fields and preserved bits, WLAN ownership and disabled coexistence, the per-card flags, LNA selection and rescans, stale and invalid receive metadata, a long aggregate, firmware ASPM preserved and disabled at both ends, missing and unreadable capabilities, RF-silence polarity and GPIO bounds, deferred initial rfkill reporting, suspend and repeated calls, a wake during preparation, a signal, a resume error and retry, and blocks raised while asleep.

`ath9k_phy_hosttest.c` runs the radio, calibration and noise immunity parts on the chip model, with the real wireless worker's queue and the EEPROM's targets and limits and the reset request as stubs. It checks the synthesizer's words for channels 1 to 14, the tables' order of writes and the overrides after them, the delta slope, both spur masks and the rule at 2437 MHz, each power cap, the piers' interpolation and a negative correction kept in its field, the order of the reset-time calibrations, the peak detector's search and a stuck offset calibration, the noise floor's history, median, load and release, a load that times out and the reset it asks for, the RX IQ correction and its skip at either bound, the periodic work's schedule and restart, the ANI levels' moves and the fields they write, and a channel change refused with a frame queued, without the radio bus or outside the band.

Each part's host test, `tests/host/ath9k_*_hosttest.c`, runs the part against the chip model of `tests/host/ath9k_model.c`, a chip of a row of the table, which keeps the register file, logs the driver's accesses in order, answers the registers ath9k's sequences poll, and fills receive buffers and the transmit status ring through a DMA hook ([`tests/host/Readme.md`](../../../../../tests/host/Readme.md)). The model is written from ath9k, so a chip that behaves otherwise passes the tests and fails on the Lenovo.
