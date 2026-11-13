# Upstream faults of milestone 84

Faults this milestone found in another project's code, one section each, in the format of [dev-log/upstream.md](../upstream.md), which indexes them.

## util-linux's cross build takes ncurses' flags from the build host's ncursesw6-config

* Project: util-linux 2.42.4, `util-linux-2.42.4.tar.xz`, SHA-256 fbd62a100ab7bb8746ba0661255c3c48185b1e9021507c624da01fbc696330ec.
* Fails: `UL_NCURSES_CHECK` in `m4/ul.m4` (lines 525-536) looks for `ncursesw6-config` with `AC_CHECK_TOOL`, which in a cross build falls back to the unprefixed name on the build host's path, and asks that script for `--libs` and `--cflags` before it tries pkg-config (lines 551-559), which reads the target's files. `text-utils/Makemodule.am` (lines 85-91) then links `ul` and `more` with those flags. On a Debian or Ubuntu build host with `libncurses-dev`, the script answers `-lncursesw -ltinfo`, since Debian builds ncurses with a separate libtinfo; a target ncurses built without one has no `libtinfo.so`, and the link fails with `cannot find -ltinfo`. When the two answers happen to agree, the build passes with the host's flags.
* Found: 2026-11-12, the testing PC, milestone 84, task 84-29, in the release rehearsal's i686 GNU ports, the first build after `libncurses-dev` was installed on the testing PC on 2026-11-11.
* Reproduction: on a host with `libncurses-dev`, cross-configure util-linux 2.42.4 against a sysroot whose ncurses 6.6 is built with its default single library and `--enable-pc-files`, without `NCURSESW6_CONFIG` set, and run make: `ul` fails to link with `cannot find -ltinfo`.
* Evidence: `vendor/gnu/ports/util-linux.sh`; the rehearsal's log, `build/runs/reh-d979.log` in the testing account on the testing PC, around line 58839.
* Rach2: the recipe sets `NCURSESW6_CONFIG` to the root's own script, `$ROOT/usr/bin/ncursesw6-config`, which answers `-lncursesw`.
* Status: unreported.
* Next: read util-linux's master to see whether `UL_NCURSES_CHECK` still prefers an unprefixed config script in a cross build, then propose that a cross build try pkg-config first or accept only a host-prefixed script.

## Linux's nested SVM emulates a nested guest's VMCALL under its own guest's quirk

* Project: Linux 6.12.115, `linux-6.12.115.tar.xz` (SHA-256 0ec90fe4b20422b1b664b77e6d2dde22b40bd41e7eb2cad8ac8739a4749cb1bb), `arch/x86/kvm/svm/svm.c`, `arch/x86/kvm/svm/nested.c` and `arch/x86/kvm/x86.c`; the testing PC runs WSL's Linux 6.18.58, whose source Rach2 does not keep.
* Fails: a #UD that a nested guest takes on VMCALL, on an AMD processor, never reaches the hypervisor between, which intercepts #UD; KVM takes it as its own, emulates the VMCALL and, with KVM_X86_QUIRK_FIX_HYPERCALL_INSN enabled for its own guest, writes VMMCALL over the nested guest's instruction. `init_vmcb` intercepts #UD in every vmcb01 (`svm.c:1248`); `nested_svm_exit_special` keeps an exception exit for the host whenever vmcb01 intercepts it (`nested.c:1540-1545`), before vmcb12's intercepts are asked; `ud_interception` runs `handle_ud`, which emulates the instruction (`x86.c:7313-7337`), and `emulator_fix_hypercall` consults the quirk of the VM it runs, the outer one, and patches the instruction at the nested guest's RIP (`x86.c:9719-9740`). The patched VMMCALL then exits to the hypervisor between as a hypercall.
* Found: 2026-11-12, on the testing PC under WSL's KVM, in milestone 84, task 84-33, when the KVM suite's `selftests` case ran `x86_64/fix_hypercall_test` under Rach2's SVM and its `disable_quirk` failed at `fix_hypercall_test.c:75`, `ret == -EFAULT`; Debian's Linux 6.12.107 as the hypervisor between fails it the same way (dev-log/81-kvm/81-26.md), and WSL's Linux 6.18.58 one level up, with no hypervisor between, passes it.
* Reproduction: on an AMD processor under Linux's KVM, run a Linux or Rach2 guest with nested SVM, and in it Linux 6.12.115's `x86_64/fix_hypercall_test`: `disable_quirk` reads 0, not -EFAULT, and the instruction is VMMCALL afterwards. `enable_quirk` passes, which hides the fault.
* Evidence: `dev-log/84-locks/84-33.md`; the row of `x86_64/fix_hypercall_test` in `tests/kvm/selftests.list` and in `tests/kvm/selftests-gap.list`.
* Rach2: nothing in its KVM, whose `h_hypercall` raises #UD with the quirk disabled (`rach2/sys/kvm/emulate.c`); `selftests.list` expects the test to fail where the machine runs under KVM (`outer_kvm`) and to pass elsewhere.
* Status: unreported.
* Next: read `nested_svm_exit_special` and `handle_ud` in a current kernel to see whether a later release reflects such a #UD to the hypervisor between when vmcb12 intercepts it, or skips the patch in guest mode; check VMX's `nested_vmx_l0_wants_exit` for the same; then report it to the KVM list with the selftest as the reproduction.

