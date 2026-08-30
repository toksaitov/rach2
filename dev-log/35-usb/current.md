# Resume state

Phase: 2 of 7, the interface and the model, complete. Phase 3 is next.

In progress: nothing. Phase 2 is committed with its review applied. The one
item left in phase 1 is the laptop's column of `hardware.md`, which needs the
machine and the owner.

Evidence so far: `xhci_hosttest` passes with the rows for discovery, the D3
wake, the capability reads, the aperture and register-set bounds, the firmware
handoff at once, slowly, never, and refused, the capability walk, the halt and
reset with the ready wait and the Intel pause, and the scratchpad pages.
Fourteen mutations, each caught by the row it was aimed at.

Next action: phase 3, bring-up. The device-context base array, the command
ring with its Link TRB, the event ring and its segment table, the doorbells,
`CONFIG`, `RS`, port power, debounce, port reset, the event drain, and
`usb_stop` beside `netdev_stop` in `kernel_shutdown`.

Last file read: `tests/host/xhci_hosttest.c`.
