# Upstream faults of milestone 81

The format and the index of every milestone's faults are in [the upstream index](../upstream.md).

## Linux's KVM drops the faults of an emulated task switch

* Project: Linux 6.12, `arch/x86/kvm/emulate.c` (SHA-256 55ded5bb83d0d2cc8698206178ab1cdd6d20a852304d1d1f5a18db94b02785d8), `arch/x86/kvm/x86.c` (0ee7bd359a222004aa3de5c313909a892ca0d7bcff18d48f0c63625002318176) and `arch/x86/kvm/vmx/vmx.c` (745d6c8542331c7acf9fe6d9ade5b4c036e120a5aeef9ebfe84ec413131b1881), read from the v6.12 tag.
* Fails: a fault that the emulated task switch raises never reaches the guest, and the guest goes on with part of the switch made. `emulator_do_task_switch` returns X86EMUL_PROPAGATE_FAULT with `ctxt->exception` set for #TS (a TSS below its size), #GP (a task gate's DPL, a selector past its table, a CR3 whose PDPTEs do not load), #NP, #SS and #TS (the new task's segments) and #PF (a TSS or table not mapped). `emulator_task_switch` (emulate.c line 3011) turns every result but X86EMUL_UNHANDLEABLE into EMULATION_OK. `kvm_task_switch` (x86.c line 11842) then writes `ctxt->eip` and `ctxt->eflags` back and returns 1, and never injects `ctxt->exception`, as `x86_emulate_instruction` does through `inject_emulated_exception`. RIP is the old one, already stepped past the instruction by `handle_task_switch` (vmx.c line 5720). The segments `__load_segment_descriptor` loaded before the fault stay loaded, TR and the busy bits are as far as the switch got, and EFLAGS may be the new task's.
* Found: 2026-11-10, on the Mac, in milestone 81, task 81-3a.5, while emulating the task switch for Rach2's VMX backend after Linux's code.
* Reproduction: a 32-bit guest that JMPs to a TSS descriptor with a limit below 0x67, or whose new TSS names a data segment that is not present, under KVM on an Intel processor. The processor would deliver #TS or #NP; under Linux 6.12 the guest runs on with no fault. Not yet run on Linux.
* Evidence: `dev-log/81-kvm/upstream.md`; the commit "Emulate a VMX guest's hardware task switch as Linux does, with the faults the processor raises, which Linux drops"; the rows of `tests/host/kvm_vmx_hosttest.c` for a TSS too small, a gate's DPL and a segment not present.
* Rach2: `kvm_task_switch` in `rach2/sys/kvm/emulate.c` queues each fault as the processor raises it. A fault found before the switch is committed leaves the old task as it was, with RIP at the instruction. A fault in loading the new task's state comes after TR, the busy bits, NT and the registers are the new task's, as the SDM's order of a task switch gives.
* Status: unreported.
* Next: run the reproduction on a current kernel under KVM, check whether a later release injects `ctxt->exception` in `kvm_task_switch`, and report it to the KVM list with the reproduction as a kvm-unit-tests case.

## QEMU's TCG gives no exit for an intercepted CR3 read

* Project: QEMU 10.2.1, Ubuntu's build 1:10.2.1+ds-1ubuntu3.3, its SVM emulation for TCG.
* Fails: with INTERCEPT_CR3_READ set in the VMCB, a guest's `mov %cr3, %reg` completes in the guest with no #VMEXIT, where AMD's manual gives SVM_EXIT_READ_CR3, which Linux's nested KVM gives on the same test. The lines of QEMU's source that do it are not yet traced.
* Found: 2026-11-10, on the testing PC under WSL, in milestone 81, task 81-6c, while qualifying the `max-svm` processor profile, `max` with `svm` and `npt`, by kvm-unit-tests' SVM tests under TCG.
* Reproduction: `qemu-system-x86_64 -accel tcg -cpu max,svm=on,npt=on -smp 2 -m 4g -device pc-testdev -device isa-debug-exit,iobase=0xf4,iosize=0x4 -nodefaults -display none -serial stdio -kernel x86/svm.flat -append cr3_read_intercept`, with kvm-unit-tests at commit 5a221342; `cr3_read_intercept_emulate` fails the same way. `sh tests/kvm/qualify.sh tcg max-svm` runs it with the others.
* Evidence: `dev-log/81-kvm/81-6c/tcg-failures/svm-cr3_read_intercept.log`, `svm-cr3_read_intercept_emulate.log`; `tests/kvm/ref/svm-max-svm-tcg.status` and `svm-host-svm-kvm.status`, where the test passes under KVM.
* Rach2: nothing; Rach2's SVM backend is proven on the CR3 intercept under nested KVM on the testing PC, and its cases under TCG depend on no such exit.
* Status: unreported.
* Next: read QEMU's SVM intercept checks for CR reads in its TCG helpers and see whether a later release than 10.2.1 fixes it; then report it with the kvm-unit-tests command.

