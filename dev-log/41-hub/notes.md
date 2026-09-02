# Milestone 41 notes

## Phase 1 record

The tree at the start, clean, `git log --oneline -12`:

    37e4daa Close milestone 40 with its full run on the final tree
    aba4490 Record the closing tree's make test time
    caf9fc2 Keep the lag-sleep boot's log
    487705e Record milestone 40 with the Codex round and the index entry
    c2deedc Pull the stick from under a read of the device itself, and check a write after it
    3db3315 Pair the checked entry's type with its own range, and clean up a failed build
    0108d33 Quote the stick's label and tidy the acceptance script's stick section
    3450344 Say in the four Readmes what the code does, from the Codex reading
    31169d3 Sweep phase 7's two small findings: a stale count and a one-caller helper
    aa2826c Say in the Readmes what the review changed in the class driver and what the stick suite runs
    5933fe8 Record phase 6's image, its checker and its boot
    525a2b0 Say what a flash drive and the stick image are in the Readmes

### The review round

One round after phase 5, before the sweep; units of at most six files and
ten minutes, on Codex through its rescue agent where it returns a report
and on Claude at Opus strength otherwise, two at a time:

* unit A, the controller: `hcd.h`, `xhci.c`, `xhci.h`, `xhci_hosttest.c`;
* unit B, the core and the hub driver: `core.c`, `core.h`, `hub.c`, `hub.h`;
* unit C, the tests: `hub_hosttest.c`, `usb.sh`, `usbprobe.c`,
  `metal_accept.sh`;
* unit D, the reader-facing documents: `rach2/sys/usb/Readme.md`,
  `rach2/sys/fs/synthfs.Readme.md`, `rach2/Readme.md`, `Readme.md`.

### The numbers

published constants (the specifications' own PDFs were not reachable from
this host: usb.org ships a zip, intel.com answers 403). Sources: Linux
include/uapi/linux/usb/ch11.h (which cites the USB 2.0 tables), Linux
drivers/usb/host/xhci.h (slot context bit layout), Linux drivers/usb/core/
hub.c and usb.c (timings, route computation).

USB 2.0 chapter 11, via ch11.h's table citations:
- hub descriptor type 0x29 = USB_TYPE_CLASS | 0x09 (Table 11-13); fields in
  order: bDescLength 1, bDescriptorType 1, bNbrPorts 1, wHubCharacteristics 2
  LE, bPwrOn2PwrGood 1 (2 ms units), bHubContrCurrent 1, DeviceRemovable
  bitmap (ports+1+7)/8 bytes, then PortPwrCtrlMask
- wHubCharacteristics (Table 11-13): LPSM bits 0-1: 0 ganged, 1 per port, 2
  no switching; TTTT bits 5-6 (mask 0x60): 0x00 8, 0x20 16, 0x40 24, 0x60 32
  FS bit times
- class requests (Table 11-16): GET_DESCRIPTOR 6, GET_STATUS 0,
  CLEAR_FEATURE 1, SET_FEATURE 3 (standard codes with the class recipient);
  CLEAR_TT_BUFFER 8, RESET_TT 9, GET_TT_STATE 10, STOP_TT 11
- hub features (Table 11-17): C_HUB_LOCAL_POWER 0, C_HUB_OVER_CURRENT 1
- port features (Table 11-17): CONNECTION 0, ENABLE 1, SUSPEND 2,
  OVER_CURRENT 3, RESET 4, POWER 8, LOW_SPEED 9, C_CONNECTION 16, C_ENABLE
  17, C_SUSPEND 18, C_OVER_CURRENT 19, C_RESET 20
- wPortStatus (Table 11-21): connection 0x0001, enable 0x0002, suspend
  0x0004, over-current 0x0008, reset 0x0010, power 0x0100, low speed 0x0200,
  high speed 0x0400; wPortChange (Table 11-22) has the same low five bits
- wHubStatus / wHubChange: local power 0x0001, over-current 0x0002
- hub protocol byte: 0 full speed or HS without TT, 1 HS single TT, 2 HS
  multi TT (alternate setting 1 selects per-port TT: hub.c
  usb_set_interface(hdev, 0, 1)), 3 SuperSpeed
- status change endpoint: interrupt IN, bitmap (ports+1+7)/8 bytes, bit 0
  the hub (USB 2.0 11.12.3, cited in hub.c); Linux reads maxpacket bytes
  because some hubs send more
- timings (hub.c): debounce stable 100 ms, timeout 2000 ms, step 25 ms;
  short reset 10 ms, root reset 60 ms, reset timeout 800 ms; power-good
  bPwrOn2PwrGood * 2 ms

