# Milestone 39 hardware notes

Nothing touches the Lenovo in this milestone. What the machine must say before a kernel with MSI is installed on it is read by `rach2.apicdiag`, which changes nothing; milestone 43 runs it there, fills the column below, and boots the new kernel with `rach2.nomsi` first.

QEMU's column is from a boot of `-cpu max -device qemu-xhci,msi=on -device intel-hda` with `rach2.apicdiag`.

| Reading | QEMU | Lenovo (milestone 43) |
|---------|------|-----------------------|
| `cpuid` leaf 1: local APIC | yes | |
| `cpuid` leaf 1: model-specific registers | yes | |
| `IA32_APIC_BASE` | 0xFEE00900: base 0xFEE00000, enabled, bootstrap processor, not x2APIC | |
| APIC id | 0 | |
| APIC version | 0x14, 6 local-vector entries | |
| Task priority | 0 | |
| Spurious vector register | 0x1FF: software enabled, vector 255 | |
| `LINT0` | 0x8700: unmasked, delivery 7 (external interrupt) | |
| `LINT1` | 0x8400: unmasked, delivery 4 (non-maskable) | |
| xHCI | 00:04.0, 1b36:000d, legacy irq 11 | 00:14.0, Lynx Point-LP (milestone 35) |
| xHCI MSI | capability at 0x70, disabled, 1 of 16 vectors, 64-bit address | |
| xHCI MSI-X | capability at 0x90, disabled, 16 vectors | |
| HDA | 00:05.0, 8086:2668, legacy irq 10 | |
| HDA MSI | capability at 0x60, disabled, 1 of 1 vector, 64-bit address | |
| HDA MSI-X | none | |

The firmware QEMU boots leaves the APIC enabled in the model-specific register, software enabled in its spurious register, `LINT0` an unmasked external-interrupt entry and `LINT1` an unmasked non-maskable one. That is the virtual-wire state the plan requires before the 8259's interrupts keep arriving; a machine whose `LINT0` is masked is the case `lapic.c` has to set.
