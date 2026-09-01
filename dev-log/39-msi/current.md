Phase 2 done; phase 3 (xHCI on MSI) is next.
Evidence gathered: the APIC comes up in QEMU as `lapic: ready: id 0, version 0x14, base 0xfee00000`; the timer check after it finds the tick still arriving; /proc/interrupts counts vector 32 and names it pit; /proc/stat's intr line moves; all 64 suites pass on the phase 2 tree in 9:05.
Next action: the xHCI's interrupter and its handler as the pump, the wake, the timer's fallback and its counter, then the usb suite's new boots and the xhci rows.
File being read when written: dev-log/39-msi/plan.md, phase 3.
