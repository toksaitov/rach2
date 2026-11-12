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
* Fails: with V_INTR_MASKING set and INTR intercepted, a guest of the emulated SVM that runs with its own IF clear is not ended with SVM_EXIT_INTR when the local APIC's timer raises its vector; the vector waits in the IRR while the guest runs on. AMD's manual makes the host's IF and GIF, not the guest's IF, gate a physical interrupt under V_INTR_MASKING, and an intercepted one exits at once. On the Mac it never exits in about one run of five, and the others pass in 1 s; under WSL every run takes the interrupts, at about 18 a second. It is not the fault below of an x87 status load that rewrites processor 0's `hflags2`, which could drop HF2_VINTR or HF2_HIF as it drops HF2_NPT: under WSL `reg_corruption` passed in 143 s on two processors, and with one, where no other thread writes processor 0's `hflags2`, in 210 s, and in 209 s on one thread. The lines of QEMU's source that do it are not yet traced.
* Found: 2026-11-10, on the Mac, in milestone 81, task 81-6c, while qualifying the `max-svm` processor profile under TCG on the Mac.
* Reproduction: as the CR3 fault's, with `-append reg_corruption`; on the Mac, run it until one does not end within a few seconds, and read `info registers -a` and `info lapic` in its monitor.
* Evidence: `dev-log/81-kvm/81-6c/mac-reg-corruption.txt`; `tests/kvm/ref/svm-max-svm-tcg-mac.status`, where the test's status is 2, QEMU stopped without the test's exit.
* Rach2: Rach2's SVM backend sets V_INTR_MASKING and intercepts INTR as the test does, so a guest of Rach2's KVM that spins with its IF clear under TCG may wait on an interrupt QEMU does not deliver; 81-2's cases on `max-svm` meet it there, and nested KVM on the testing PC does not have it.
* Status: unreported.
* Next: read the interrupt check of QEMU's TCG loop for a guest in SVM (`svm_check_intercept` and the `CPU_INTERRUPT_HARD` test in `x86_cpu_exec_interrupt`) against the manual's rule, try a current QEMU, and report it with the command.

## QEMU's TCG lets an x87 status load on any processor clear processor 0's nested paging

* Project: QEMU's x86 emulation for TCG, Ubuntu's QEMU 10.2.1 (1:10.2.1+ds-1ubuntu3.3) on x86-64 under WSL; the lines below are QEMU v10.2.0's, `target/i386/tcg/system/fpu_helper.c` (SHA-256 7038c59597a2c4d9afc9fb351a2521a992ad4b4ff4a866a337e46e14f5804589), `target/i386/tcg/fpu_helper.c` (59eddcf1cb1aa9adc21ee3d7a2d90bbb9b5319d3a262d509c2bfab54e4cd67ff) and `target/i386/tcg/system/svm_helper.c` (f2b8c150382ac0d2aefa4af0e8d525b6320d62a8082ad9c48987eb5d7a6b68d4).
* Fails: with a thread for each processor, the default, an FLDENV, FRSTOR, FXRSTOR or XRSTOR on any processor that loads a status word without ES calls `cpu_set_fpus` (fpu_helper.c line 2500), which calls `cpu_clear_ignne` (system/fpu_helper.c line 42). That does `X86_CPU(first_cpu)->env.hflags2 &= ~HF2_IGNNE_MASK` on processor 0's state, by a plain read and write without a lock, from whichever thread runs the instruction. When processor 0 runs a VMRUN with nested paging at the same moment, its `env->hflags2 |= HF2_NPT_MASK` (svm_helper.c line 296) can be lost under the other thread's write, and the nested guest then runs with its guest-physical addresses taken as the host's; a VMEXIT's clear (line 765) can be undone the same way. A guest of the emulated SVM on processor 0 reads the wrong memory or faults; one on another processor does not meet it.
* Found: 2026-11-11, on the testing PC, in milestone 81, task 81-2b, while tracing the double faults of Rach2's i686 image as a guest of Rach2's SVM under TCG.
* Reproduction: `svm-entry race SECONDS CPU` (tests/kvm/svm-entry.c) runs a guest whose loop reads a byte through nested paging and writes it to a port, pinned to processor CPU, while a child on the other processor runs FXRSTOR without end. Under `qemu-system-x86_64 -accel tcg -smp 2 -cpu max,svm=on,npt=on` with Rach2's x86-64 kernel, `race 120 0` failed twice, with KVM_EXIT_INTERNAL_ERROR after 68 and after 188 exits, and in a third boot the outer kernel double-faulted. `race 120 1` passed with 1,864,184 exits, and `race 120 0` under `-accel tcg,thread=single` passed with 853,905.
* Evidence: `dev-log/81-kvm/upstream.md`; the commit "Add svm-entry's race mode, which runs nested paging beside FXRSTOR on another processor".
* Rach2: the etude gives a machine whose processor profile has `svm` one processor under TCG (`tools/etude/qemu_profile.c`), which has neither this fault nor the next one. `-accel tcg,thread=single` is not a way around it, for the next fault.
* Status: unreported.
* Next: report it with the reproduction and the fix, an atomic and of processor 0's `hflags2` or a request run on processor 0, and try a current QEMU.

