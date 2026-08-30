# Resume state

Phase: 1 of 7, arrival and inventory.

In progress: the configuration-space inventory. The four tracking files are
committed and the tree is otherwise as milestone 34 left it.

Evidence so far: none beyond the plan. `hardware.md` holds the two facts the
plan was written from, both from outside this tree.

Next action: add `rach2/sys/usb/usb.c` and `usb.h` with a report of every PCI
class 0x0C subclass 0x03 function and, for an Intel xHCI, `XUSB2PR` and
`XUSB2PRM`; call it from `kernel.c` after the PCI scan; add the file to
`rach2/sources.mk`.

Last file read: `rach2/sys/pci.h`, for `pci_device_t` and
`pci_find_device_skip`.
