# 81 kvm: plan

Milestone 81 of the graphics stack, whose branch is `milestone/76-display`. The stack is milestones 76 to 83, planned together in [the approved plan](../76-graphics/plan.md) and [its task list](../76-graphics/tasks.md); this file gathers what concerns milestone 81. The research behind it is in [dev-log/76-graphics/](../76-graphics/research/Readme.md).

## The goal

The goal, as a researcher sees it: Debian's QEMU on Rach2 boots a Debian guest and Rach2's own images with `-accel kvm`, on the Lenovo (VMX), and on the testing PC (SVM, under nested KVM and under TCG) and the Mac (SVM under TCG); kvm-unit-tests and the KVM selftests pass or skip with a stated reason; the cost of a VM exit is measured.

| Task | What it builds | Waits on |
|---|---|---|
| 81-1 KVM interface | /dev/kvm, VM and vCPU files, memory slots over process memory that follow every change to it (copy-on-write, protection changes, madvise, truncation, unmap), with vCPUs stopped and translations flushed before a frame is reused; the run page, the state ioctls QEMU calls (MSRs and their index lists, XSAVE and XCRs, MP state, vCPU events, LAPIC state, GSI routing, the TSS and identity-map addresses), the capabilities QEMU requires; slots over device mappings, such as an assigned GPU's BARs, mapped uncached in the nested page tables with the guest's PAT in charge, never pinned as RAM; slots over a GPU object's mappings with huge pages, which stop Linux's KVM ("kvm run failed Bad address": QEMU's `pf io` mappings of `anon_inode:i915.gem` with `shmem_enabled` at `always`, where `hva_to_pfn_remapped` returns `-EFAULT` when `kvm_try_get_pfn` fails; that the failing page is a huge page's tail is read in the source, not observed), mapped or refused with an error the VM survives, the reproducer kept and the failing page traced (its flags and count) on the Lenovo under Debian | 76-2 |
| 81-2 SVM | The VMCB, nested paging, intercepts, the interrupt shadow; run under TCG and, once 81-1 boots a guest, under WSL's nested KVM on the testing PC (81-6's profile) | 81-1 |
| 81-3 VMX | VMXON, the VMCS, EPT, unrestricted guest, VPID, the preemption timer; EPT memory types as Linux sets them: write-back with the guest's PAT ignored until a device that does not snoop is assigned, then the guest's PAT honoured (the Lenovo's graphics IOMMU has no snoop control) | 81-1 |
| 81-4 Interrupts | The in-kernel local APIC with its timer, the split irqchip's routing, KVM_SIGNAL_MSI, irqfd, ioeventfd | 81-1 |
| 81-5 MMIO decoder | The instruction decoder for device memory, tested against a table of encodings | nothing |
| 81-6 QEMU and the corpus | Rach2's PCI devices found by one walk at boot rather than one per driver (138 of the 180 s Rach2 took to its prompt at the third level under KVM; at the second level a patch that does it, `run2/step5-7/pci-once.diff`, cut KVM's boot from 2.140 s to 0.914 to 0.954 s and its exits from 352,415 to 353,503 a boot to 148,401 to 149,035), then the SB16 probe's 40,400 reads of port 0x80 a boot and a walk that follows bridges, the walk tried on the Lenovo by trial boot; Debian's qemu-system-x86 through the fetcher; kvm-unit-tests and the KVM selftests built and staged; a TCG profile with svm and npt, qualified first by a small entry and nested-page-fault payload, and a nested-KVM profile on the testing PC with the SVM features WSL offers (npt, nrip_save, tsc_scale, vmcb_clean, flushbyasid, pause filter; no decode assists, vgif or virtual VMSAVE), each chosen per workload; a runner that boots guests to a serial token | the PCI walk, Linux fixtures and the outer profiles at once; Rach2's use of them after 81-1 |
| 81-7 Measurement | The exit cost and the selftests' timing cases on the Lenovo beside Linux's there, exits counted by kind (the testing PC's numbers are the cost of its whole nested stack, recorded apart from the Lenovo's and compared only between runs with the same outer layers, such as Rach2's and Linux's there, where exits are counted, not timed; at the second level step 7's counted boots give about 6 us per exit to QEMU under KVM and 8 under WHPX), with boot time, exit microbenchmarks and workload throughput apart; Linux's numbers on the Lenovo taken early by 80-2 on the Debian on p5 (under Alpine its kernel reached its first process in 1.11 s under KVM, 8.84 s under TCG) | 81-2 to 81-6; 80-2's Linux numbers (evidence) |

Done: QEMU boots Debian and Rach2's i686 image under `-accel kvm` on the three machines (the Mac under TCG, 81-2.5; the Lenovo by trial boot); the corpus passes, each other result as Linux's or an exclusion you approve; each exit keeps Linux's semantics (XSETBV checked against the advertised features, WBINVD honoured once a device is assigned); a slot over a GPU object's huge-page mapping works or fails with an error the VM survives; a killed QEMU leaves no leaked pins, files or mappings; the i686 kernel builds without the hypervisor.

## The owner's decisions

The owner approved the graphics plan, draft 6i, with `tasks.md`, for all eight milestones of the stack on 2026-11-09 at 22:07 UTC+6, with each of the nine decisions as the plan recommends:

* A: the udmabuf module in WSL's own kernel, loaded by root after each start of WSL; the accounts in the `render` group.
* B: Firefox decodes video on the GPU and Chromium on the CPU.
* C: WebGPU on the Lenovo for tests only.
* D: Chromium's GPU sandbox as Linux has it, so 76-20 is not filed (140 tasks, 665 subtasks).
* E: the performance gate, no unexplained loss over 10% against Linux on the same Lenovo with the IOMMU's translation off.
* F: the Lenovo's p3 shrunk to 64 GiB, the data partition made from Debian on p5, rehearsed in QEMU first (76-15c).
* G: every download the plan's Costs section lists is approved, from the official sources; no card is filed and no task waits on one.
* H: the second run's Lenovo facts and Linux references committed (`tests/fixtures/lenovo-gpu/`, `tests/graphics/ref/lenovo/`) with the run's reports.
* I: the 32-bit profile leaves out the disk-image suites.

Standing decisions: one shared branch, `milestone/76-display`; Steam stays out of the build.

## Tasks

Each task below is an issue in the GitHub milestone `81 kvm`, and each subtask a sub-issue under it; a wait is a blocker on GitHub. Faults found in other projects' code go in `upstream.md` beside this file, in the format [dev-log/upstream.md](../upstream.md) gives.

The goal (plan.md, 81): Debian's QEMU on Rach2 boots a Debian guest and Rach2's own images with `-accel kvm` on the Lenovo (VMX), the testing PC (SVM under nested KVM and under TCG) and the Mac (SVM under TCG); kvm-unit-tests and the KVM selftests pass or skip with a stated reason; the cost of a VM exit is measured.

The plan's seven tasks become twelve. 81-1 is split so that the files, the vCPU state and the backend interface start early and only the memory slots wait on 76-2b; 81-3 is split into its code (81-3a) and its Lenovo step (81-3b); 81-6 is split by host, since it mixes a kernel change, the GNU layer and the test tools; 81-6d carries the Lenovo step of 81-6a and 82-1.

All of 81's kernel code goes in `rach2/sys/kvm/` (new), listed by `rach2/sys/kvm/sources.mk` (new), which `rach2/sys/arch/x86_64/sources.mk` includes, so that the i686 kernel builds without it.

### 81-1a The KVM interface

`/dev/kvm`, the VM and vCPU descriptors, the capabilities QEMU requires, the run page, the state ioctls, the run loop and the backend interface, and the kernel's own use of eventfds.

* Files: `rach2/sys/kvm/Readme.md`, `sources.mk`, `kvm.h` (the backend and slot interface), `kvm_abi.h` (Linux's API 12 structures in C89), `kvm.c`, `vcpu.c`, `cpuid.c`, `msr.c` (all new), with stubs for `svm.c`, `vmx.c`, `vlapic.c`, `irq.c`, `iobus.c`, `slots.c`, `tdp.c`, `dirty.c`, `decode.c` (a decoder that refuses every instruction), `svm_run.s` and `vmx_run.s` (entry points that return an error) that the later tasks replace, so that no later task edits `sources.mk`; `rach2/sys/arch/x86_64/sources.mk` (one include line); a row in 76-1's device registry (`kvm` 10:232), not `devfs.c`; `rach2/sys/io/eventfd.c`, `eventfd.h`; `man/man4/kvm.4` (new); `man/man7/limits.7`; `rach2/hardware.md`; `tests/host/kvm_hosttest.c` (new), `tests/host/eventfd_hosttest.c`.
* Waits on: 76-1 (code: the device registry); 76-2a (code: the mapping dispatch through `file_ops.mmap`, which the run page needs).
* Labels: none.
* Subtasks:
  * 81-1a.1 `kvm.h`: the backend operations (create, run, state, event injection, the guest-physical entry format), the slot lookup that 81-1b, 82-2 and 83-5a call, and the lock order, written in the Readme; check: every later file of 81 compiles against it.
  * 81-1a.2 `/dev/kvm`, `KVM_GET_API_VERSION` 12, `KVM_CHECK_EXTENSION` answering QEMU's required capabilities (USER_MEMORY, DESTROY_MEMORY_REGION_WORKS, JOIN_MEMORY_REGIONS_WORKS, INTERNAL_ERROR_DATA, IOEVENTFD, IOEVENTFD_ANY_LENGTH, IRQFD, and for x86 SET_TSS_ADDR, EXT_CPUID, MP_STATE, SIGNAL_MSI, IRQ_ROUTING, DEBUGREGS, XSAVE, VCPU_EVENTS, X86_ROBUST_SINGLESTEP, MCE, ADJUST_CLOCK, SET_IDENTITY_MAP_ADDR) only as each later task makes it true, `KVM_CREATE_VM`, `KVM_CREATE_VCPU`, `KVM_GET_VCPU_MMAP_SIZE`; check: the host test against a backend that refuses to run.
  * 81-1a.3 The run page mapped into QEMU through the vCPU descriptor's `file_ops.mmap`, which 76-2a's dispatch calls (today `mmap_file_check`, `syscall_proc.c:6102-6112`, refuses a descriptor that is not a file), giving a `vm_file` whose `share` returns the page's frame (`rach2/sys/mm/vm.h:65-71`), so that no edit of `mm/` is needed; check: a musl program maps it, and its fields survive a fork as Linux's do.
  * 81-1a.4 The run loop: signals and `immediate_exit`, the big lock let go in guest mode and taken back on each exit (`bkl_drop`, `bkl_take`, `rach2/sys/proc/smp.h:53-60`), a vCPU stopped from another processor by `smp_kick` (`smp.h:78`), exits to QEMU for port I/O, MMIO, HLT, shutdown and internal errors; check: the host test's fake backend drives each exit kind.
  * 81-1a.5 The state ioctls: registers, special registers, FPU and XSAVE with XCRs, MSRs with `KVM_GET_MSR_INDEX_LIST` and the feature list, `KVM_SET_CPUID2` and `KVM_GET_SUPPORTED_CPUID` from the host's CPUID with no paravirtual leaf at first, MP state, vCPU events, debug registers, TSS and identity-map addresses, `KVM_GET_CLOCK` and `KVM_SET_CLOCK`; check: QEMU's start-up sequence, taken from `strace` under Linux, replays through the host test.
  * 81-1a.6 The kernel's eventfd interface: take a reference from a descriptor, signal from the kernel, and a callback when a program signals it, for 81-4's irqfd and ioeventfd, 82-2's VFIO interrupts and 83-3b's doorbells; check: `eventfd_hosttest.c` rows for each, with a writer and a callback racing a close.
  * 81-1a.7 `kvm(4)`, `limits(7)` (vCPUs per VM, slots per VM) and `hardware.md` (VMX or SVM required); check: `make mancheck` and the i686 kernel built with no file of `kvm/`.
* Done: on Rach2 in QEMU, Debian's QEMU with `-accel kvm` opens `/dev/kvm`, finds every capability it requires, makes a VM and its vCPUs and reaches `KVM_RUN`, where the stub backend refuses with an error QEMU reports.

### 81-1b Memory slots and the guest-physical tables

Memory slots over process memory that follow every change to it, the guest-physical page tables that EPT and NPT share, dirty logging, slots over device mappings and over GPU objects with huge pages.

* Files: `rach2/sys/kvm/slots.c`, `tdp.c`, `dirty.c` (new, or the stubs of 81-1a), `rach2/sys/kvm/Readme.md` (its section), `man/man4/kvm.4` (its section), `tests/musl/kvmslotmusl.c` (new).
* Waits on: 81-1a (code); 76-2 (code: pinning, device mappings, huge pages and the notice of a region's change, which this task uses without editing `mm/`, Problems item 4).
* Labels: none.
* Subtasks:
  * 81-1b.1 `KVM_SET_USER_MEMORY_REGION`: slots made, moved, flagged read-only and deleted while vCPUs run; check: the musl test against Linux's rules for overlap and alignment.
  * 81-1b.2 The guest-physical tables built on fault from the slot's pages, with the entry format the backend gives, large pages where the host page and the slot allow; check: host test rows for 4 KiB, 2 MiB and mixed slots.
  * 81-1b.3 Slots follow copy-on-write, protection changes, `madvise`, truncation and unmap: vCPUs stopped and translations flushed before a frame is reused; check: the musl test forks, protects, drops and unmaps under a running guest and reads what the guest sees.
  * 81-1b.4 Dirty logging: `KVM_MEM_LOG_DIRTY_PAGES`, `KVM_GET_DIRTY_LOG` and `KVM_CLEAR_DIRTY_LOG`, which QEMU's VGA turns on at once (Problems, item 3); check: the selftest `dirty_log_test` in 81-2's run, and a host test row.
  * 81-1b.5 Slots over device mappings (`/dev/mem`'s `VM_PROT_IO` and later VFIO's BARs) mapped uncached with the guest's PAT in charge, never pinned as RAM; check: a slot over a QEMU BAR mapped from `/dev/mem`, its writes reaching the device.
  * 81-1b.6 Slots over a GPU object's huge-page mapping, mapped or refused with an error the VM survives; check: 76-2's huge-page fixture as a slot, the guest reading it or QEMU getting the error and going on.
* Done: a slot follows every change the plan lists (plan.md, Problems, item 4), dirty logging works, and a killed QEMU leaves no pinned page counted.

### 81-2 SVM

AMD's backend: the VMCB, nested paging, intercepts, the interrupt shadow, run under TCG and under WSL's nested KVM.

* Files: `rach2/sys/kvm/svm.c` (replacing 81-1a's stub), `rach2/sys/kvm/svm_run.s` (new, listed by 81-1a's manifest), the Readme's SVM section.
* Waits on: 81-1a, 81-1b, 81-4, 81-5 (code: the interface, memory, interrupts to inject and the MMIO decoder); 81-6b, 81-6c (code: QEMU and the corpus in the root, the TCG and nested-KVM profiles to run them).
* Labels: `linux-host`.
* Subtasks:
  * 81-2.1 Enable SVM per processor, the host save area, VMRUN with the guest's and host's state saved, host interrupts taken after the exit (no edit of `isr.c`); check: kvm-unit-tests' `sieve` under the TCG profile.
  * 81-2.2 Nested page tables in 81-1b's format, ASIDs and flushes by ASID where offered; check: `access` and `emulator`.
  * 81-2.3 Intercepts: I/O, MSR permission maps, CPUID, HLT, INVLPG, shutdown, XSETBV checked against the advertised features, WBINVD as Linux handles it; check: `msr`, `xsave`, the selftests `state_test` and `sync_regs_test`.
  * 81-2.4 Event injection and the interrupt shadow, with 81-4's LAPIC; check: `eventinj`, `apic-split`, `smptest`.
  * 81-2.5 The Debian guest and Rach2's i686 image to their tokens under the TCG profile on WSL and on the Mac, then under nested KVM on the testing PC, each a case of `machine.kvm.json` whose `requirements.hosts` names its hosts; check: 81-6c's runner reports both tokens on each.
  * 81-2.6 From the runs of 81-2.1 to 81-2.5 and 81-2.7, each timed as it ran, the minutes of 81's checkpoint slice estimated and recorded in `tests/kvm/` and the plan's Checkpoints; no extra run.
  * 81-2.7 `kvm:slots` and `kvm:leak` as cases of `machine.kvm.json`: 81-1b's slots over a `/dev/mem` device mapping and over a huge-page object with the guest going on in both, slots following fork, `mprotect` and `munmap` under a running guest, and QEMU killed under load with every pinned page, descriptor and mapping counted back; check: its author runs each once under the TCG profile.
* Done: under TCG on WSL and the Mac and under nested KVM, Debian and Rach2's i686 image boot under Rach2's SVM, the corpus's SVM rows pass or skip with a reason, and the slice's minutes are recorded.

### 81-3a VMX

Intel's backend: VMXON, the VMCS, EPT, unrestricted guest, VPID, the preemption timer, and EPT memory types as Linux sets them, proven by a host test that runs the trial's whole path on a VMX model. Split from its Lenovo step (81-3b), since 81-7 and 83-2b need the code.

* Files: `rach2/sys/kvm/vmx.c` (replacing the stub), `rach2/sys/kvm/vmx_run.s` (replacing 81-1a's stub), the Readme's VMX section, `tests/host/kvm_vmx_hosttest.c` (new) with a model of the Lenovo's VMX.
* Waits on: 81-1a, 81-1b, 81-4, 81-5 (code: the interface, memory, interrupts and the decoder).
* Labels: none.
* Subtasks:
  * 81-3a.1 VMXON after reading `IA32_FEATURE_CONTROL` (locked with VMX on, 0x5, on the Lenovo), the VMCS from the capability MSRs, VMXOFF on every exit path, every VMX instruction through one table of operations that the model stands in for; check: a review on the issue of each VMX instruction's failure path, line by line against Linux's `vmx.c`.
  * 81-3a.2 EPT with the entry format of 81-1b, VPID, unrestricted guest, the preemption timer bounding every guest run; EPT memory types write-back with the guest's PAT ignored until a non-snooping device is assigned, then the guest's PAT honoured, as Linux's `vmx_get_mt_mask`; check: the code beside Linux's, and a host test of the type chosen for each case.
  * 81-3a.3 Exits as 81-2's, with NMIs passed to the host and host interrupts taken after the exit; check: 81-3a.4's host test drives each exit's handler, since TCG has no VMX.
  * 81-3a.4 `kvm_vmx_hosttest.c`: a model of the Lenovo's capability MSRs and of VMXON, VMLAUNCH, VMRESUME, VMREAD, VMWRITE and their failures, through which the host test runs the trial's whole path, start to finish: VMXON, the VMCS built, a guest entered, each exit kind 81-6c's corpus rows cause handled, the preemption timer's exit, VMXOFF on every exit path and on panic, as the `machine` skill's readiness note asks; check: the host test passes.
* Done: the VMX host test runs the trial's whole path on the model, and each VMX instruction's failure path has its review on the issue.

### 81-3b VMX on the Lenovo

Rach2's VMX on the Lenovo by trial boot, silent and headless.

* Files: `dev-log/81-kvm/81-3.md` (new).
* Waits on: 81-3a (code); 81-6b, 81-6c (code: QEMU, the corpus and the runner); 76-15b (code: the trial root's slot); 76-15c (`evidence`: the data partition that carries QEMU on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 81-3b.1 The readiness note for the trial boot: why no new path can stop the machine (every wait bounded, the preemption timer armed, VMXOFF on panic), 81-3a.4's host test as the run of the same path, and the manager's go; hand back `owner-only`; check: the note and the go on the issue.
  * 81-3b.2 Lenovo step, silent and headless, a trial boot of its own, `mixer vol 1` after the boot: the trial kernel and root; the corpus rows of 81-6c; the Debian guest and Rach2's i686 image to their tokens with `-accel kvm`; check: the runner's tokens and the corpus's results in `81-3.md`.
* Done: on the Lenovo, by trial boot, Debian and Rach2's i686 image boot under Rach2's VMX and the corpus passes or skips with a reason.

### 81-4 Interrupts

The in-kernel local APIC with its timer, the split irqchip's routing, `KVM_SIGNAL_MSI`, irqfd and ioeventfd.

* Files: `rach2/sys/kvm/vlapic.c`, `irq.c`, `iobus.c` (replacing the stubs), the Readme's interrupt section, `man/man4/kvm.4` (its section), `tests/host/kvm_irq_hosttest.c` (new).
* Waits on: 81-1a (code).
* Labels: none.
* Subtasks:
  * 81-4.1 The local APIC: registers through MMIO and x2APIC MSRs, the timer in one-shot, periodic and TSC-deadline modes, `KVM_GET_LAPIC` and `KVM_SET_LAPIC`; check: a host test against the SDM's register rules.
  * 81-4.2 `KVM_ENABLE_CAP(KVM_CAP_SPLIT_IRQCHIP)`, `KVM_SET_GSI_ROUTING` with MSI routes, the EOI exit to QEMU for level-triggered lines; `KVM_CREATE_IRQCHIP` refused, so every QEMU command line names `kernel-irqchip=split` (Problems, item 5); check: host test rows.
  * 81-4.3 `KVM_SIGNAL_MSI` and `KVM_IRQFD` with resample, through 81-1a's eventfd callback; check: an interrupt raised by a write to the eventfd reaches the vLAPIC in the host test.
  * 81-4.4 `KVM_IOEVENTFD` on port and MMIO addresses with and without a data match; check: host test rows.
  * 81-4.5 The page and the Readme's section; check: `make mancheck`.
* Done: the host tests of the APIC, routing, irqfd and ioeventfd pass; 81-2 and 81-3b prove them with guests.

### 81-5 MMIO decoder

The instruction decoder for guest accesses to device memory, tested against a table of encodings.

* Files: `rach2/sys/kvm/decode.c` (replacing 81-1a's stub), `decode.h` (new), `tests/host/kvm_decode_hosttest.c` (new) with its table, `tests/fuzz/x86dec_fuzz.c` (new).
* Waits on: 81-1a (code: the stub and the manifest).
* Labels: none.
* Subtasks:
  * 81-5.1 Decode MOV of each size with sign and zero extension, MOVS, STOS, the arithmetic and logic group with a memory operand, TEST, CMP, XCHG, PUSH and POP of memory, in 16, 32 and 64-bit modes with prefixes; check: the table of encodings, each with its expected operand and length.
  * 81-5.2 Anything else is an internal error carrying the instruction's bytes, as Linux's `KVM_INTERNAL_ERROR_EMULATION`; check: table rows.
  * 81-5.3 The decoder's fuzzer; check: a fixed corpus replays clean under the sanitizers.
* Done: every row of the table decodes as written, and the fuzzer finds nothing.

### 81-6a Rach2's PCI devices found by one walk

One walk at boot that follows bridges, in place of one walk per driver over every bus; the SB16 probe's 40,400 reads of port 0x80 a boot cut; tried on the Lenovo by 81-6d.

* Files: `rach2/sys/pci.c`, `pci.h`, `rach2/sys/arch/x86/platform.c`, `rach2/sys/mm/pagecache.c` (`pc_probe_window_size` alone, as `run2/step5-7/pci-once.diff:26-85` does), `rach2/sys/audio/sb16.c`, `tests/host/` (a new `pciwalk_hosttest.c`), `rach2/hardware.md`.
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 81-6a.1 The walk from `run2/step5-7/pci-once.diff`, from bus 0 through each bridge's secondary bus, kept as one list that `pci_nth` and the finders read; check: a host test over a configuration-space model with bridges and an empty slot.
  * 81-6a.2 The SB16 probe's waits on port 0x80 replaced or skipped when no card answers; check: a count of port reads at boot under QEMU, before and after.
  * 81-6a.3 Under KVM on the testing PC at the second level, boot time and exits counted against step 7's 0.914 to 0.954 s and 148,401 to 149,035 exits; check: the counts recorded.
* Done: one walk finds every device on QEMU's machines, and a boot under KVM costs the exits step 7 measured with the patch; 81-6d proves the walk on the Lenovo.

### 81-6b QEMU and the corpus staged

Debian's QEMU through the fetcher, and kvm-unit-tests and the KVM selftests built from pinned sources and staged.

* Files: `vendor/gnu/kvm/` (new: Readme, `qemu.list` in 76-13's fetcher's format, the build of kvm-unit-tests and the selftests, their pins), `vendor/gnu/install_root.sh` (one call line), `man/programs.md` (made again).
* Waits on: 76-13a (code: the fetcher); the downloads card (`decision`: Debian's QEMU, kvm-unit-tests and the KVM selftests' sources).
* Labels: `linux-host`.
* Subtasks:
  * 81-6b.1 `qemu-system-x86`, `qemu-system-modules-opengl` (virtio-gpu-gl and the GL displays, which 83-2 needs), `qemu-utils`, SeaBIOS and OVMF with their closures; check: the fetcher's `check` step resolves QEMU's libraries inside the root.
  * 81-6b.2 kvm-unit-tests at the revision 80-2 pins, built for x86-64, with its `unittests.cfg`; check: its `x86/sieve.flat` boots under TCG on the build host.
  * 81-6b.3 The KVM selftests from the Linux source at the same revision 80-2 pins, built static with the GNU toolchain; check: each binary runs `--help` or its skip on a host without `/dev/kvm`.
  * 81-6b.4 The downloads and their sizes in the Readme, as the downloads card lists them; check: the Readme's table.
* Done: the root has Debian's QEMU with its GL modules, kvm-unit-tests and the selftests, each pinned.

### 81-6c Profiles and the guest runner

The outer profiles (TCG with `svm` and `npt`, qualified first; nested KVM on the testing PC with the SVM features WSL offers) and a runner on Rach2 that boots guests to a serial token.

* Files: `tools/etude/processor.c` (its only editor in the stack), `tests/kvm/` (new: the runner, the corpus's case lists, the guest images' recipes), `tests/cases/machine.kvm.json` (new), `tests/registry` (one row), `tests/system/suites.mk` (its block), `tests/Readme.md` (its line).
* Waits on: 76-16a (code: the KVM profile with `-cpu host`, 76-16a.2); 81-6b (code: QEMU and the corpus in the root).
* Labels: `linux-host`.
* Subtasks:
  * 81-6c.1 A processor profile with `svm` and `npt` under TCG, qualified by kvm-unit-tests' own `svm` tests run by the outer QEMU directly; check: those tests pass under TCG on WSL and on the Mac.
  * 81-6c.2 The nested-KVM profile on the testing PC with npt, nrip_save, tsc_scale, vmcb_clean, flushbyasid and the pause filter; check: the profile's guest reports those flags.
  * 81-6c.3 The runner: QEMU on Rach2 with `-accel kvm`, `kernel-irqchip=split` and a serial console, a guest booted to its token or failed on silence; check: a Debian guest under TCG inside Rach2 reaches its token.
  * 81-6c.4 The corpus's lists: the kvm-unit-tests and selftests that run, and each skip with its reason (nested virtualization, PMU, Hyper-V); check: the lists are in `tests/kvm/` and the suite `kvm` names them.
  * 81-6c.5 The family's line in `tests/registry`, its block in `suites.mk` and the Readme entry, each case with its `requirements.hosts`; check: `tests/run` lists the `kvm` cases.
* Done: the two outer profiles exist, the TCG one qualified, and the runner boots a guest to its token on Rach2.

### 81-6d The PCI walk and the VT-d units on the Lenovo

One silent, read-only trial kernel for 81-6a's walk and 82-1's reading of the DMAR table, so that neither code task, nor 82-2 after them, waits on the Lenovo.

* Files: `dev-log/81-kvm/81-6d.md` (new, the record).
* Waits on: 81-6a, 82-1 (code).
* Labels: `lenovo`.
* Subtasks:
  * 81-6d.1 Prepare the step: the trial kernel from the branch's head with both tasks on it and its SHA-256, the commands, what to read and what passes; the review note (the walk only reads configuration space; the DMAR code reads the table and both units' capability registers with translation off and writes no unit register) and the manager's go; hand back `owner-only`; check: the comment on the issue.
  * 81-6d.2 Lenovo step, silent, read-only, `mixer vol 1` after the boot: the trial kernel lists the same PCI functions as the boot before, and parses the DMAR table and reads both units' capability registers; check: `/proc/bus/pci/devices` or `/sys/bus/pci/devices` equal before and after, and the values equal Linux's log (`dev-log/70-wifi-chip/linux/dmesg.txt:190-200`: HAW 39, two units, two RMRRs, dmar0 ecap f0101a, dmar1 ecap f010da).
* Done: one walk finds every device on the Lenovo, and its VT-d units are read as Linux reads them.

### 81-7 Measurement

The exit cost and the selftests' timing cases on the Lenovo beside Linux's there, exits counted by kind, and the `/dev/kvm` fuzzer.

* Files: `tests/kvm/` (after 81-6c: the measurement scripts), `dev-log/81-kvm/81-7.md` (new).
* Waits on: 81-2, 81-3a, 81-4, 81-6a, 81-6c (code); 80-2, 81-3b, 81-6d (`evidence`: Linux's numbers, VMX and the walk proven on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 81-7.1 Exits counted by kind in the kernel and read through the debug files or `/proc`; check: a boot's counts by kind in QEMU.
  * 81-7.2 76-22's ioctl fuzzer run as an ordinary user against `/dev/kvm`, a VM and a vCPU; check: a fixed seed count with no fault or leak.
  * 81-7.3 On the testing PC, Rach2's and Linux's exit counts for the same guests under the same outer layers, never timed across layers; check: the counts in `81-7.md`.
  * 81-7.4 Lenovo step, silent, a trial boot of its own, `mixer vol 1` after the boot: `vmexit_*` and the selftests' timing cases, boot time, exit microbenchmarks and a workload's throughput, each apart; reusing 81-3b's trial kernel when nothing changed; check: each beside 80-2's Linux numbers in `81-7.md`.
  * 81-7.5 The big lock's cost on the hottest exits read from the counts, as risk 9 asks; check: the report says which exits a lock-free path would serve, with the numbers.
* Done: the exit costs on the Lenovo beside Linux's, counted by kind, and the fuzzer clean.
