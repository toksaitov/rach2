Phase 2 done; phase 3 (the class driver) is next.
Evidence gathered: eight slots and eight devices, endpoint numbers to 15 with bulk contexts, a generation carried through a transfer and back, a close that never answers leaving a tombstone; the xHCI rows pass and the USB suite is unchanged.
Next action: msc.c with the transport, bring-up, transfers, the error matrix and reset recovery, the model with its vanishing, msc_hosttest.c in full, and the block device with its admission queue and atomic wait.
File being read when written: dev-log/40-usbdisk/plan.md, phase 3.
