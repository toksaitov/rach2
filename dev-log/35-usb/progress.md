# Milestone 35 progress

Every item in `plan.md`, in the order the phases do them. An item is ticked in
the commit that completes it.

## Phase 1: arrival and inventory

* [x] `AGENTS.md`, `CLAUDE.md` and `plan.md` read in full
* [x] clean tree, `git log --oneline -12` recorded in `notes.md`
* [x] `progress.md`, `current.md`, `notes.md`, `hardware.md` created
* [x] the kernel records every USB controller from configuration space
* [x] the Intel xHCI routing registers recorded beside it
* [x] `metal_accept.sh` reports the same over SSH
* [x] QEMU's configuration-space facts in `hardware.md`
* [ ] the laptop's facts in `hardware.md`, including whether the firmware's
      legacy emulation already types at the console
* [x] proof: the records appear with a controller attached and not without one

## Phase 2: the interface and the model

* [ ] `sys/usb/hcd.h`
* [ ] `sys/usb/xhci.h` with `struct xhci_io`
* [ ] the controller model in `tests/host/xhci_hosttest.c`
* [ ] rows: discovery, D0, the aperture check
* [ ] rows: handoff at once, slowly, never
* [ ] rows: stop, reset, the one-millisecond pause, `CNR`
* [ ] rows: the capability reads, both context sizes, `PAGESIZE` without 4 KiB
* [ ] rows: scratchpad request of zero, of several, above the maximum
* [ ] mutations: ownership wait, `CNR`, the pause, context size, the scratchpad
      array, the scratchpad maximum
* [ ] review 1 of 6

## Phase 3: bring-up

* [ ] the device-context base array, the command ring, the event ring and its
      segment table, the doorbells, `CONFIG`, `RS`
* [ ] port power, debounce, port reset, the event drain
* [ ] `usb_stop` and its place in `kernel_shutdown`
* [ ] rows: ring wrap and the cycle bit, `ERDP`, the port cases, command abort,
      `PORTSC` write discipline
* [ ] QEMU's capability values in `hardware.md`
* [ ] proof: a device on a root port is recorded with its speed; nothing is
      recorded with no device attached
* [ ] review 2 of 6

## Phase 4: enumeration

* [ ] `sys/usb/core.c`: the state machine and its deadlines
* [ ] Enable Slot, the input contexts, Address Device
* [ ] the descriptors, Evaluate Context, the derived endpoint fields
* [ ] Configure Endpoint, then `SET_CONFIGURATION`
* [ ] endpoint recovery: Reset Endpoint, `CLEAR_FEATURE`, Set TR Dequeue,
      Stop Endpoint
* [ ] the device table and its refusals
* [ ] `/proc/usb` and its empty-file contract
* [ ] `usbprobe`
* [ ] `tests/host/usb_hosttest.c` in full
* [ ] mutations: the configuration order, a descriptor length past the buffer,
      the Evaluate Context step, the interval derivation, the fifth device
* [ ] review 3 of 6

## Phase 5: input and HID

* [ ] `sys/input.c`: `input_key` and `input_mouse`, the PS/2 handler reduced to
      reading the port
* [ ] `/dev/mouse` opens when any mouse exists
* [ ] the refactor proved: `ttyprobe`, `stdintest`, `jobprobe`, `console`, Doom
* [ ] `sys/usb/hid.c`: boot protocol, the report diff, the scancode table with
      modifiers and `0xE0` sequences, the mouse packets
* [ ] the held-key release on teardown
* [ ] the harness monitor connection and its four steps
* [ ] `harness --selftest` covers the new steps
* [ ] `tests/run usb` at both `usb_version` settings
* [ ] `rach2.nousb` and the `subset` boot
* [ ] `procfiles` gains the `/proc/usb` case
* [ ] mutations: break codes, rollover, the report diff, the disconnect event,
      the held-key release, the USB path into `input_key`
* [ ] review 4 of 6

## Phase 6: the machine

* [ ] the phase 1 readings presented to the owner; the routing decided
* [ ] the firmware's VT-d setting recorded
* [ ] the laptop booted with a USB keyboard and mouse
* [ ] the real capability values in `hardware.md` beside QEMU's
* [ ] `tests/run metal` and the acceptance script gain the USB lines
* [ ] proof: a person types and moves on the laptop; the internal keyboard is
      untouched; `rach2.nousb` still reaches a shell; poweroff and reboot both
      complete with the controller halted and the routing as it was found
* [ ] review 5 of 6

## Phase 7: the record

* [ ] `rach2/sys/usb/Readme.md`
* [ ] the input layer's section in `rach2/Readme.md`
* [ ] `rach2/Readme.md`: subsystem table, hardware list, boot options, limits
* [ ] `Readme.md`: the system and hardware-status paragraphs
* [ ] `synthfs.Readme.md`: the `/proc/usb` row
* [ ] `tests/Readme.md` and `tests/system/Readme.md`
* [ ] the documentation and comment pass, with the Codex read
* [ ] the tier decision for `usb`, measured
* [ ] `notes.md` read from the top and every line closed
* [ ] the full suite set twice, no machine left running, `make test` measured
* [ ] `dev-log/35-usb/Readme.md` with the trust statement
* [ ] `dev-log/Readme.md` gains the entry
* [ ] review 6 of 6
