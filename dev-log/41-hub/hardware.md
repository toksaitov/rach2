# Milestone 41 hardware

What this milestone measures about the machines it runs on. The Lenovo's
column is filled in milestone 43; nothing here touches it.

## What milestone 43 needs

* A USB 2.0 or 3.0 hub. A cheap unpowered one is the harder and the better
  test: it switches port power in the ganged mode and has one transaction
  translator for all its ports. A USB 3.0 hub also shows the SuperSpeed half
  being refused and the high-speed half carrying the devices.
* A low-speed keyboard to put behind it, which is the one case the
  transaction translator fails loudly on if its slot-context fields are
  wrong.
* A full-speed device to put behind it, such as a mouse; and the stick from
  milestone 40 behind it as the high-speed device.
* Optionally a second hub, to put one behind the other.

## The controller's tables

Eight slots before this milestone, twelve after it, from the notebook's own
inventory with headroom. The scratchpad is the controller's demand and did
not change.

| Table | Before | After |
|-------|--------|-------|
| Device context base array | 72 | 104 |
| Device contexts | 18432 | 26624 |
| Input context | 2112 | 2112 |
| Endpoint rings | 73728 | 106496 |
| Command and event rings | 8192 | 8192 |
| Total | 102536 | 143528 |

## The device table

| Machine | Soldered devices refused | Hubs | Keyboard, mouse, stick | Total |
|---------|--------------------------|------|------------------------|-------|
| Lenovo G50-70 | 4 | a USB 3 hub is 2 entries | 3 | 9 of 12 |
| QEMU | 0 | 1 | 3 | 4 of 12 |

## Bring-up

| Machine | The hub itself | A keyboard behind it | A stick behind it |
|---------|----------------|----------------------|-------------------|
| QEMU | 0.31 s | 0.68 s | 0.83 s ready |
| Lenovo G50-70 | 0.95 s, most of it a soldered device on port 7 that never answered for its configuration | 0.17 s after the hub, the mouse 0.13 s | milestone 43, phase 4 |

Each device behind a hub is a few dozen control transfers through the one
command turn; the times are from the boot's records.

## The owner's hub

35d6:2510, four ports, one transaction translator, think time 1, power
switching declared ganged, power good declared 0 ms. After one power
request it reported port 1 powered and ports 2 to 4 not, which is the
Powered-off state the specification leaves a port in until it has a
request of its own; the driver now powers every port itself and waits at
least 100 ms. Behind it on the Lenovo a low-speed keyboard and a
low-speed mouse enumerate at every port, type and move at the console
and in Doom, and come and go when moved between ports.

## The tree under QEMU

`usb-hub` on `qemu-xhci` is a full-speed hub with eight ports; devices
behind it run at full or low speed, and there is no transaction translator.