## QEMU's TCG on one thread delivers interrupts into a nested guest that masks them

* Project: QEMU's SVM emulation for TCG, Ubuntu's QEMU 10.2.1 (1:10.2.1+ds-1ubuntu3.3) on x86-64 under WSL, with `-accel tcg,thread=single`.
* Fails: with one thread for every processor, a guest of the emulated SVM that runs with IF clear takes vector 32 from its local APIC. Rach2's i686 image under Rach2's SVM stopped with the inner guest's stack holding interrupt frames whose saved EFLAGS was 0x10002, IF clear: one at EIP 0x2bfa4d, inside `bkl_acquire`'s `k_memset`, and one at 0x2bf993, just past a `cli`. The inner local APIC held vector 32 in both the ISR and the IRR with PPR 0x20, so it could not have given that vector a second time, and an instrumented build of Rach2's SVM backend logged no injection while the guest's IF was clear. The lines of QEMU's source are not traced.
* Found: 2026-11-11, on the testing PC, in milestone 81, task 81-2b, while trying `thread=single` as a way around the fault above.
* Reproduction: boot Rach2's x86-64 GNU root under `qemu-system-x86_64 -accel tcg,thread=single -smp 2 -cpu max,svm=on,npt=on` and run `tests/kvm/kvm-boot.sh -a kvm -k guests/rach2/kernel -i guests/rach2/ram.krffs -t 'KVMRACH2 OK'` from the KVM case image several times. The inner guest hung in one run of 6, one of 2, one of 9 and one of 1, the last in SeaBIOS at f000:ff53; read `info registers` and `info lapic` in the inner QEMU's monitor. kvm-unit-tests' `reg_corruption` did not end in 700 s with `-smp 2`, where it passes in 209 s with `-smp 1` and in 143 s on a thread for each processor.
* Evidence: `dev-log/81-kvm/upstream.md`; the measurements of task 81-2b's issue.
* Rach2: as the fault above: one processor under TCG for a profile with `svm`, on a thread for each processor, the default.
* Status: unreported.
* Next: find where TCG's round-robin loop takes a hard interrupt for a processor in guest mode without the guest's IF under V_INTR_MASKING, and report it with the reproduction.

## Linux's hypercall fix-up writes at RIP without CS's base

