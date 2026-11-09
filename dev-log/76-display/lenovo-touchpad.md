# The touchpad on the Lenovo

Task 76-10c: the Synaptics driver of 76-10b (`rach2/sys/arch/x86/synaptics.c`) on its first trial boot on the Lenovo, silent, read over SSH. It passes when the pad comes up in absolute mode and its event device has the capabilities Debian's kernel gives the same pad. Events from a finger are the sitting's (84-1).

## The reference

Debian's psmouse logged the pad on this machine (`dev-log/70-wifi-chip/linux/dmesg.txt`):

```
psmouse serio1: synaptics: queried max coordinates: x [..5670], y [..4794]
psmouse serio1: synaptics: queried min coordinates: x [1314..], y [1098..]
psmouse serio1: synaptics: Touchpad model: 1, fw: 8.1, id: 0x1e2b1, caps: 0xd00123/0x840300/0x2a800/0x0, board id: 2848, fw id: 1581875
input: SynPS/2 Synaptics TouchPad as /devices/platform/i8042/serio1/input/input13
```

No record has Debian's `/proc/bus/input/devices` block or the axes' EVIOCGABS for it; 80-2's Debian session was asked for them on its issue. Until they are on record, the reference is what Linux's `set_input_params` and `input_mt_init_slots` give those IDs, which `tests/host/synaptics_hosttest.c` checks:

```
I: Bus=0011 Vendor=0002 Product=0007 Version=01b1
N: Name="SynPS/2 Synaptics TouchPad"
P: Phys=isa0060/serio1/input0
B: PROP=1
B: EV=b
B: KEY=e520 30000 0 0 0 0
B: ABS=660800011000003
```

with X from 1314 to 5670, Y from 1098 to 4794, pressure and MT pressure to 255, tool width to 15, two slots and tracking IDs to 65535. The log does not give the resolution, which is compared with Debian's once that is on record.

## The step

`evprobe touchpad` (`tests/evprobe.c`) finds the event device by its name, prints its block in the form of `/proc/bus/input/devices` and a line for each axis's EVIOCGABS, `value min max fuzz flat res`, and prints `EVPROBE OK` only when they are the reference's above. The step's commands, the readiness note and the manager's go are on the task's issue.

## Results

None yet: the trial has not run.
