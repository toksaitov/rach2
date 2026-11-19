# Multitouch event comparison

## QEMU

On 2026-11-19, QEMU 11.1.3 on the Mac ran Rach2 at `721c1ad9fc` and Debian 13 with kernel `6.12.115+deb13.1-amd64`. Both used `virtio-multitouch-pci`, the same `evtest` and `libinput debug-events` binaries, and udev's input rules. Both identified the device as a direct touchscreen with eleven slots, and udev named its PCI path. Both guests powered off after the capture.

Three QMP `input-send-event` calls supplied the same frames to each guest. The first began contact 20 in slot 0 at (4000, 5000) and contact 21 in slot 1 at (15000, 18000). The second moved contact 20 to (9000, 10000). The third sent tracking ID -1 for slots 1 and 0 in that order.

The 17 evtest event codes and values, including the three `SYN_REPORT` records, were identical in order. Libinput reported two `TOUCH_DOWN` records, one `TOUCH_MOTION`, two `TOUCH_UP` records and a `TOUCH_FRAME` after each frame. Slot numbers and coordinates matched after removing the timestamps and device node numbers.

## Host models

The new rows ran alone. Evdev read 24 tracking IDs, selected slot 23, read a second axis across two copy batches and kept the open valid through unplug. The USB descriptor model assembled a frame from two reports and kept the IDs across a change in report order. The Lenovo Synaptics model brought up the 8042 and drove IRQ 12 from two contacts through both lifts, reading the tracking IDs after each change.
