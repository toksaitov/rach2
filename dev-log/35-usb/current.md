# Resume state

Phase: 1 of 7, arrival and inventory.

In progress: the last item of phase 1, which is the laptop's half of
`hardware.md`. Everything else in the phase is done and committed: the
inventory, the routing read, the acceptance line, QEMU's facts, and the proof
by two boots.

Evidence so far: `hardware.md` holds QEMU's configuration-space facts for all
four controller kinds. The laptop's column is empty and needs the machine.

Next action: ask the owner for the notebook. It needs one boot of the current
kernel with an external USB keyboard and mouse plugged in, to fill the laptop
column and to answer whether the firmware's legacy emulation already types at
the Rach2 console. Phase 2 does not depend on it and can start first.

Last file read: `tests/system/accept/metal_accept.sh`.
