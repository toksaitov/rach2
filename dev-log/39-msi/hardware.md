# Milestone 39 hardware notes

What the machine must say before a kernel with MSI is installed on it is read by `rach2.apicdiag`, which changes nothing. Milestone 43 ran it on the Lenovo on 2026-09-03 through a trial boot under `rach2.nomsi`, and the column below is what it printed.

QEMU's column is from a boot of `-cpu max -device qemu-xhci,msi=on -device intel-hda` with `rach2.apicdiag`.

| Reading | QEMU | Lenovo G50-70 |
|---------|------|-----------------------|
| `cpuid` leaf 1: local APIC | yes | yes |
| `cpuid` leaf 1: model-specific registers | yes | yes |
| `IA32_APIC_BASE` | 0xFEE00900: base 0xFEE00000, enabled, bootstrap processor, not x2APIC | 0xFEE00900: base 0xFEE00000, enabled, bootstrap processor, not x2APIC |
| APIC id | 0 | 0 |
| APIC version | 0x14, 6 local-vector entries | 0x15, 7 local-vector entries |
| Task priority | 0 | 0 |
| Spurious vector register | 0x1FF: software enabled, vector 255 | 0x10F: software enabled, vector 15 |
| `LINT0` | 0x8700: unmasked, delivery 7 (external interrupt) | 0x700: unmasked, delivery 7 (external interrupt) |
| `LINT1` | 0x8400: unmasked, delivery 4 (non-maskable) | 0x400: unmasked, delivery 4 (non-maskable) |
| xHCI | 00:04.0, 1b36:000d, legacy irq 11 | 00:14.0, 8086:9c31, legacy irq 7 |
| xHCI MSI | capability at 0x70, disabled, 1 of 16 vectors, 64-bit address | capability at 0x80, disabled, 1 of 8 vectors, 64-bit address |
| xHCI MSI-X | capability at 0x90, disabled, 16 vectors | none |
| HDA | 00:05.0, 8086:2668, legacy irq 10 | 00:03.0, 8086:0a0c, legacy irq 7 (the display audio; the driver binds 00:1b.0) |
| HDA MSI | capability at 0x60, disabled, 1 of 1 vector, 64-bit address | capability at 0x60, disabled, 1 of 1 vector, 32-bit address |
| HDA MSI-X | none | none |

The firmware QEMU boots leaves the APIC enabled in the model-specific register, software enabled in its spurious register, `LINT0` an unmasked external-interrupt entry and `LINT1` an unmasked non-maskable one. That is the virtual-wire state the plan requires before the 8259's interrupts keep arriving; a machine whose `LINT0` is masked is the case `lapic.c` has to set. The Lenovo's firmware leaves the same state with two differences that changed nothing: its spurious vector is 15 rather than 255, which `lapic.c` rewrites to 255 when it enables the APIC, and its HDA capability has a 32-bit message address. The diagnostic reports the first audio-class device in configuration space, the Haswell display audio at 00:03.0, while the driver binds the chipset's controller at 00:1b.0; both took their message vectors on the boot that followed.