## QEMU's TCG gives no selective CR0 write exit for LMSW

* Project: QEMU 10.2.1, Ubuntu's build 1:10.2.1+ds-1ubuntu3.3, its SVM emulation for TCG.
* Fails: with the selective CR0 write intercept set, an LMSW that changes CR0.MP completes in the guest with no SVM_EXIT_CR0_SEL_WRITE, which AMD's manual gives for LMSW and Linux's nested KVM gives on the same test; a MOV to CR0 that changes the same bit exits as it should under TCG. The lines of QEMU's source that do it are not yet traced.
* Found: 2026-11-10, on the testing PC under WSL, in milestone 81, task 81-6c, with the first fault.
* Reproduction: as the first fault's, with `-append sel_cr0_lmsw_intercept`.
* Evidence: `dev-log/81-kvm/81-6c/tcg-failures/svm-sel_cr0_lmsw_intercept.log`; the status files of the first fault.
* Rach2: nothing, as for the first fault.
* Status: unreported.
* Next: read QEMU's TCG LMSW helper for its intercept check; then report it with the command.

## QEMU's TCG ends a VMRUN after a failed event injection in #UD

* Project: QEMU 10.2.1, Ubuntu's build 1:10.2.1+ds-1ubuntu3.3, its SVM emulation for TCG.
* Fails: kvm-unit-tests' `exc_inject` injects NMI as an exception, which VMRUN refuses with VMEXIT_INVALID as it should, then injects #DE and runs the guest again; under TCG the host then takes #UD at 0x4018af, two bytes into the first instruction of the test's `init_startup_check`, so the host's RIP after that VMRUN is wrong. Linux's nested KVM passes the test. The lines of QEMU's source that do it are not yet traced.
* Found: 2026-11-10, on the testing PC under WSL, in milestone 81, task 81-6c, with the first fault.
* Reproduction: as the first fault's, with `-append exc_inject`.
* Evidence: `dev-log/81-kvm/81-6c/tcg-failures/svm-exc_inject.log`; the status files of the first fault.
* Rach2: nothing, as for the first fault.
* Status: unreported.
* Next: trace the host's state that QEMU's VMEXIT_INVALID path saves and the next VMRUN loads, with QEMU's `-d int,cpu` log; then report it with the command.

## QEMU's TCG raises no nested page fault for reserved bits

* Project: QEMU 10.2.1, Ubuntu's build 1:10.2.1+ds-1ubuntu3.3, its SVM emulation for TCG.
* Fails: with bit 8, reserved, set in a nested page table entry, the guest's access completes and the guest exits with its VMMCALL (exit 0x81), where AMD's manual gives #NPF (exit 0x400) with the error code 0x20000000f, present, write, user and reserved; with NX clear in the host's EFER, a set NX bit, reserved then, gives #NPF with 0x20000000e, without the present flag. 72 of the 128 checks of `svm_npt_rsvd_bits_test` fail; Linux's nested KVM passes all. The lines of QEMU's source that do it are not yet traced.
* Found: 2026-11-10, on the testing PC under WSL, in milestone 81, task 81-6c, with the first fault.
* Reproduction: as the first fault's, with `-kernel x86/svm_npt.flat -append svm_npt_rsvd_bits_test`.
* Evidence: `dev-log/81-kvm/81-6c/tcg-failures/svm_npt-svm_npt_rsvd_bits_test.log`; the status files of the first fault.
* Rach2: nothing; nested paging's reserved bits are proven under nested KVM on the testing PC, and Rach2's tables set no reserved bit.
* Status: unreported.
* Next: read the reserved-bit masks of QEMU's nested page walk for TCG; then report it with the command.