## Linux's max_guest_memory_test stops on a machine of one processor

* Project: Linux 6.12.115, `linux-6.12.115.tar.xz` (SHA-256 0ec90fe4b20422b1b664b77e6d2dde22b40bd41e7eb2cad8ac8739a4749cb1bb), `tools/testing/selftests/kvm/max_guest_memory_test.c`.
* Fails: on one processor the test stops with `Uh, no CPUs?` before it tests anything, whatever `-c` says. `calc_default_nr_vcpus` sets the default vCPU count to three quarters of the processors in the affinity mask, rounded down, and asserts that it is positive (lines 148-149); `main` calls it at line 186, before `getopt` reads `-c`, so one processor gives 0 and the assertion.
* Found: 2026-11-12, by Codex's review j49 of milestone 84's branch, reading task 84-33's rows against `tools/etude/qemu_profile.c`, whose `machine_processors` gives an SVM profile one processor under TCG, as the KVM suite's cases have on the Mac.
* Reproduction: run `max_guest_memory_test -m 4` under Linux's KVM with its affinity limited to one processor, `taskset -c 0`; it stops at line 149. Not yet run.
* Evidence: `vendor/gnu/kvm/max-guest-memory-one-cpu.patch`; `dev-log/84-locks/84-33.md`; the row of `max_guest_memory_test,-m,4` in `tests/kvm/selftests.list`.
* Rach2: the KVM layer builds the selftests with `vendor/gnu/kvm/max-guest-memory-one-cpu.patch`, which makes the default at least one vCPU and keeps the assertion for an empty mask; Linux's reference runs the same patched program from the same root.
* Status: unreported.
* Next: read `max_guest_memory_test.c` in a current kernel to see whether the default still rounds one processor down to none, then send the patch to the KVM list.

## Linux lowers a link whose speed it has not seen to a reserved SPD

* Project: Linux 6.12.115, `linux-6.12.115.tar.xz` (SHA-256 0ec90fe4b20422b1b664b77e6d2dde22b40bd41e7eb2cad8ac8739a4749cb1bb), `drivers/ata/libata-sata.c`, `drivers/ata/libata-core.c` and `drivers/ata/libata-eh.c`.
* Fails: the first speed step down of a SATA link whose speed is unknown writes SControl.SPD 15, a reserved value, and not a lower speed. `sata_link_init_spd` leaves `hw_sata_spd_limit` at UINT_MAX when SControl.SPD is 0, as firmware leaves it (`libata-core.c:5496` and 5534-5540). When a reset fails with -EPIPE, `ata_eh_reset` calls `sata_down_spd_limit` (`libata-eh.c:3020-3025`), which takes the speed from SStatus only while DET reads 3 and otherwise from `link->sata_spd`, 0 before any reset has succeeded (`libata-sata.c:551-555`); with no speed known it only clears the limit's top bit, which its comment intends as a step down from the fastest speed allowed, but the top bit of UINT_MAX is no speed, and the limit becomes 0x7FFFFFFF (`libata-sata.c:561-577`). `__sata_set_spd_needed` then writes `fls(limit) & 0xf`, 31 & 15 = 15, to SControl.SPD (`libata-sata.c:455-461`), and `sata_link_hardreset` applies it (`libata-sata.c:634-646`); a second such step writes 14. SATA and AHCI 1.3.1 (3.3.11) define SPD 0 to 3 only.
* Found: 2026-11-12, the testing PC, milestone 84, task 84-96, reading Linux's link recovery while porting it to Rach2's AHCI driver after Codex's review j53.
* Reproduction: not yet run; it needs a link that flaps past the debounce's 2 s deadline and reads DET other than 3 when `sata_down_spd_limit` reads SStatus, under a kernel with SControl.SPD 0 at boot, and a print of SControl after the next hardreset.
* Evidence: `rach2/sys/block/ahci.c` (`ahci_link_slow`); Codex's review j53, `build/flow/manager/review-161/j53/sol.md` in the main checkout on the Mac.
* Rach2: `ahci_links_reset` starts the limit at the controller's speeds, CAP.ISS, so a step with no speed known drops the fastest of those.
* Status: unreported.
* Next: read `sata_down_spd_limit` in a current kernel to see whether it still lets the top bit of UINT_MAX stand for a step down, confirm on hardware or QEMU what SControl reads after such a step, then propose that the limit start at the host's supported speeds or that the step return when no speed is known.