xHCI slot context (xhci.h):
- word 0: route string bits 0-19; speed 20-23; MTT bit 25; hub bit 26;
  context entries 27-31
- word 1: max exit latency 0-15; root hub port 16-23; number of ports 24-31
- word 2: TT hub slot id 0-7; TT port number 8-15; TT think time 16-17
- word 3: device address 0-7; slot state 27-31
- route string (usb.c): route = parent.route + (port1 << ((parent.level-1)*4)),
  a port above 15 written as 15 ("Route string assumes hubs have less than
  16 ports"); 20 bits is five tiers. The plan's "refuse a port above 15"
  is stricter than Linux, which clamps.

The plan's bullet checked: every number in it agrees with the above.

iPXE src/drivers/bus/usb.c usb_route_string agrees: walking up from the
device, route = (route << 4) | min(port, 15) per hub tier; the nearest hub's
port is the lowest nibble. Two independent implementations, the same rule.

Every number in the plan's hub-class bullet and its slot-context bullet
agrees with these. One decision the plan makes is stricter than the two
implementations read: they clamp a port above 15 to 15 in the route string,
and the plan refuses such a device by name, which is kept, because a device
addressed by a wrong port is a device the controller talks to at random.

## Phase 2 record

`struct usb_location` in `hcd.h` is the tree address: the root port, the
depth, the route string, and the translating hub's handle, port and per-port
flag for a low or full-speed device below a high-speed hub. `device_open`
takes it, `device_hub` is new, and one function in `xhci.c`, `slot_words`,
writes the three location words of the slot context for every command that
carries it, which is what keeps the address, configure and evaluate commands
from disagreeing. The controller refuses a depth past five tiers, a route
with bits above its depth, and a think time above 3. `XHCI_SLOTS` and
`USB_MAX_DEVICES` are twelve; the tables' sizes are in `hardware.md`.

Twelve rows in `xhci_hosttest`: the route string and root port of a device
one and two tiers down, the fifth tier accepted and a sixth refused, a route
with bits above its depth refused, port 15, a full-speed hub's word and a
high-speed multi-TT hub's, a low-speed device below it with the translator's
slot and port and the multi-TT bit, a device below a full-speed hub below
the high-speed one carrying the high-speed hub at its own port, and the
thirteenth device refused at its addressing. `tests/run usb` and `tests/run
usbdisk` pass unchanged.

Four mutations, each caught: the route written a tier up, `with the route
string 0x3`; the hub bit forgotten, `with the hub bit and the count`; the
translator's port written as the device's own, `and carries the high-speed
hub as its translator, at the hub's port`; a sixth tier accepted, `and one
on a sixth tier is refused`. The third was first caught by the root-port
row and not the one named, because for a device directly below a hub the
route's low nibble and the translator's port are the same number; the row
two tiers down, whose translator port is the hub's port the full-speed hub
sits on, is the one that can tell, and it had that port wrong.

## Phase 3 record

The core's port operations are a vtable on the device record, `usb_port_ops`
in `core.h`: status, settled, reset begun and reset done, the controller's
for a root port and the hub driver's for a hub port. A device's location is
built from its hub's by `location_of`, which refuses a port above 15 and a
sixth tier, and which finds the translator for a low or full-speed device by
walking up to the nearest high-speed hub. The hub is class 9 with protocol 0,
1 or 2; protocol 3 is refused as a SuperSpeed hub by name. A hub's ports are
refused on the hub's own bitmap. `device_gone` closes a device's subtree
before the device, and a closing hub waits for its subtree before it asks
for its slot back. Records name a device by its path, `5.2` for port 2 of
the hub on root port 5.

`hub.c` brings a hub up from the pump: the descriptor, the controller told
through a command turn the core gives it (`usb_core_hub_tell`, made on the
pattern of the halt clear), the ports powered as the descriptor's mode says,
the power-good time, and every port's status read once, reporting a
connection to the core with `usb_core_hub_port`. A reset behind a hub is
asked for by the core and run by the driver, `SET_PORT_FEATURE(PORT_RESET)`,
the status read every 20 ms, and `C_PORT_RESET` cleared. The change
endpoint is phase 4's.

In QEMU, `usb-hub` on `qemu-xhci` with `usb-kbd` on its port 2 gives
`usb: device: port 5: 409:55aa, class 9 protocol 0, endpoint 1, a hub`,
`usb: hub: device 1: 8 ports, power mode 2, think time 0`, and the keyboard
at `port 5.2`; pulling the hub records `port 5.2: gone` before `port 5:
gone`.

`hub_hosttest` includes the core and the hub driver over a model of a tree:
the controller stubs give a slot for a location and the model answers on it
as what sits there. Rows: the descriptor read and eight ports told, a
full-speed hub told nothing of translators, unswitched, ganged and per-port
power, no port read before the power-good time, the keyboard on port 2 found
by the scan and enumerated at route 2 one tier down with its port reset
through the hub, `/proc/usb` giving `port 1.2`, a multi-TT high-speed hub's
slot told its translators and think time and a low-speed keyboard below it
carrying the hub at port 3, twenty ports told as fifteen, a reset that never
ends refusing the device with the port remembered on the hub until its
device leaves, a disconnection reported by the hub closing the device below,
a hub pulled with two devices below closing both children before itself,
and a hub that will not give its descriptor closed.

`tests/run usb` used QEMU's hub as its stand-in for a device this kernel
cannot drive; the hub is driven now, so the stand-in is `usb-tablet`, a HID
device with no boot protocol, which is refused with the same record.

Mutations: the ports scanned before the hub is configured, caught by `its
descriptor is read and its eight ports told to the controller`; a hub closed
before its children, caught by `the two children before the hub`; two
devices enumerated at once across the tree, caught; a pulled hub's child
kept in the table, not caught, and it cannot be: the child also leaves
through its port status, which answers nothing once the hub driver is
detached, so the check is met by the second road. The explicit close of the
subtree is kept for the order of the records, children before their hub.
The first attempt at the second mutation did not build, an unused static
function under `-Werror`, and the script read a stale log as an escape; it
now reports a build that failed as such.

## Phase 4 record

The change endpoint: one transfer always outstanding, of `(ports + 1 + 7) /
8` bytes, put out again at each answer; its bitmap goes into a word of
pending changes, bit 0 the hub and bit N port N, and the running arm deals
with them one request at a time, the hub's own status for bit 0 with its
local-power and over-current changes cleared, and a port's status for its
bit with every change bit acknowledged by its own `CLEAR_PORT_FEATURE`. A
connection change tells the core, which closes the old subtree first. An
enable lost closes the device and tells the core the connection is there
again. An over-current powers the port off, closes the device, powers on
once after a second, and leaves the port off with a record on a second
over-current; a port left off or waiting for its power reports no
connection. A multi-TT hub has `SET_INTERFACE` alternate 1 sent before the
controller is told, and one that refuses it is told with one translator. A
stalled change endpoint, or a request unanswered for half a second, gives
the hub up with its subtree.

In QEMU, with `usb-hub` and a mouse behind it, a keyboard plugged into hub
port 3 after the boot enumerates at `port 5.3`, its pull records `port 5.3:
gone`, and a keyboard plugged in again enumerates again; a stick behind the
hub passes `usbdiskprobe volumes`.

The model gained the change endpoint, the hub's status, port change words,
power off and on, `SET_INTERFACE`, a stalled change endpoint, and the rule
that a reset change never acknowledged refuses the next reset on that port.
Rows: the transfer out once the hub is up and out again after each answer,
a keyboard plugged in after the hub is up enumerated with its connection
change cleared on its port, pulled and gone, the hub's local-power change
read and cleared, a connection change with the device still there closing
the old before the new, an enable lost closing the device and clearing its
change, an over-current powering off and closing, on again after a second,
and left off when it recurs, a multi-TT hub's setting selected and told,
one that refuses told with one translator, and a stalled change endpoint
closing the hub with its child.

Mutations, each caught: the power-good wait skipped, `no port is read
before the power-good time`; the scan after it skipped, `the keyboard on
port 2 is found and enumerated`; the wrong change bit cleared, `acknowledged
with C_PORT_CONNECTION on port 4`; the speed read from the wrong bit, `at
low speed, read from the status word`, once the row read the speed the core
opened the device with rather than the model's own; the change transfer put
out only once, `with the transfer out a third time`; `C_PORT_RESET` never
cleared, caught by `its port was reset through the hub and the reset change
cleared` and not by the second connection the plan named, because the
driver also acknowledges a stale reset change through the change path; a
connection change enumerated without closing what was there, `a connection
change with the device still there closes the old device`. The think time
written into a child's location is the controller test's row from phase 2,
since the location has no such field. Two mutations first failed to build,
an unused variable and an unused function; both were re-made in a form that
builds.

A hub with three devices behind it comes up under QEMU in about half a
second from the hub's own enumeration: the hub at 0.31 s, its keyboard at
0.68 s; each device behind it is a few dozen control transfers through the
one command turn.

## Open items

* None yet.
