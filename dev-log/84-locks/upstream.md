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

* Project: QEMU 10.2.1 (Ubuntu's 1:10.2.1+ds-1ubuntu3.3) with virglrenderer 1.10.0, on Ubuntu in WSL; read at the tag `v10.2.1`. `accel/tcg/cputlb.c`, `system/physmem.c`, `hw/display/virtio-gpu-virgl.c`.
* Fails: under TCG, a vCPU's I/O access through a TLB entry filled before a change of the memory map looks the entry's section index up in the new map, which may have fewer sections. A TLB fill keeps the index of the section in the address space's map of that moment (`physmem.c` lines 695 and 773, `cputlb.c` lines 1092 and 1156); an I/O access finds the section by that index in the map current at the access (`cputlb.c` line 1278, `iotlb_to_section` in `physmem.c` lines 751-766). A commit of the map queues a TLB flush for each vCPU with `async_run_on_cpu` (`tcg_commit`, `physmem.c` lines 3081-3105), which runs only when that vCPU next leaves its translated code. Venus's blob unmap deletes the blob's subregion from virtio-gpu's host-memory BAR (`virtio-gpu-virgl.c` lines 187-188), so the map loses a section and those after it are renumbered; a vCPU that touches an I/O page before its flush runs then fails `section_index < d->map.sections_nb` (line 760), or, with an index still in range, reaches another section than the one it filled. Under KVM nothing calls `iotlb_to_section`. QEMU's `854cd16e31` ("accel/tcg: Fix iotlb_to_section() for different AddressSpace", first in v11.0.0) keeps the section's pointer in the TLB entry and removes `iotlb_to_section`, which ends this lookup; its message names another cause, sections of an IOMMU's address space.
* Found: 2026-11-13, on the testing host's WSL, in milestone 84's release run at 08c7795ac: four of the six cases of `tests/run venus` (fraction-1, 2, 4 and 5 on x86-64) ended with "Rach2 SSH client: the machine or its transport was lost" while the guest's console showed dEQP running normally. Again on 2026-11-14 in the release run at dc8ee3986, in seven of its eight fractions.
* Reproduction: `RACH2_QEMU_ACCEL=tcg ARCH=x86_64 RACH2_CASES=machine.venus.fraction-1 tests/run venus` on the testing host's WSL, 8 CPUs and 4 dEQP processes in the guest, aborts within 3 to 10 minutes: five fractions of five on 2026-11-14 at f2822dbbb. With QEMU tracing virtio-gpu's blob commands (`-trace enable=virtio_gpu_cmd_res_create_blob`, `..._map_blob`, `..._unmap_blob`, `..._res_unref`, `-D FILE`), each of two runs aborted within 5 ms of the first step of a `RESOURCE_UNMAP_BLOB` (`finish_unmapping 0`, the subregion's deletion), after 5,922 and 2,388 unmaps, without the second step, which otherwise follows in some 12 ms. Debian's reference guest in the same QEMU under TCG gives no comparison: its first `vulkaninfo` stayed blocked in `virtio_gpu_vram_mmap` for over 360 s, with QEMU idle, after its last traced command, the release of blob 5, which the guest had mapped, unmapped and freed.
* Also: an index still in range loses a write. In four runs of `machine.venus.piglit-quick-gl` under TCG on 2026-11-14, the guest's notify of the control queue after a RESOURCE_MAP_BLOB never reached virtio-gpu, and the queue stopped for good ([84-260n.md](84-260n.md#the-lost-notify)).
* Evidence: [4972/](4972/): `tcg-runs.out`, the runs' lines; `tcg-1` and `tcg-2`, each traced run's QEMU standard error and the last 40 lines of its trace; `debian-tcg-console.out`, the Debian guest's hung tasks. The first appearance's `gnu.1.qemu.stderr` of each case kept under `/home/ghrunner/rach2-ci-2/_work/_temp/full-results.9NfifA/kept/` on WSL.
* Rach2: nothing in Rach2's code. `tools/ci/kvm-suites` runs the release's `venus` suite under KVM, as its Debian references were taken (`tests/graphics/ref/venus-wsl`), in 20388c1e1.
* Status: fixed upstream in QEMU 11.0.0 by `854cd16e31`, which is not reported as a fix of this race; not reported.
* Next: run the reproduction under TCG with QEMU 11.0 or later to confirm that `854cd16e31` ends the abort, then report the race against 10.2's stable branch with the trace.

## QEMU's TCG aborts an interrupt from CPL 3 through a busy 32-bit TSS

* Project: QEMU, Homebrew's build on the Mac, 11.1.3 by `tests/kvm/Readme.md`; the run's evidence does not record its version. QEMU's master at 11.1.90, read on 2026-11-13, has the same code. `target/i386/tcg/seg_helper.c`.
* Fails: an interrupt or exception that moves a 32-bit guest from CPL 3 to CPL 0 stops QEMU with `qemu: fatal: invalid tss type` when the TR it caches has the busy type, 11, which the processor's TR holds after every LTR. `get_ss_esp_from_tss` aborts unless `(type & 7) == 1` (lines 237-240), which only the available types 1 and 9 satisfy. QEMU's own LTR caches the descriptor before it sets the busy bit (lines 1395-1402) and its task switch clears the bit in the cache (line 515), so TCG alone never caches type 11; VMLOAD does, since it loads TR's attributes from the VMCB as they are (`target/i386/tcg/system/svm_helper.c`, line 505). Long mode's `get_rsp_from_tss` checks only the present bit (lines 930-932), so a 64-bit guest is not affected.
* Found: 2026-11-13, the Mac, milestone 84's release run at 08c7795ac, task 84-230: `machine.kvm.boot-rach2-kvm` on x86-64, the x86-64 Rach2 under TCG running the i686 Rach2 guest in Debian's QEMU under its KVM, lost its transport when the outer QEMU aborted, with the nested guest at CPL 3 and TR `0028 00740780 00000067 00008b00 ... TSS32-busy`.
* Reproduction: under QEMU's TCG with SVM, have a hypervisor write type 11 into a 32-bit guest's VMCB TR, VMLOAD and VMRUN it, and let a timer interrupt arrive while the guest runs at CPL 3. Rach2's KVM wrote that TR before its fix, whenever any part of the special registers was written back; Linux's writes it whenever KVM_SET_SREGS hands back the busy TR that `svm_get_segment` reports.
* Evidence: `gnu.1.qemu.stderr` and `gnu.1.console` under `build/machines/mac/runs/milestone21-08c7795ac733-mac-1113-130411-d5f552/failed-evidence/milestone21-08c7795ac733-mac-1113-130411-d5f552.machine.kvm.boot-rach2-kvm.x86_64.r0/evidence/` on the Mac.
* Rach2: `rach2/sys/kvm/svm.c` keeps the TR attributes VMSAVE stored when the record's TR is still the one the last exit reported, so that only a TR the caller changed reaches the VMCB, as Linux writes TR only through `svm_set_segment`.
* Status: unreported.
* Next: reproduce it with Linux's KVM as the hypervisor under TCG, through a KVM_GET_SREGS and KVM_SET_SREGS of a running 32-bit guest, then report that `get_ss_esp_from_tss` should take types 3 and 11, as the processor does, for example by testing `(type & 5) == 1`.

## QEMU's TCG loads non-canonical bases without a #GP

* Project: QEMU 11.1.3, Homebrew's build on the Mac, read at the tag `v11.1.3`; 10.2.0 has the same WRMSR code. `target/i386/tcg`.
* Fails: in 64-bit mode a processor raises #GP for a WRMSR of a non-canonical FS_BASE or GS_BASE and for a non-canonical base loaded by LGDT, LIDT, LLDT or LTR. TCG stores each unchecked: WRMSR in `system/misc_helper.c` (lines 222-227), LGDT and LIDT in `translate.c` (lines 3029-3059), the upper half of a 64-bit LDT or TSS descriptor in `seg_helper.c` (lines 1326-1330 and 1383-1391).
* Found: 2026-11-13, the Mac, milestone 84's release run at 08c7795ac, task 84-239: kvm-unit-tests' `msr`, `msr64` and `la57` failed in a guest of Rach2's KVM, which passes these MSRs through and intercepts no descriptor-table load, as Linux's SVM does.
* Reproduction: under TCG, in 64-bit mode at CPL 0, run `wrmsr` to 0xC0000100 with 0xaaaaaaaaaaaaaaaa, or `lgdt` of a pseudo-descriptor with that base; neither faults. kvm-unit-tests' `msr` and `la57` under `-accel tcg` show it.
* Evidence: [84-239.md](84-239.md); the case logs of `machine.kvm.unit-tests` under `build/machines/mac/runs/milestone21-08c7795ac733-mac-1113-130411-d5f552/failed-evidence/` on the Mac.
* Rach2: nothing; the rows of `tests/kvm/unit-tests.list` expect the failure under another hypervisor.
* Status: unreported.
* Next: run kvm-unit-tests' `msr` under `-accel tcg` with QEMU's master to confirm, then report.

## QEMU's TCG raises #GP for a non-canonical stack access

* Project: QEMU 11.1.3, Homebrew's build on the Mac, read at the tag `v11.1.3`; 10.2.0 has the same code. `target/i386/tcg/system/excp_helper.c`.
* Fails: a non-canonical access through SS, as by RSP or RBP, raises #SS(0) on the processor. TCG's translation raises #GP for every non-canonical address, whatever the segment (lines 589-598).
* Found: 2026-11-13, the Mac, milestone 84's release run at 08c7795ac, task 84-239: kvm-unit-tests' `emulator` failed `non-canonical rsp-based access, should #SS(0), got #GP(0)` and the same for RBP in a guest of Rach2's KVM.
* Reproduction: under TCG, in 64-bit mode, run `orq $0, (%rsp,%rax,1)` with RAX making the address non-canonical; the vector is 13, not 12. kvm-unit-tests' `emulator` under `-accel tcg` shows it.
* Evidence: [84-239.md](84-239.md); the case log as above.
* Rach2: nothing; the row of `emulator` expects the failure under another hypervisor.
* Status: unreported.
* Next: check QEMU's master and its bug tracker for a report, then report.

## QEMU's TCG takes XSETBV's intercept before CR4.OSXSAVE's #UD

* Project: QEMU 11.1.3, Homebrew's build on the Mac, read at the tag `v11.1.3`; 10.2.0 has the same code. `target/i386/tcg`.
* Fails: with CR4.OSXSAVE clear, XSETBV raises #UD, which on the processor takes priority over SVM's XSETBV intercept. TCG checks the intercept when it translates the instruction (`translate.c`, lines 2906-2911) and the #UD only in `helper_xsetbv` (`fpu_helper.c`, lines 3219-3222), so the hypervisor sees an exit, and Linux's KVM, which leaves that #UD to the processor (`arch/x86/kvm/x86.c:1304-1311`), sets XCR0.
* Found: 2026-11-13, the Mac, milestone 84's release run at 08c7795ac, task 84-239: kvm-unit-tests' `xsave` failed `Write XCR0=(FP|SSE) without XSAVE support - expect #UD` in a guest of Rach2's KVM.
* Reproduction: under TCG with SVM, intercept XSETBV in a guest's VMCB and have the guest clear CR4.OSXSAVE and run XSETBV; the hypervisor gets exit 0x8D.
* Evidence: [84-239.md](84-239.md); the case log as above.
* Rach2: nothing; Rach2's `kvm_emul_xsetbv` follows Linux's, and the row of `xsave` expects the failure under another hypervisor.
* Status: unreported.
* Next: check QEMU's master, then report with the reproduction.

## QEMU's TCG checks INT3 against SVM's INTn intercept

* Project: QEMU 11.1.3, Homebrew's build on the Mac, read at the tag `v11.1.3`; 10.2.0 has the same code. `target/i386/tcg/excp_helper.c`.
* Fails: on the processor INT3 raises #BP as an exception, which SVM's exception intercept for vector 3 catches, and the INTn intercept does not apply to it. TCG raises INT3 through `raise_interrupt` as a software interrupt (line 120), and `raise_interrupt2` checks a software interrupt against the INTn intercept alone (lines 100-106), so a hypervisor that intercepts #BP never sees the guest's INT3.
* Found: 2026-11-13, the Mac, milestone 84's release run at 08c7795ac, task 84-239: Linux's selftest `x86_64/debug_regs` failed with the guest's own `Unhandled exception '0x3'` at its `sw_bp`, under Rach2's KVM, which sets the #BP intercept for KVM_GUESTDBG_USE_SW_BP as Linux's `svm.c:1979` does.
* Reproduction: under TCG with SVM, set the exception intercept of vector 3 in a guest's VMCB and have the guest run INT3; the guest's IDT handler runs and no exit comes.
* Evidence: [84-239.md](84-239.md); the case log of `machine.kvm.selftests` under the run's failed evidence on the Mac.
* Rach2: nothing; the row of `x86_64/debug_regs` expects the failure under another hypervisor.
* Status: unreported.
* Next: check QEMU's master, then report with the reproduction.

## libdrm's modetest reports DIRTYFB's error in place of SETCRTC's

* Project: libdrm 2.4.124, Debian 13's `libdrm-tests`, whose `modetest` the GNU root stages; Rach2 keeps no copy of its source.
* Fails: when `modetest -s` cannot set a mode, it prints `failed to set mode:` with the `errno` of a DRM_IOCTL_MODE_DIRTYFB made after the failed DRM_IOCTL_MODE_SETCRTC, so on a driver without a dirty hook, such as Rach2's bochs-drm, it reports `Function not implemented` (ENOSYS) whatever SETCRTC said. The call is in `set_mode` of `tests/modetest/modetest.c`; its line is missing from the records.
* Found: 2026-11-13, the testing host's WSL, milestone 84, task 84-244: `machine.modetest.std-vga` on x86-64 printed `failed to set mode: Function not implemented` for both 1920x1080 modes, while a log line added to Rach2's SETCRTC showed it returning -12 (ENOMEM) from bochs's pin of the new framebuffer.
* Reproduction: on Rach2's bochs-drm card, run `modetest -M bochs-drm -s CONNECTOR:#INDEX` for a mode whose frame does not fit in the video memory left free; it prints ENOSYS where SETCRTC failed with ENOMEM.
* Evidence: the commit that places the console's bochs buffer from the bottom of video memory names the cause; the debug run's console is not kept.
* Rach2: nothing in Rach2; the mode's failure is a result that `judge.py modes` compares with Debian's.
* Status: unreported.
* Next: read `set_mode` in libdrm's current source to confirm that DIRTYFB still comes between SETCRTC and the report, and propose saving `errno` before it or calling it only on success.

## Linux's memslot_perf_test ends a progressing guest on an alarm

* Project: Linux 6.12.115, `linux-6.12.115.tar.xz`, SHA-256 0ec90fe4b20422b1b664b77e6d2dde22b40bd41e7eb2cad8ac8739a4749cb1bb, `tools/testing/selftests/kvm/memslot_perf_test.c`.
* Fails: `host_perform_sync` arms `alarm(10)` before setting `sync_flag` and waiting for the guest to clear it (lines 418-426). A guest's RW pass that takes longer than ten seconds ends the process with SIGALRM even while the guest makes progress. The timeout follows emulation and nested exits, rather than a count of completed guest work.
* Found: task 84-33 measured the RW timeout under both Rach2 and Linux as the KVM host inside the testing PC's KVM. The WSL release run 43430545575 at 08c7795ac also ended the program with status 142 in its RW phase under Rach2 inside TCG; the Mac's TCG run at the same head passed it.
* Reproduction: run the program's default RW case two hypervisors deep, or under TCG on a loaded host, so that a guest's pass cannot finish within ten seconds. The runner prints `Testing RW performance`, then the shell reports `Alarm clock`.
* Evidence: [84-33.md](84-33.md) records its author's Linux comparison; [84-256.md](84-256.md) gives the WSL release's own output and evidence paths. The source is the pinned Linux tarball.
* Rach2: the release runs the KVM suite under KVM, as its nested cases require. Its `outer_kvm` row expects the failure already measured under Linux there. The program and the TCG row are unchanged.
* Status: unreported.
* Next: propose making the sync timeout optional for emulated and nested hosts, with the external runner's progress watchdog detecting a stopped guest.

## TinyCC's x86-64 code keeps the high half of a 64-bit value cast to unsigned int

* Project: TinyCC 0.9.27, `tcc-0.9.27.tar.bz2`, SHA-256 de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c, `tccgen.c`.
* Fails: on a 64-bit target `gen_cast` emits nothing for a cast of a 64-bit value to `unsigned int` (the truncation under `PTR_SIZE == 4`, lines 2635-2646, has no 64-bit counterpart) and nothing for the widening of an `unsigned int` to 64 bits (lines 2602-2623 extend only a signed source, with `movslq`), on the assumption that the register's high half is already zero, as after a 32-bit load. A value already in a register, such as the result of a conditional expression, keeps its high half through both: `(unsigned long)(unsigned int)x` gives `x`, and `x == (unsigned int)x` is always true. A value cast straight from memory is right, since the load is 32 bits wide.
* Found: 2026-11-14, the testing PC, milestone 84, task 84-260y, in `machine.selfbuild.kernel` on x86-64: ipcmusl's `sem-wide-count` row, a semop of 0x100000001 operations, ran one operation on the TinyCC-built kernel, where the GCC-built one returns E2BIG. The kernel's `USER_LEN32` is `(n) == (uint32_t)(n) ? (uint32_t)(n) : 0xFFFFFFFFu`, and x86-64's `SC_ARG` is a conditional expression.
* Reproduction: compile `int f(unsigned long a, unsigned long b, int c) { return (c ? a : b) == (unsigned)(c ? a : b); }` with the x86-64 TinyCC 0.9.27 and read the object: the two 64-bit values are compared with no `movl r, r` before the `cmp`, and `f(0x100000001, 0, 1)` returns 1. `rach2/sys/ipc/sysv.c`'s `sys_semop`, compiled with `-nostdinc -nostdlib -D__RACH2__ -Isys/acpi -mno-sse`, shows the same.
* Evidence: the release's failed case `tests.x86_64.20261114T070413Z.2894414.machine.selfbuild.kernel.r0`, in m2's clone on the testing PC, `evidence/probes.1.console`: `IPCMUSL FAIL (a count of operations past 32 bits is not E2BIG, or ran: errno 11)`.
* Rach2: `vendor/tcc/rach2-x86_64-zero-extend.patch` clears the high half with `movl r, r` at the widening of an `unsigned int` on x86-64.
* Status: unreported.
* Next: read TinyCC's mob branch to see whether its reworked `gen_cast` truncates or zero-extends here, and report it with the reproduction if not.

## TinyCC's x86-64 code converts an unsigned int to floating point from the whole register

* Project: TinyCC 0.9.27, `tcc-0.9.27.tar.bz2`, SHA-256 de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c, `x86_64-gen.c`.
* Fails: `gen_cvt_itof` (lines 2074-2113) converts an `unsigned int` to `float` or `double` with the REX.W form of `cvtsi2ss` or `cvtsi2sd` (line 2107), and to `long double` by pushing the whole register and loading it with `fildll` (lines 2085-2092), so both read the register's high half, which a 64-bit value cast to `unsigned int` in a register still holds, as the entry above says.
* Found: 2026-11-14, Codex's review of the patch of the entry above, milestone 84, task 84-261e, which read the code generator for every other path that reads such a register whole.
* Reproduction: compile `double f(unsigned long a, int c) { return (unsigned)(c ? a : 0); }` with the x86-64 TinyCC 0.9.27 and read the object: `cvtsi2sd %rax,%xmm0` follows the conditional expression with no `movl %eax,%eax`, and `f(0x100000001, 1)` returns 4294967297 in place of 1. A `float` result gives `cvtsi2ss %rax`, and a `long double` one `push %rax` and `fildll (%rsp)`.
* Evidence: `vendor/tcc/rach2-x86_64-zero-extend.patch`; the `widened` assertion of `machine.tcc.compile` in `tests/cases/machine.tcc.json`.
* Rach2: `vendor/tcc/rach2-x86_64-zero-extend.patch` clears the high half with `movl r, r` before each of the three conversions of an `unsigned int`.
* Status: unreported.
* Next: report it with the entry above, after reading the mob branch's `gen_cvt_itof`.

## TinyCC's x86-64 code adds a 32-bit index into a VLA to a 64-bit pointer

* Project: TinyCC 0.9.27, `tcc-0.9.27.tar.bz2`, SHA-256 de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c, `tccgen.c` and `x86_64-gen.c`.
* Fails: `post_type` stores the size of a VLA in an `int` slot (`tccgen.c` line 4371) and `vla_runtime_type_size` reads it back as an `int` (line 2729). `gen_op` scales the index of a pointer into an array of variable length by that size, where a fixed size is pushed as a `long long`, so the multiply is 32 bits wide; `gen_opic` then adds the product to the pointer with a width that `gen_opi` takes from the pointer alone (`x86_64-gen.c` line 1742), 64 bits, and a negative product, zero-extended by the 32-bit `imul`, moves the pointer almost 4 GiB forward. A size of 0x80000000 or more, which the `int` cannot hold, reads back negative, and a cast of it to 64 bits moves the pointer backward. GCC keeps the size in a `size_t`.
* Found: 2026-11-14, the owner's Mac, milestone 84, task 84-261e, reading the code generator for paths that read a 32-bit value from a whole register.
* Reproduction: compile `long g(int n, int i) { int a[2][n]; return (char *)(a + 1 + i) - (char *)a; }` with the x86-64 TinyCC 0.9.27 and read the object: `imul %ecx,%eax` is followed by `add %rax,%rcx`, and `g(3, -1)` returns 4294967296 in place of 0.
* Evidence: `vendor/tcc/rach2-x86_64-zero-extend.patch`; the `widened` assertion of `machine.tcc.compile` in `tests/cases/machine.tcc.json`.
* Rach2: `vendor/tcc/rach2-x86_64-zero-extend.patch` keeps the size in a `size_t` slot, computes the product of a length and an element's size as a `size_t` and pushes a fixed element size as one, so that the index is widened to the pointer's width before the multiply; the difference of two pointers to VLA rows divides by the size cast to `ptrdiff_t`.
* Status: unreported.
* Next: read the mob branch's pointer arithmetic for a VLA, and report it with the reproduction if it is unchanged.

## TinyCC scales an index into a pointer to a VLA by the size of a pointer

* Project: TinyCC 0.9.27, `tcc-0.9.27.tar.bz2`, SHA-256 de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c, `tccgen.c`.
* Fails: `gen_op` takes the runtime size of the pointed type only when the pointer operand's own type has `VT_VLA` (`tccgen.c` lines 2198 and 2224), which an array of variable length has and a declared pointer to one, `int (*p)[n]`, does not. `pointed_size` then gives the size of the pointed array type, which `type_size` takes for a pointer's, so `p + 1` moves `p` by 8 bytes on x86-64 and 4 on i386, and `p - q` divides by the same.
* Found: 2026-11-14, the owner's Mac, milestone 84, task 84-261j, reading the code a host-built x86-64 TinyCC made for the test of the VLA's size.
* Reproduction: compile `long g(int n) { int a[2][n]; int (*p)[n] = a; return (char *)(p + 1) - (char *)a; }` with TinyCC 0.9.27 for either target and read the object: the add is of the constant 8, or 4, and `g(3)` returns 8, or 4, in place of 12.
* Evidence: `vendor/tcc/rach2-x86_64-zero-extend.patch`; the `widened` assertion of `machine.tcc.compile` in `tests/cases/machine.tcc.json`.
* Rach2: `vendor/tcc/rach2-x86_64-zero-extend.patch` takes the runtime size when the pointed type has `VT_VLA` too.
* Status: unreported.
* Next: read the mob branch's `gen_op`, whose `vpush_type_size` of the pointed type may already cover it, and report it with the reproduction if not.

## TinyCC refuses an array parameter sized by an earlier parameter

* Project: TinyCC 0.9.27, `tcc-0.9.27.tar.bz2`, SHA-256 de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c, `tccgen.c`.
* Fails: `void f(int n, int a[n])`, which C99 allows (6.7.5.2, 6.7.5.3p7, 6.9.1p10), stops with `'n' undeclared`. `post_type` pushes each parameter as a field symbol (`tccgen.c` line 4296), which `sym_find` never returns, so a later parameter's bound cannot name it; at file scope a bound must be a constant expression (line 4344); and nothing evaluates a bound on entry to the function, so a parameter `int a[n][m]`, a pointer to a row of variable length, has no size for `a[i][j]` or `sizeof a[0]`. `convert_parameter_type` clears `VT_ARRAY` but not `VT_VLA` (line 4226), and `compare_types` takes an array of variable length and one of fixed size for different types (line 2815), against 6.7.5.2p6.
* Found: 2026-11-14, Codex's review of the VLA fixes of task 84-261j, milestone 84, which named the limit; task 84-261n confirmed it on the owner's Mac with an x86-64 and an i386 TinyCC built from the tarball and Rach2's patches.
* Reproduction: compile `void f(int n, int a[n]) { a[0] = n; }` with TinyCC 0.9.27 for either target: it fails with `'n' undeclared` on line 1. `void h(int n, int a[*]);` fails the same way.
* Evidence: `vendor/tcc/rach2-vla-param.patch`; the `vla-built` and `vla-param` assertions of `machine.tcc.compile` in `tests/cases/machine.tcc.json`.
* Rach2: `vendor/tcc/rach2-vla-param.patch` makes each named parameter visible to the bounds after it, keeps the tokens of a bound that is not constant, evaluates them on entry to the function into the size slot of each row a parameter points to, accepts `[*]` in a prototype, clears `VT_VLA` in the adjustment to a pointer, and makes an array of variable length compatible with one of fixed size.
* Status: fixed upstream on TinyCC's mob branch, in no release: ec5d9429 of 2022-03-01 makes the parameters visible to the bounds, 313855c2 of 2022-03-21 evaluates a saved bound on entry to the function, and 0244320b of 2022-03-24 with a5588501 of 2022-04-12 allows `int a[][m]`. The mob branch at f830a169, built for x86-64, compiles the program of `machine.tcc` and passes `int arr[2][3]` for `int a[n][m]` with no warning. The commits do not apply to 0.9.27, whose parser they postdate.
* Next: nothing to report; drop the patch when Rach2 moves to a TinyCC release that has these commits.

## TinyCC leaves an i386 static executable's PLT slots and GOT unfilled

* Project: TinyCC 0.9.27, i386 target, `tcc-0.9.27.tar.bz2`, SHA-256 de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c, `tccelf.c`, `i386-link.c` and `lib/Makefile`.
* Fails: an i386 static executable that converts a floating value to an integer faults. `lib/Makefile` (line 26) builds the runtime `libtcc1.a` with GCC and `-fPIC`, so `__fixdfdi`, `__fixsfdi` and `__fixxfdi` call `__fixunsdfdi`, `__fixunssfdi` and `__fixunsxfdi` by `R_386_PLT32`. `gotplt_entry_type` gives every `R_386_PLT32` a PLT slot (`i386-link.c` line 82), whose `jmp *n` keeps the GOT entry's offset n as an absolute address in a static link, and `fill_got` (`tccelf.c` line 1330) runs after `tidy_section_headers` (line 2215) has dropped the relocation sections it reads, so the GOT stays 0 as well. It is the fault of milestone 57's entry for x86-64, on the other target; Rach2's patch fixed only x86-64.
* Found: 2026-11-14, the owner's Mac, milestone 84, task 84-261n, running `machine.tcc.compile` on i686 alone: the program of the `widened` assertion faulted, `vm: fault: killing pid 32: bad read at 0x14 (err 0x5)`.
* Reproduction: on i686 Rach2, compile `int main(int c, char **v) { double x = c + 0.5; return (long)x; }` with `tcc -static` and run it: it faults reading 0x14, and with `long double` 0x18. The disassembly of the executable shows `__fixdfdi`'s call to a PLT slot `jmp *0x14` and a GOT of zeros.
* Evidence: `vendor/tcc/rach2-x86_64-static-link.patch`; the `widened` assertion of `machine.tcc.compile` in `tests/cases/machine.tcc.json`.
* Rach2: `vendor/tcc/rach2-x86_64-static-link.patch` now also makes an `R_386_PLT32` call to a function the executable defines a plain `R_386_PC32` call, and fills the entries of `R_386_GOT32` and `R_386_GOT32X` before the relocation sections are dropped.
* Status: fixed upstream on TinyCC's mob branch, in no release: 9a7edb20 of 2025-12-30 ("Add pic/pie support to i386") makes an `R_386_PLT32` or `R_386_PC32` call to a function an executable defines PC-relative.
* Next: nothing to report; drop the change when Rach2 moves to a TinyCC release that has the commit.

## virgl on Mesa's D3D12 driver shows a geometry or tessellation shader's image stores out of order across memoryBarrier

* Project: virglrenderer 1.2.0, Ubuntu 26.04's 1.2.0-2ubuntu2, in QEMU 10.2.1 (1:10.2.1+ds-1ubuntu3.3), drawing on Mesa 26.0.8's D3D12 gallium driver (26.0.8-1ubuntu0.3) under WSL2 on an NVIDIA GeForce RTX 4080 SUPER; the guests' Mesa is Debian's 25.0.7-2+deb13u1 and their piglit Debian's 0.0~git20250409.af62c0dea-2. Which of virglrenderer, Mesa's driver, Direct3D 12 and NVIDIA's driver is at fault was not traced.
* Fails: in piglit's `arb_shader_image_load_store-shader-mem-barrier`, the `'volatile' qualifier` subtests of the geometry and the tessellation evaluation stages at modulus 64 fail in some runs and pass in others. Each producer invocation stores `i` at `x + K` and then, after `memoryBarrier()`, at `x`; its consumer, K invocations to the right, loads `x`, calls `memoryBarrier()`, loads `x + K`, and fails when the second load is the smaller (`tests/spec/arb_shader_image_load_store/shader-mem-barrier.c`, lines 146 to 172 at af62c0dea). A failing pixel holds `v << 16 | u`: release run 343804 read 5898240, 90 at `x` and 0 at `x + K`, so the consumer saw the later store and not the earlier. The `'coherent'` subtests of every stage and the `'volatile'` ones of the other four stages passed in each of the 500 runs below.
* Found: 2026-11-14, the testing PC's WSL, milestone 84, task 84-261f, in the release's full run 343804: `machine.virgl.piglit-14` on x86-64 failed the geometry stage's subtest, which Debian's reference guest had passed in its one run of 2026-11-11.
* Reproduction: on that host, in a Linux or Rach2 guest on the virgl profile, run `arb_shader_image_load_store-shader-mem-barrier --quick -auto -fbo` on surfaceless EGL 100 times. Counts of runs with a failure, of 100 each: Rach2 under TCG with run 343804's kernel and root, 9 (geometry 5, tessellation evaluation 5); Rach2 under KVM, 66 (23, 58); Debian under KVM, 40 (14, 31); then both guests at once under KVM, Rach2 13 (9, 5) and Debian 39 (27, 15). The rate follows the host's load, which the release's own guests changed through the afternoon, and not the guest: Rach2's went from 66 to 13 between two runs. Debian's kernel stalled in its i8042 probe under TCG on the loaded host and ran no test there.
* Evidence: `84-261f/`, each run's lines from `loop.sh`, with the Debian guest's record, and the scripts that made them: `rep.py` for Rach2's guest and `deb.sh` for Debian's.
* Rach2: nothing in its own code; the shader's loads and stores run within one draw on the host's GPU. `tests/graphics/piglit-virgl.flakes` and `piglit-quick-gl.flakes` leave the two subtests, and the test's own row, which fails with them, unjudged.
* Status: unreported.
* Next: run the test on the host itself, without a guest, with `GALLIUM_DRIVER=d3d12`, to see whether virglrenderer is needed; then read how virglrenderer writes TGSI's `volatile` image declarations and `MEMBAR` into GLSL (`vrend_shader.c`) and how Mesa's `nir_to_dxil` gives the image's accesses Direct3D's `globallycoherent` and its barrier in the geometry and domain shaders.

## Venus leaves a ring asleep with commands in it when the guest's clock read is slow

* Project: Mesa 26.1.6's Venus driver, Debian's binary package in the GNU root, `src/virtio/vulkan/vn_ring.c`, with virglrenderer 1.2.0's ring thread on the host, `src/venus/vkr_ring.c`; Rach2 keeps neither source.
* Fails: a guest submission that sees the ring's IDLE bit sends no notify when it read its clock less than 1 ms after the last notify's reading, and the host's ring thread can go idle in that millisecond, so the ring sleeps with its tail ahead of its head, and the driver, which never sends a notify while it waits, waits for good. `vn_ring_submit_internal` stores the tail, loads the status, and only then reads `os_time_get_nano()` and sends vkNotifyRingMESA unless the time lies inside [`last_notify`, `last_notify` + 1 ms) (`vn_ring.c:464-491`, `VN_RING_IDLE_TIMEOUT_NS` at line 18). `vkr_ring_thread` goes idle 1 ms after the last command it ran, not after the last notify; a notify that arrives while it runs is taken and moves nothing (`vkr_ring.c:262-285`, `vkr_ring_notify` at 368-371). If a submission A finds IDLE, the thread meanwhile finds A's tail by itself and runs it, and A's clock read returns a time δ after its status load, A's notify reaches a running thread, which goes idle 1 ms after running A; a submission B in the next δ finds IDLE and is throttled by A's late time. The race needs δ longer than the host's time to run A.
* Found: 2026-11-14, the testing PC under QEMU 10.2.1's TCG with virtio-vga-gl's Venus, milestone 84, task 84-260n, when the release's `machine.venus.piglit-quick-gl` case on x86_64 overran its gap bound with the host logging `vkr: failed to look up object 59 of type 8`. Rach2's `clock_gettime` there was a system call reading the HPET, whose every read waits on QEMU's global lock, which virtio-gpu holds for some 0.8 ms per command: under 8 test processes its median was 0.46 to 0.66 ms, its 99th percentile 9 to 16 ms.
* Reproduction: under TCG, run 8 of piglit's textureGather tests at once on zink over Venus in a guest whose clock read takes most of a millisecond; within 300 to 520 s of guest time a test stops in its wait, its ring's status 0x5, IDLE and ALIVE, the tail ahead of the head, the ring thread asleep in `futex_do_wait`. A Debian guest, whose Linux reads the TSC in its vDSO under TCG, finishes the same list in some 270 s.
* Evidence: `dev-log/84-locks/84-260n.md`, with the stuck rings read from the host's view of guest memory and QEMU's trace of each ring's last notifies.
* Rach2: x86-64's vDSO reads a TSC that nothing vouches for, watched against the HPET or the PM timer as Linux's clocksource watchdog watches one, so that the clock read takes microseconds under TCG, as on Linux. That makes δ short; it does not close the race.
* Status: unreported.
* Next: read Mesa's and virglrenderer's main to see whether either moved; propose that `vkr_ring_thread` restart its idle timer when it takes a notify, which bounds B's time below by A's, or that the driver notify again from its wait.
