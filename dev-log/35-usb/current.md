# Resume state

Phase 6 is the only phase not finished, and it needs the notebook.

Everything that can be done for it from here is done:

* `dev-log/35-usb/checklist.md` is the session, step by step, with what each
  answer means and what to record.
* The kernel logs the raw register words the session needs: the legacy-support
  pair before and after the handoff, the three capability words, the scratchpad
  count, the context size, and the Intel routing registers.
* The acceptance script prints every `usb:` line and fails on a refusal.
* `tests/run metal` boots the metal image with a USB keyboard attached, so the
  image that will go to the notebook enumerates one in the rehearsal.
* The controller is given back to the firmware when this kernel cannot use it,
  so a failed bring-up leaves the machine as it found it.
* Review 5 was run against the hardware path rather than the machine.

What is left needs a person at the Lenovo: the firmware settings, a boot with
`rach2.nousb` to see what the firmware does on its own, a boot with the stack,
the `usb:` lines into `hardware.md`, and the routing decision.

The plan says the owner closes this, not the milestone. With the record as it
stands they can close it with QEMU alone; nothing has been recorded either way.

Last file read: `tests/system/accept/metal_accept.sh`.
