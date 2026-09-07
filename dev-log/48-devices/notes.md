# Milestone 48: notes

## Where the work stands

Phase 1 is part done. What is on `master` is the first half: every xHCI
controller is driven from a record of its own. What is on the branch
`48-sizing-wip` is the second half, unfinished: the tables sized from each
controller's own capability registers.

Phases 2, 3 and 4 have not been started.

## What is done, on master

Commit `f71aad0`, "Give every xHCI controller its own record, rings and
interrupt".

`struct xhci_ctrl` holds what used to be forty file-scope variables in
`xhci.c`: the aperture, the capability record, the legacy-handoff state, the
PCI position, the command and event rings, the context base array, the
per-port state, the completions, the slot table, the input context and the
command in flight. `ctrls[XHCI_MAX_CONTROLLERS]` holds four of them. Every
function in the driver takes the controller as its first argument, named `hc`;
the register accessors in `struct xhci_io` take it too, so a host test can
model two controllers.

`xhci_discover` walks the whole bus and takes a record for every xHCI
controller on it, in the order the walk finds them. A controller of another
interface is named and not driven, through `usb_interface_name`, which moved
from `usb.c` to `usb.h`. A controller whose bring-up fails is left out of the
table and says why; the others still come up. `xhci_controllers()` and
`xhci_controller(n)` are how the rest of the kernel reaches them, and
`xhci_number(hc)` is a controller's place in the table.

Above the driver: `usb_device_state` records which controller a device is on,
and `usb_core_step` drains and scans every controller in one pass. The
enumeration turn, the refused-port bitmap and the unentered-port bitmap are
one per controller, so two controllers enumerate beside each other. A hub
record and a stick record each carry their controller. The port operations
take the controller as their first argument; a hub ignores it, since a hub is
reached through transfers to its own device. `usb_init` brings up each
controller on its own and `usb_interrupts_start` gives each its own vector,
with the one handler draining them all.

`xhci_error(0)` answers with the discovery's own error rather than
dereferencing a controller that does not exist, which is what a machine with
no xHCI asks for; the same for `xhci_ports(0)`.

Proved by `tests/run host` (all of it, including the four USB host tests),
`tests/run usb`, `tests/run usbdisk` and `tests/run default`.

## What is on the branch, unfinished

Branch `48-sizing-wip`, commit `8f41a64`. It builds and the kernel is correct
as far as the compiler goes; `xhci_hosttest` fails 479 rows and the cause is
not yet found.

What it changes:

* The scratchpad pages come from the heap, as many as the controller asks for,
  instead of thirty-two compiled in. The array of their addresses is one page,
  which holds 512, and a controller asking for more is refused by name.
  `XHCI_MAX_SCRATCHPADS` is that 512.
* The per-port state is four heap arrays sized from `caps.max_ports` instead
  of sixteen compiled in. `XHCI_MAX_PORTS` is 255, the range of the standard's
  field, and a controller reporting more is refused rather than clipped.
* The slot table and the context base array are sized from `caps.max_slots`,
  and the controller is told to enable every slot it offers rather than
  `min(its own, 12)`. `XHCI_SLOTS` is 255.
* A slot's device context and each endpoint's ring are one page each, taken
  from the heap when the slot is addressed and when the endpoint is opened,
  and given back in `xhci_device_closed`. The endpoint ring is a page, so
  `XHCI_EP_RING_TRBS` is 256 rather than 16.
* Because the heap's pages are not identity-mapped, every address the
  controller reads is now the physical one recorded beside the pointer this
  kernel reaches it by: `dcbaa_phys`, `scratchpad_array_phys`, `ctx_phys` and
  `ring_phys[dci]`.
* `ctrl_free_tables` gives back what a record holds, called when the registers
  are read again and when a record is reused by a later discovery.
* `xhci_hosttest` gained a page arena and a bump allocator for the kernel heap
  it now needs, and its rows for the scratchpad limit, the slot count and the
  port count were rewritten to the new contract.

Where to pick it up: the 479 failures begin at "and it can be addressed",
which is `xhci_device_address` returning non-zero, which is `slot_pages`
failing, which is the test's page arena giving nothing back. The arena is
1024 pages and the test opens few slots, so either a page is leaked on a path
that does not reach `xhci_device_closed`, or the arena's bump reset in the
test's `kfree` frees memory that is still live. Check the second first: the
reset was added on the assumption that the driver gives every plain
allocation back before it takes another, and `ctrl_free_tables` frees the slot
table after freeing the slots' pages, so a page freed after the reset would be
handed out twice.

## What is left in the milestone

Phase 1, after the sizing: device and hub records allocated rather than fixed
tables (`core.c`'s `devices[]` and `hub.c`'s `hubs[]`); a configuration
descriptor read whole rather than truncated at 64 bytes (`core.c:999`); a
device that finds no slot waiting for one rather than being refused; a
full- or high-speed hub with a device on a port above fifteen driven through
the route encoding the specification defines for it.

Phases 2, 3 and 4 as the plan describes them: the network stack per interface,
the platform's firmware and chipset checks, and the close.

## What was found on the way

The `usb` suite's latency row asserts that a keystroke arrived by message and
not by the timer's pump. It passes five times out of five when its machine
runs alone and failed twice when a second machine ran beside it. That is the
pooled-run race of entry 2 of the shortcomings list, not a fault of this
milestone's code, but it will fail a full set now and then.
