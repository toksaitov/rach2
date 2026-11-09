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

Linux's `set_input_params` and `input_mt_init_slots` give those IDs the block below, which `tests/host/synaptics_hosttest.c` checks, and Debian's own reading on this machine, in Results, is the same:

```
I: Bus=0011 Vendor=0002 Product=0007 Version=01b1
N: Name="SynPS/2 Synaptics TouchPad"
P: Phys=isa0060/serio1/input0
B: PROP=1
B: EV=b
B: KEY=e520 30000 0 0 0 0
B: ABS=660800011000003
```

with X from 1314 to 5670, Y from 1098 to 4794, pressure and MT pressure to 255, tool width to 15, two slots and tracking IDs to 65535. The log does not give the resolution; Debian's EVIOCGABS gives 44 units/mm for X and 76 for Y.

## The step

`evprobe touchpad` (`tests/evprobe.c`) finds the event device by its name, prints its block in the form of `/proc/bus/input/devices` and a line for each axis's EVIOCGABS in the form `touchpad.sh` prints Debian's, and prints `EVPROBE OK` only when they are the reference's above. The two outputs are compared line for line, less each axis's value, which is only the pad's last reading. The step's commands, the readiness note and the manager's go are on the task's issue.

## Results

Run on 2026-11-10 from 02:46 to 02:52 UTC+6, from the Mac by the owner's loop under the Lenovo's lock. It passed: the pad came up in absolute mode on the trial boot, and every line of its capabilities equals Debian's.

* The build: the Mac's build of `task/2643` at 733a1a412 merged with `milestone/76-display` at eb9b22408, which holds 81-6d's commit that leaves the VT-d units unwritten under `intel_iommu=off intremap=off` (e8b470dc2). The kernel is 3,338,744 bytes with SHA-256 0251b435ff1b64274de48ff551c363b819eba12955500cdd47e8a17f37064145; `evprobe` 26,440 bytes with SHA-256 b466e676147818145f66332e9c753def707ae898a30efd23a446beee1e03a9d5.
* Before: the release of milestone 12 (bb151e3ba) booted from the candidate slot with the trial consumed, and `machine_identity.sh` found its kernel, 2,845,120 bytes with SHA-256 0effe5ac..., in both slots.

### Debian's reference

`mac.sh arm` saved p1 with SHA-256 33829ae85d4283cd9076f03d8267727481e2f9b2caa7bdc776f52c4c91305193, wrote 9 runs of Debian's files, staged the menu and armed one boot at 02:47:19; Debian answered SSH 26 s later. `touchpad.sh` read, whole:

```
== kernel 6.12.115+deb13.1-amd64
== /proc/bus/input/devices
I: Bus=0011 Vendor=0002 Product=0007 Version=01b1
N: Name="SynPS/2 Synaptics TouchPad"
P: Phys=isa0060/serio1/input0
S: Sysfs=/devices/platform/i8042/serio1/input/input2
U: Uniq=
H: Handlers=mouse0 event1 
B: PROP=1
B: EV=b
B: KEY=e520 30000 0 0 0 0
B: ABS=660800011000003

== kernel log
[    2.232264] psmouse serio1: synaptics: queried max coordinates: x [..5670], y [..4794]
[    2.280900] psmouse serio1: synaptics: queried min coordinates: x [1314..], y [1098..]
[    2.380612] psmouse serio1: synaptics: Touchpad model: 1, fw: 8.1, id: 0x1e2b1, caps: 0xd00123/0x840300/0x2a800/0x0, board id: 2848, fw id: 1581875
[    2.444369] input: SynPS/2 Synaptics TouchPad as /devices/platform/i8042/serio1/input/input2
== EVIOCGABS
EVPROBE: abs 0x0 min 1314 max 5670 fuzz 0 flat 0 res 44 value 0
EVPROBE: abs 0x1 min 1098 max 4794 fuzz 0 flat 0 res 76 value 0
EVPROBE: abs 0x18 min 0 max 255 fuzz 0 flat 0 res 0 value 0
EVPROBE: abs 0x1c min 0 max 15 fuzz 0 flat 0 res 0 value 0
EVPROBE: abs 0x2f min 0 max 1 fuzz 0 flat 0 res 0 value 0
EVPROBE: abs 0x35 min 1314 max 5670 fuzz 0 flat 0 res 44 value 0
EVPROBE: abs 0x36 min 1098 max 4794 fuzz 0 flat 0 res 76 value 0
EVPROBE: abs 0x39 min 0 max 65535 fuzz 0 flat 0 res 0 value 0
EVPROBE: abs 0x3a min 0 max 255 fuzz 0 flat 0 res 0 value 0
```

