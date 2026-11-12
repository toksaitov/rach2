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
