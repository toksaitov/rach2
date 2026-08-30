# Resume state

Phase: 5 of 7 complete with its review. Phase 7's documents are written. Phase 6
needs the notebook and is the only thing blocking the milestone.

In progress: nothing. What is left needs the owner and the machine:

* Phase 1's last item: the laptop's column of `hardware.md`.
* The whole of phase 6: the firmware handoff, the real capability values, the
  routing decision, and a person typing on a USB keyboard at the laptop's
  console.

Everything phase 6 needs is ready. The kernel reports its USB inventory from
configuration space at boot, including the Intel routing registers;
`tests/system/accept/metal_accept.sh` reads those lines back over SSH; and
`rach2.nousb` switches the stack off if a boot with it goes wrong.

Next action for a session with the machine: boot the notebook with the current
kernel and an external USB keyboard and mouse, read the `usb:` records, fill
`hardware.md`, and put the routing question to the owner with the values in
front of them.

Next action without it: phase 7's remaining items, which are the measurements,
the tier decision, the milestone record and the dev-log entry. They can all be
written except the half of the trust statement that only the laptop can settle.

Last file read: `tests/system/Readme.md`.