Debian left by `leave.sh`'s `rtcwake -m off -s 120` at 02:47:57, and Rach2 answered at 02:50:20. `mac.sh restore` wrote 10 runs back, and p1 hashed 33829ae8... again, as before the Debian boot.

### Rach2's trial

* The boot: `bootctl kernel` with the trial kernel and a menu of one entry, the release's command line with `intel_iommu=off intremap=off` added. `reboot` at 02:50:47, SSH at 02:51:08, 21 s; `mixer vol 1` at once. `/proc/cmdline` had `rach2.slot=candidate ... intel_iommu=off intremap=off`, and `bootctl status` showed the trial consumed.
* The log: `boot: Rach2 Booting` first, then

```
[    0.045000] mouse: started
[    0.045000] synaptics: found: touchpad model 1, fw 8.1, id 0x1e2b1, caps 0xd00123/0x840300/0x2a800/0x0, board id 2848, fw id 1581875
[    1.291000] mouse: ready
```

with no other `synaptics:` line, and `vtd: off by intel_iommu=off intremap=off: 2 units read`. The pad's bring-up, from `mouse: started` to `mouse: ready`, took 1.25 s by the kernel's clock.
* `evprobe touchpad`, whole:

```
EVPROBE: /dev/input/event1 is SynPS/2 Synaptics TouchPad
I: Bus=0011 Vendor=0002 Product=0007 Version=01b1
N: Name="SynPS/2 Synaptics TouchPad"
P: Phys=isa0060/serio1/input0
B: PROP=1
B: EV=b
B: KEY=e520 30000 0 0 0 0
B: ABS=660800011000003
EVPROBE: abs 0x0 min 1314 max 5670 fuzz 0 flat 0 res 44 value 0
EVPROBE: abs 0x1 min 1098 max 4794 fuzz 0 flat 0 res 76 value 0
EVPROBE: abs 0x18 min 0 max 255 fuzz 0 flat 0 res 0 value 0
EVPROBE: abs 0x1c min 0 max 15 fuzz 0 flat 0 res 0 value 0
EVPROBE: abs 0x2f min 0 max 1 fuzz 0 flat 0 res 0 value 0
EVPROBE: abs 0x35 min 1314 max 5670 fuzz 0 flat 0 res 44 value 0
EVPROBE: abs 0x36 min 1098 max 4794 fuzz 0 flat 0 res 76 value 0
EVPROBE: abs 0x39 min 0 max 65535 fuzz 0 flat 0 res 0 value 0
EVPROBE: abs 0x3a min 0 max 255 fuzz 0 flat 0 res 0 value 0
EVPROBE OK
exit 0
```

* The comparison: the `I:`, `N:`, `P:`, `B:` and `EVPROBE: abs` lines of both outputs, less each axis's value, `diff` with no output. The identity, name, place, the four bit sets and each axis's limits, fuzz, flat and resolution equal Debian's, the resolutions 44 and 76 among them.
* Back: the empty menu staged, the release kernel into the candidate slot, a boot at 02:51:31 that answered at 02:51:52 on the release's own command line, `bootctl commit` and `bootctl fallback`. `machine_identity.sh` found 0effe5ac... running from the candidate slot and in the fallback slot, and the lock was removed at 02:52.
