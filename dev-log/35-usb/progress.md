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

* [x] `sys/usb/hcd.h`
* [x] `sys/usb/xhci.h` with `struct xhci_io`
* [x] the controller model in `tests/host/xhci_hosttest.c`
* [x] rows: discovery, D0, the aperture check
* [x] rows: handoff at once, slowly, never
* [x] rows: stop, reset, the one-millisecond pause, `CNR`
* [x] rows: the capability reads, both context sizes, `PAGESIZE` without 4 KiB
* [x] rows: scratchpad request of zero, of several, above the maximum
* [x] mutations: ownership wait, `CNR`, the pause, context size, the scratchpad
      array, the scratchpad maximum
* [x] review 1 of 6

## Phase 3: bring-up

* [x] the device-context base array, the command ring, the event ring and its
      segment table, the doorbells, `CONFIG`, `RS`
* [x] port power, debounce, port reset, the event drain
* [x] `usb_stop` and its place in `kernel_shutdown`
* [x] rows: ring wrap and the cycle bit, `ERDP`, the port cases, command abort,
      `PORTSC` write discipline
* [x] QEMU's capability values in `hardware.md`
* [x] proof: a device on a root port is recorded with its speed; nothing is
      recorded with no device attached
* [x] review 2 of 6

## Phase 4: enumeration

* [x] `sys/usb/core.c`: the state machine and its deadlines
* [x] Enable Slot, the input contexts, Address Device
* [ ] the descriptors, Evaluate Context, the derived endpoint fields
* [x] Configure Endpoint, then `SET_CONFIGURATION`
* [x] endpoint recovery: Reset Endpoint, Set TR Dequeue (`CLEAR_FEATURE` and
      Stop Endpoint are phase 5, where a transfer can stall)
* [x] the device table and its refusals
* [x] `/proc/usb` and its empty-file contract
* [x] `usbprobe`
* [x] `tests/host/usb_hosttest.c` in full
* [x] mutations: the configuration order, a descriptor length past the buffer,
      the Evaluate Context step, the interval derivation (the fifth device is a
      recorded limit)
* [x] review 3 of 6

## Phase 5: input and HID

* [x] `sys/input.c`: `input_key` and `input_mouse`, the PS/2 handler reduced to
      reading the port
* [x] `/dev/mouse` opens when any mouse exists
* [x] the refactor proved: `ttyprobe`, `stdintest`, `jobprobe`, `console`, Doom
* [x] `sys/usb/hid.c`: boot protocol, the report diff, the scancode table with
      modifiers and `0xE0` sequences, the mouse packets
* [x] the held-key release on teardown
* [x] the harness monitor connection and its four steps
* [x] `harness --selftest` covers the new steps
* [x] `tests/run usb` at both `usb_version` settings
* [x] `rach2.nousb` and the `subset` boot
* [x] the `/proc/usb` empty-file case, in the `usb` suite rather than
      `procfiles`: that suite's machine is the one that can boot without USB
* [x] mutations: break codes, rollover, the report diff, the disconnect event,
      the held-key release, the USB path into `input_key`
* [x] review 4 of 6

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

* [x] `rach2/sys/usb/Readme.md`
* [x] the input layer's section in `rach2/Readme.md`
* [x] `rach2/Readme.md`: subsystem table, hardware list, boot options, limits
* [x] `Readme.md`: the system and hardware-status paragraphs
* [x] `synthfs.Readme.md`: the `/proc/usb` row
* [x] `tests/Readme.md` and `tests/system/Readme.md`
* [ ] the documentation and comment pass, with the Codex read
* [ ] the tier decision for `usb`, measured
* [ ] `notes.md` read from the top and every line closed
* [ ] the full suite set twice, no machine left running, `make test` measured
* [ ] `dev-log/35-usb/Readme.md` with the trust statement
* [ ] `dev-log/Readme.md` gains the entry
* [ ] review 6 of 6
