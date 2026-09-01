Phase 4 is built and its suite passes; the mutations are next, then phase 5.
Evidence gathered: tests/run usbdisk passes its eight boots in 103 s; the host
tests pass; the throughput is 49648 KB/s with messages and 133 KB/s on the
timer's pump, both in notes.md.
Next action: phase 4's eight mutations, each from a script that restores the
source and rebuilds; then tests/run style, host, usb and usbdisk once; then
phase 5's tests/run all and the gate review.
File being read when written: dev-log/40-usbdisk/plan.md, phase 4.