* Project: Linux 6.12.115, `arch/x86/kvm/x86.c` (SHA-256 0a4d25f3522b54b7b51d593f987b1221ff84943be03917fff4a901290e5e8233) and `arch/x86/kvm/emulate.c` (b8af3f684d592d1eb44444735c2bb3f596ad6275f7854dd47b463e8c4a08bbd9), read from kernel.org's `linux-6.12.115.tar.xz`.
* Fails: when a guest runs the other vendor's hypercall instruction from a code segment whose base is not 0, KVM writes the three bytes of the processor's own instruction at the linear address RIP, not at CS.base + RIP where the instruction is, and the instruction that raised #UD is left as it was. `emulator_fix_hypercall` (x86.c line 9719) takes `rip = kvm_rip_read(vcpu)` and passes it to `emulator_write_emulated` (line 9738), whose address is linear; `em_hypercall` (emulate.c line 3421) then puts RIP back on the instruction for the processor to run it again. In real mode, or in protected mode with such a segment, three bytes of other memory are overwritten, the instruction raises #UD again and is patched again in the wrong place, without end.
* Found: 2026-11-10, on the Mac, in milestone 81, task 81-12, while writing Rach2's fix-up after Linux's code.
* Reproduction: a guest in real mode with CS above 0, or in 32-bit protected mode with a code segment whose base is not 0, that runs VMMCALL on an Intel processor or VMCALL on an AMD one, with KVM_X86_QUIRK_FIX_HYPERCALL_INSN enabled, as it is by default. Under Linux the bytes at the linear address RIP change and the guest never gets past the instruction. Not yet run on Linux.
* Evidence: `dev-log/81-kvm/upstream.md`; the commit "Take #UD, #DB and #AC from the guest under VMX and #UD under SVM, and rewrite the other vendor's hypercall".
* Rach2: `h_hypercall` in `rach2/sys/kvm/emulate.c` writes at CS's base plus RIP.
* Status: unreported.
* Next: run the reproduction on a current kernel as a kvm-unit-tests case, check whether a later release adds the segment's base, and report it to the KVM list with the case.

## Cppcheck calls a test of a negated 64-bit value always true or always false

* Project: Cppcheck 2.19.0, Ubuntu's build on the testing PC under WSL, which `tools/setup/judges` records, and Cppcheck 2.22.0, Homebrew's build on the Mac.
* Fails: after `lo = 0u - lo;` on an `unsigned long long` or, on an LP64 platform, an `unsigned long`, the check `knownConditionTrueFalse` says that `lo == 0` is always true, and after `lo = ~lo + 1u;` that it is always false. Neither is so: the first holds only for a `lo` of 0, and the second fails only then. With `unsigned` operands and `lo = -lo;` there is no message. The lines of Cppcheck's source that do it are not traced.
* Found: 2026-11-10, on the Mac, in milestone 81, task 81-12, when the push hook refused `h_div` in `rach2/sys/kvm/emulate.c`, which negates IDIV's 128-bit dividend and takes the carry from the low half.
* Reproduction: `cppcheck --std=c89 --platform=unix64 -q --enable=warning,style m.c` on a file that holds `typedef unsigned long long uint64_t; uint64_t f(uint64_t hi, uint64_t lo); uint64_t f(uint64_t hi, uint64_t lo) { lo = 0u - lo; if (lo == 0) { hi++; } return hi ^ lo; }` prints `Condition 'lo==0' is always true`; with `lo = ~lo + 1u;` it prints `always false`. Both versions print both.
* Evidence: `dev-log/81-kvm/upstream.md`; the commit "Take IDIV's carry from the dividend's low half before negating it, which Cppcheck misjudges after".
* Rach2: `h_div` tests the low half before it negates it, a form Cppcheck does not misjudge; no suppression was added.
* Status: unreported.
* Next: try Cppcheck's current release and its `--debug` value flow on the reproduction, search its tracker for the unsigned negation, and report it there with the file.

## Linux 6.12.115's monitor_mwait_test turns a quirk back on, which its KVM no longer does

