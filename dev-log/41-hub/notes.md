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

## Open items

* None yet.
