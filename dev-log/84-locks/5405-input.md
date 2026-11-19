# Multitouch event comparison

## QEMU

On 2026-11-19, QEMU 11.1.3 on the Mac ran Rach2 at `721c1ad9fc` and Debian 13 with kernel `6.12.115+deb13.1-amd64`. Both used `virtio-multitouch-pci`, the same `evtest` and `libinput debug-events` binaries, and udev's input rules. Both identified the device as a direct touchscreen with eleven slots, and udev named its PCI path. Both guests powered off after the capture.

Three QMP `input-send-event` calls supplied the same frames to each guest. The first began contact 20 in slot 0 at (4000, 5000) and contact 21 in slot 1 at (15000, 18000). The second moved contact 20 to (9000, 10000). The third sent tracking ID -1 for slots 1 and 0 in that order.

The 17 evtest event codes and values, including the three `SYN_REPORT` records, were identical in order. Libinput reported two `TOUCH_DOWN` records, one `TOUCH_MOTION`, two `TOUCH_UP` records and a `TOUCH_FRAME` after each frame. Slot numbers and coordinates matched after removing the timestamps and device node numbers.

The comparison also passed at `f4b515d33f`. On that boot the single-touch tablet reported ABS_X 8000 and ABS_Y 10000. Unbinding the multitouch device removed its node and a read on a retained descriptor returned `ENODEV`. The node appeared again after a bind.

## Host models

The new rows ran alone. Evdev read 24 tracking IDs, selected slot 23, read a second axis across two copy batches and returned `ENODEV` to the existing open after unplug. The USB descriptor model assembled a frame from two reports and kept the IDs across a change in report order. The Lenovo Synaptics model brought up the 8042 and drove IRQ 12 from two contacts through both lifts, reading the tracking IDs after each change.

The USB model reads a reported capacity of 16, sets the Input Mode field alone, and withholds the touch node on a short shared feature read or a refused mode write. Contact rows cover repeated records in the same Finger collection, two explicit lifts with separate selectors, and an implicit lift on a device that lists active contacts. A report without button fields preserves the previous click. Linux 6.12's `input_mt_report_pointer_emulation` counts an in-range hover slot for `BTN_TOUCH`, while `ABS_MT_DISTANCE` carries the hover state.