* Project: Linux 6.12.115, `arch/x86/kvm/x86.c` (SHA-256 0a4d25f3522b54b7b51d593f987b1221ff84943be03917fff4a901290e5e8233) and `tools/testing/selftests/kvm/x86_64/monitor_mwait_test.c` (ca17d45ddd29d2d9e259e4078f80b0e857175355617444d431562127ead086aa), read from kernel.org's `linux-6.12.115.tar.xz`.
* Fails: the selftest and the kernel of one release disagree, so the selftest must fail wherever it runs. KVM_ENABLE_CAP of KVM_CAP_DISABLE_QUIRKS2 ORs its argument into the VM's disabled quirks (x86.c lines 6068 to 6079), so a quirk turned off stays off. The selftest uses one VM for its six cases and calls `vm_enable_cap(vm, KVM_CAP_DISABLE_QUIRKS2, disabled_quirks)` before each with that case's set alone (line 107), which turns a quirk back on only if the capability assigns. Its second case turns KVM_X86_QUIRK_MWAIT_NEVER_UD_FAULTS off; its third (line 61), with only KVM_X86_QUIRK_MISC_ENABLE_NO_MWAIT off and MWAIT absent from the guest's CPUID, expects MONITOR to run, but `kvm_emulate_monitor_mwait` (x86.c line 1694) finds the first quirk still off and the CPUID bit clear and raises #UD, which the guest's assertion reports.
* Found: 2026-11-11, on the Mac, in milestone 81, task 81-13, by reading both sources while settling the selftest's row in `tests/kvm/selftests.list`; Rach2's capability follows the kernel's OR.
* Reproduction: run 6.12.115's `monitor_mwait_test` on Linux 6.12.115 on a processor whose CPUID has MONITOR and MWAIT, on bare metal, since the test skips without them and a guest of Hyper-V or of KVM has none. Not yet run on Linux: the testing PC's Linux, a guest of Hyper-V, skips it (`tests/kvm/ref/linux-selftests.status`).
* Evidence: the two sources at the lines named above; the row of `x86_64/monitor_mwait_test` in `tests/kvm/selftests.list`, which expects the failure on a processor with `monitor`.
* Rach2: nothing in the kernel, whose KVM_CAP_DISABLE_QUIRKS2 keeps Linux's OR; the list's row expects a failure where the processor has MONITOR and MWAIT and a skip where it does not.
* Status: unreported.
* Next: run the reproduction on the Lenovo's Debian or another bare-metal Linux 6.12.115, then find the stable commit that made the capability OR its argument and whether mainline changed the selftest with it, and report the missing backport to the stable and KVM lists.

## Linux's KVM drops the Guest-only and Host-only bits of an AMD guest's event selectors

