Phase 1 done; phase 2 (the APIC, the vectors and MSI) is next.
Evidence gathered: rach2.apicdiag boots and reads QEMU's APIC and the two controllers' capabilities, recorded in hardware.md; the plan's specification numbers check out; QEMU's xHCI needs msi=on to have an MSI capability at all.
Next action: lapic.c and its host test, the sixteen stubs at 48 to 63, the router's two ranges, /proc/interrupts and the intr count, pci_msi_enable and pci_msi_disable with their host test, and rach2.nomsi.
File being read when written: dev-log/39-msi/plan.md, phase 2.