## QEMU aborts in iotlb_to_section while Venus runs Vulkan under TCG

* Project: QEMU 10.2.1 (Ubuntu's 1:10.2.1+ds-1ubuntu3.3) with virglrenderer 1.10.0, on Ubuntu in WSL.
* Fails: QEMU stops on its own assertion, `system/physmem.c:760: iotlb_to_section: Assertion 'section_index < d->map.sections_nb' failed`, while a guest runs dEQP's Vulkan cases through virtio-gpu's Venus contexts under TCG; virglrenderer's render server then reports each Venus context destroyed with a valid instance. The section index a TCG translation kept no longer names a section of the address space's current map; which change of the map, such as a Venus blob region added or removed, races with it is not yet known.
* Found: 2026-11-13, on the testing host's WSL, in milestone 84's release run at 08c7795ac: four of the six cases of `tests/run venus` (fraction-1, 2, 4 and 5 on x86-64) ended with "Rach2 SSH client: the machine or its transport was lost" while the guest's console showed dEQP running normally.
* Reproduction: `tests/run venus` on WSL under TCG lost four of six cases; a load that makes the abort certain is still needed.
* Evidence: `gnu.1.qemu.stderr` of each case kept under `/home/ghrunner/rach2-ci-2/_work/_temp/full-results.9NfifA/kept/` on WSL.
* Rach2: nothing in Rach2; the release leaves `x86_64:venus` out of WSL's rerun, and issue 4972 takes it up in milestone 85.
* Status: unreported.
* Next: force the abort with tracing, check it against current QEMU, and find whether KVM or QEMU's fix ends it.

## QEMU's TCG aborts an interrupt from CPL 3 through a busy 32-bit TSS

* Project: QEMU, Homebrew's build on the Mac, 11.1.3 by `tests/kvm/Readme.md`; the run's evidence does not record its version. QEMU's master at 11.1.90, read on 2026-11-13, has the same code. `target/i386/tcg/seg_helper.c`.
* Fails: an interrupt or exception that moves a 32-bit guest from CPL 3 to CPL 0 stops QEMU with `qemu: fatal: invalid tss type` when the TR it caches has the busy type, 11, which the processor's TR holds after every LTR. `get_ss_esp_from_tss` aborts unless `(type & 7) == 1` (lines 237-240), which only the available types 1 and 9 satisfy. QEMU's own LTR caches the descriptor before it sets the busy bit (lines 1395-1402) and its task switch clears the bit in the cache (line 515), so TCG alone never caches type 11; VMLOAD does, since it loads TR's attributes from the VMCB as they are (`target/i386/tcg/system/svm_helper.c`, line 505). Long mode's `get_rsp_from_tss` checks only the present bit (lines 930-932), so a 64-bit guest is not affected.
* Found: 2026-11-13, the Mac, milestone 84's release run at 08c7795ac, task 84-230: `machine.kvm.boot-rach2-kvm` on x86-64, the x86-64 Rach2 under TCG running the i686 Rach2 guest in Debian's QEMU under its KVM, lost its transport when the outer QEMU aborted, with the nested guest at CPL 3 and TR `0028 00740780 00000067 00008b00 ... TSS32-busy`.
* Reproduction: under QEMU's TCG with SVM, have a hypervisor write type 11 into a 32-bit guest's VMCB TR, VMLOAD and VMRUN it, and let a timer interrupt arrive while the guest runs at CPL 3. Rach2's KVM wrote that TR before its fix, whenever any part of the special registers was written back; Linux's writes it whenever KVM_SET_SREGS hands back the busy TR that `svm_get_segment` reports.
* Evidence: `gnu.1.qemu.stderr` and `gnu.1.console` under `build/machines/mac/runs/milestone21-08c7795ac733-mac-1113-130411-d5f552/failed-evidence/milestone21-08c7795ac733-mac-1113-130411-d5f552.machine.kvm.boot-rach2-kvm.x86_64.r0/evidence/` on the Mac.
* Rach2: `rach2/sys/kvm/svm.c` keeps the TR attributes VMSAVE stored when the record's TR is still the one the last exit reported, so that only a TR the caller changed reaches the VMCB, as Linux writes TR only through `svm_set_segment`.
* Status: unreported.
* Next: reproduce it with Linux's KVM as the hypervisor under TCG, through a KVM_GET_SREGS and KVM_SET_SREGS of a running 32-bit guest, then report that `get_ss_esp_from_tss` should take types 3 and 11, as the processor does, for example by testing `(type & 5) == 1`.