* Project: Linux 6.12.115, `arch/x86/kvm/svm/pmu.c` (SHA-256 c3a5b427722428f54b0d8c244656d7e219e9af7d88e6d66400ec59adbf9638ed) and `arch/x86/kvm/pmu.c` (c6db7f19b51fb0882475a22449686ef126fb1177c7a245b91a447e1d331d08f3), read from kernel.org's `linux-6.12.115.tar.xz`. The testing PC's WSL kernel, 6.18.58, beneath which the fault was measured, was not read.
* Fails: a hypervisor that runs in a guest of Linux's KVM cannot restrict its counters to its own guests, so they count its own work too. `amd_pmu_refresh` makes bits 36 to 63 of PERF_CTL reserved (svm/pmu.c line 208), among them Guest-only, bit 40, and Host-only, bit 41, and `amd_pmu_set_msr` clears them from every write (line 166). The perf event behind the counter is `exclude_host` alone (pmu.c line 187), which counts everything the guest runs, its own hypervisor's code and that hypervisor's guests alike.
* Found: 2026-11-11, on the testing PC, in milestone 81, task 81-9, comparing kvm-unit-tests' `pmu` under Rach2's KVM, itself under WSL's KVM, with Linux at the same depth.
* Reproduction: in a guest of WSL's KVM with `-cpu host`, Debian 13's Linux 6.12.115 runs kvm-unit-tests' `pmu.flat` (SHA-256 a8244678e9743de87f023a6ee18a049a9be1ce9300ba6a98282914ed1910a681) with `ACCEL=kvm,kernel-irqchip=split`: nearly every instruction count of a 10,000,000-instruction loop is over the test's bound of 10,200,000, at 10,205,211 to 10,283,110, and 21 or 22 of 77 subtests fail in five runs, where WSL's Linux running the same test directly fails only 7 to 10 overflows. Rach2's KVM at that depth reads 10,396,086 to 10,767,419 and fails 30 or 31 in two runs, with 30,781 exits in the test's 16.7 s against Linux's 8,800 in 6.3 s. Whether each exit's handling beneath is counted by WSL's KVM or by Hyper-V below it is not yet measured. The excess grows with the exits a measured loop takes: on 2026-11-12, in 81-25, Rach2 with its 1000 Hz tick took 4,548 interrupt exits in the test, read most instruction counts 3.7 to 5.7% high and failed 26 subtests, and with a 200 Hz tick took 624, read most 2.2 to 2.4% high and failed 19.
* Evidence: `dev-log/81-kvm/81-9/linux-wsl-pmu.*.log`, `linux-nested-pmu.log` and `rach2-nested-pmu.log`; `dev-log/81-kvm/81-9.md`; `dev-log/81-kvm/81-25/rach2-pmu-1000hz.txt`, `rach2-pmu-200hz.txt` and `linux-pmu.txt`; `dev-log/81-kvm/81-25.md`.
* Rach2: Rach2's KVM keeps every counter stopped while it switches them, so a guest counter counts as little of Rach2 as the processor's VMRUN allows; `tests/kvm/unit-tests.list` expects `pmu` to fail under a hypervisor, as Linux's does.
* Status: unreported.
* Next: read current mainline's `svm/pmu.c` for Host-only and Guest-only support, measure which layer counts the handling of an exit by running the reproduction under a Linux KVM on bare metal, and report to the KVM list if mainline still drops the bits.

## Linux's Intel PMI handler skips its checkpointed counters when a rollback left no status

* Project: Linux 6.12.115, `arch/x86/events/intel/core.c` (SHA-256 46ec7a496c3cc423ffbee0fa62f9537c9f7b7afa682434895227656ece1953e9) and `arch/x86/kernel/nmi.c` (5dfdc85e3242e180e440d058f448de233d1ec71a6fa869a674e1bc94f3a31c5e), read from kernel.org's stable tree at tag v6.12.115.
* Fails: the PMI of a counter counting a TSX-checkpointed event that overflowed alone is reported as an NMI of unknown reason. The handler's comment says checkpointed counters must always be probed, since the rollback the PMI causes clears the overflow bit, and it ORs `intel_cp_status` into the status (core.c lines 3119 to 3124); but `intel_pmu_handle_irq` reads the status first and jumps to `done` when it is 0 (lines 3183 to 3185), so the probe never runs and the handler returns 0. `default_do_nmi` then calls `unknown_nmi_error` (nmi.c line 475), which prints "Uhhuh. NMI received for unknown reason" and goes on, or panics under `unknown_nmi_panic` (lines 331 to 357). KVM gives a guest's checkpointed event no sample period (`arch/x86/kvm/pmu.c` lines 196 to 202), so perf counts it to its maximum period with the interrupt bit set (`arch/x86/events/core.c` lines 487 to 488 and 618), and its overflow raises such a PMI.
* Found: 2026-11-12, on the testing PC, in milestone 81, task 81-9, by reading Linux's handler while making Rach2's KVM claim a checkpointed guest counter's PMI (81-9.49).
* Reproduction: not yet run. It needs a processor with TSX enabled, a counter on counter 2 with `in_tx_cp=1` whose overflow comes inside a transaction that then aborts, and no other overflow in the same NMI; the kernel log should then show the unknown-reason NMI.
* Evidence: the sources at the lines named above; `rach2/sys/kvm/pmu.c`'s `kvm_pmu_nmi` at 889e192bf.
* Rach2: not affected. Its KVM sets no interrupt bit on a guest's counters, so no guest counter raises a PMI in the host; the claim `kvm_pmu_nmi` made for a checkpointed counter went with the measuring knob after 81-9's third Lenovo trial.
* Status: unreported.
* Next: read current mainline's `intel_pmu_handle_irq` for the early return, run the reproduction on a TSX processor, and report to the perf list if mainline still returns before the probe.

