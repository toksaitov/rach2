Milestone 39 is closed.
Evidence: the APIC comes up and the 8259's interrupts still arrive through it; the xHCI takes vector 48 and the audio controller 49, each with the timer's pump behind it; the four review units' findings are all fixed, including the two that would have shown only on the Lenovo; 63 of 64 suites pass in one run of the full set with `selfbuild` passing on its own, and `make test` takes 5 minutes 6 seconds.
Next action: milestone 40, the USB disk, from dev-log/40-usbdisk/plan.md.
File being read when written: dev-log/39-msi/plan.md, phase 5.
