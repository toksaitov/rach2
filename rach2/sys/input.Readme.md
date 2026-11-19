# Input devices

The input core gives applications Linux's event devices under `/dev/input`, and publishes their identity and capabilities in `/sys/class/input`. `evdev.c` keeps each device's state and each open's event queue. `input.c` receives keyboard events for the console, and `mouse.c` supplies the console's mouse device. [input(4)](../../man/man4/input.4) describes the event records and ioctls.

## Interfaces

Drivers set the per-contact absolute axes before calling `evdev_init_slots`, the equivalent of Linux's `input_mt_init_slots`. Each slot starts inactive and keeps a copy of every multitouch axis. A report selects a slot with `ABS_MT_SLOT`; evdev emits that selector when the first changed value for the slot needs it. `ABS_MT_TRACKING_ID` identifies the contact and -1 lifts it. `EVIOCGMTSLOTS` reads one axis across the slots, including inactive slots. `INPUT_MT_POINTER` sets the pointer property and the supported finger-count keys. `INPUT_MT_DIRECT` sets the direct property for a touchscreen. Both enable the single-touch compatibility axes when their per-contact counterparts are present.

## Design

The [PS/2 drivers](arch/x86/Readme.md) for Synaptics, ALPS and Elantech make type B reports on pads that provide contact positions. On a semi-multitouch pad, its two positions give the bounds of the contacts. The Lenovo's Synaptics image sensor gives two positions and counts up to five fingers.

The [USB HID driver](usb/Readme.md) uses a contact identifier to find the same slot in successive reports. The [virtio input driver](virtio/Readme.md#input) receives the slot numbers and tracking IDs directly from the device.