## QEMU's TCG aborts an interrupt from ring 3 when TR's cached type is busy

* Project: QEMU 10.2.1, Ubuntu's build 1:10.2.1+ds-1ubuntu3.3, its TCG; `target/i386/tcg/seg_helper.c` (SHA-256 0884529c19553a636783ca0859c86f2b6af226a883c08b756d683b645b5c7c04) and `target/i386/tcg/system/svm_helper.c` (f2b8c150382ac0d2aefa4af0e8d525b6320d62a8082ad9c48987eb5d7a6b68d4), read from the v10.2.1 tag.
* Fails: QEMU stops the whole machine with `qemu: fatal: invalid tss type` when a 32-bit guest takes an interrupt or exception at CPL 3 while TR's cached type is a busy TSS, 0xB, which is the type a real processor holds in TR after LTR. `get_ss_esp_from_tss` (seg_helper.c line 238), called from `do_interrupt_protected` (line 784), aborts unless the type's low three bits are 1, an available TSS. QEMU's own LTR (`helper_ltr`, lines 1381 to 1388) loads the cache from the descriptor before it sets the busy bit, so a guest that only ever loads TR itself keeps type 9 and never meets the check. VMLOAD (`helper_vmload`, svm_helper.c lines 503 and 504) loads TR's attributes from the VMCB as they are, and Linux's KVM reports TR as busy (`svm_get_segment`, arch/x86/kvm/svm/svm.c of Linux 6.12, `var->type |= 0x2`), so a VMCB that Linux's KVM writes back from KVM_SET_SREGS brings type 0xB into QEMU's cache.
* Found: 2026-11-11, on the testing PC under WSL, in milestone 81, task 81-7.3, while counting the exits of the i686 Rach2 guest under Linux 6.12.107 (Debian's cloud kernel) and its KVM, inside QEMU's TCG with the `max-svm` profile and one processor. The dump at the abort has CPL 3, EFER 0x1000, `TR =0028 0065ecc0 00000067 00008b00 DPL=0 TSS32-busy` and EIP 0x0804bb23, a user program of the guest.
* Reproduction: `sh tests/kvm/guests/mklinuxhost.sh OUT`, then boot OUT's kernel and initramfs as `tests/kvm/Readme.md` describes, under `qemu-system-x86_64 -accel tcg -machine q35 -cpu max,rdrand=on,rdseed=on,xsave=on,avx=on,svm=on,npt=on -smp 1 -m 4096`; the third boot, the Rach2 guest's, ends the outer QEMU with status 134. The two boots of the 64-bit Debian guest before it run to their end, since a 64-bit interrupt reads its stack through `get_rsp_from_tss`, which has no such check.
* Evidence: `dev-log/81-kvm/81-7.md`, which holds the dump; `tests/kvm/guests/mklinuxhost.sh` and `tests/kvm/guests/linux-host-init`.
* Rach2: nothing. Rach2's KVM reports TR as busy in KVM_GET_SREGS as Linux does; the same Rach2 guest under Rach2's KVM in the same outer QEMU reached its end, and which of the two hosts' paths keeps type 0xB out of the VMCB there is not yet traced. The Rach2 guest's counts under Linux are left to the Lenovo, whose processor has no such check.
* Status: unreported.
* Next: check whether a later QEMU accepts a busy type in `get_ss_esp_from_tss` or loads TR with the busy bit in `helper_ltr` as hardware does; then report it with a kvm-unit-tests case that VMRUNs a 32-bit guest whose VMCB TR is type 0xB and takes an interrupt at CPL 3.