## QEMU's TCG leaves an intercepted interrupt pending while a nested guest runs

* Project: QEMU's SVM emulation for TCG: Homebrew's QEMU 11.1.3 on an Apple M4 host, and Ubuntu's QEMU 10.2.1 (1:10.2.1+ds-1ubuntu3.3) on x86-64 under WSL.
* Fails: with V_INTR_MASKING set and INTR intercepted, a guest of the emulated SVM that runs with its own IF clear is not ended with SVM_EXIT_INTR when the local APIC's timer raises its vector; the vector waits in the IRR while the guest runs on. AMD's manual makes the host's IF and GIF, not the guest's IF, gate a physical interrupt under V_INTR_MASKING, and an intercepted one exits at once. On the Mac it never exits in about one run of five, and the others pass in 1 s; under WSL every run takes the interrupts, at about 18 a second. The lines of QEMU's source that do it are not yet traced.
* Found: 2026-11-10, on the Mac, in milestone 81, task 81-6c, while qualifying the `max-svm` processor profile under TCG on the Mac.
* Reproduction: as the CR3 fault's, with `-append reg_corruption`; on the Mac, run it until one does not end within a few seconds, and read `info registers -a` and `info lapic` in its monitor.
* Evidence: `dev-log/81-kvm/81-6c/mac-reg-corruption.txt`; `tests/kvm/ref/svm-max-svm-tcg-mac.status`, where the test's status is 2, QEMU stopped without the test's exit.
* Rach2: Rach2's SVM backend sets V_INTR_MASKING and intercepts INTR as the test does, so a guest of Rach2's KVM that spins with its IF clear under TCG may wait on an interrupt QEMU does not deliver; 81-2's cases on `max-svm` meet it there, and nested KVM on the testing PC does not have it.
* Status: unreported.
* Next: read the interrupt check of QEMU's TCG loop for a guest in SVM (`svm_check_intercept` and the `CPU_INTERRUPT_HARD` test in `x86_cpu_exec_interrupt`) against the manual's rule, try a current QEMU, and report it with the command.

## Linux's hypercall fix-up writes at RIP without CS's base

* Project: Linux 6.12.115, `arch/x86/kvm/x86.c` (SHA-256 0a4d25f3522b54b7b51d593f987b1221ff84943be03917fff4a901290e5e8233) and `arch/x86/kvm/emulate.c` (b8af3f684d592d1eb44444735c2bb3f596ad6275f7854dd47b463e8c4a08bbd9), read from kernel.org's `linux-6.12.115.tar.xz`.
* Fails: when a guest runs the other vendor's hypercall instruction from a code segment whose base is not 0, KVM writes the three bytes of the processor's own instruction at the linear address RIP, not at CS.base + RIP where the instruction is, and the instruction that raised #UD is left as it was. `emulator_fix_hypercall` (x86.c line 9719) takes `rip = kvm_rip_read(vcpu)` and passes it to `emulator_write_emulated` (line 9738), whose address is linear; `em_hypercall` (emulate.c line 3421) then puts RIP back on the instruction for the processor to run it again. In real mode, or in protected mode with such a segment, three bytes of other memory are overwritten, the instruction raises #UD again and is patched again in the wrong place, without end.
* Found: 2026-11-10, on the Mac, in milestone 81, task 81-12, while writing Rach2's fix-up after Linux's code.
* Reproduction: a guest in real mode with CS above 0, or in 32-bit protected mode with a code segment whose base is not 0, that runs VMMCALL on an Intel processor or VMCALL on an AMD one, with KVM_X86_QUIRK_FIX_HYPERCALL_INSN enabled, as it is by default. Under Linux the bytes at the linear address RIP change and the guest never gets past the instruction. Not yet run on Linux.
* Evidence: `dev-log/81-kvm/upstream.md`; the commit "Take #UD, #DB and #AC from the guest under VMX and #UD under SVM, and rewrite the other vendor's hypercall".
* Rach2: `h_hypercall` in `rach2/sys/kvm/emulate.c` writes at CS's base plus RIP.
* Status: unreported.
* Next: run the reproduction on a current kernel as a kvm-unit-tests case, check whether a later release adds the segment's base, and report it to the KVM list with the case.
