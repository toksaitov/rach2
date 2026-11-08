# Notes for 78-kvm.md

Drafter's working notes. The output is `78-kvm.md` beside this file.

## Progress

* Read: brief.md, plan.md (whole), dev-log/70-wifi-chip/plan.md (the form).
* Next: round3 research on KVM (codex.md, fable.md, position-chat.md, rebuttals, comparison.md, second/round3-argon.md), then the kernel code.

## Facts gathered

(filled as read; each with its path:line)

### From the research (round3, comparison, Argon)
* QEMU 10.0.13 required caps (position-chat.md:158, fable.md:61): USER_MEMORY, DESTROY_MEMORY_REGION_WORKS, JOIN_MEMORY_REGIONS_WORKS, INTERNAL_ERROR_DATA, IOEVENTFD, IOEVENTFD_ANY_LENGTH, IRQFD (with in-kernel irqchip); x86: SET_TSS_ADDR, EXT_CPUID, MP_STATE, SIGNAL_MSI, IRQ_ROUTING, DEBUGREGS, XSAVE, VCPU_EVENTS, X86_ROBUST_SINGLESTEP, MCE, ADJUST_CLOCK, SET_IDENTITY_MAP_ADDR. Optional: READONLY_MEM, IMMEDIATE_EXIT, USER_MEMORY2. Split irqchip (KVM_CAP_SPLIT_IRQCHIP 121).
* Argon item 7: QEMU's kvm_arch_init calls KVM_GET_MSR_INDEX_LIST (fatal), KVM_GET_MSR_FEATURE_INDEX_LIST, KVM_SET_IDENTITY_MAP_ADDR; per vCPU GET/SET_MSRS, XSAVE, XCRS, MP_STATE, VCPU_EVENTS, LAPIC; KVM_SET_GSI_ROUTING.
* Argon item 5: VFIO BAR mmap registered as memory slots; vma_t.pgoff is uint32 (vm.h:61), mmap rejects pgoff overflow (syscall_proc.c:6379-6386); KVA_MMIO_SIZE 128 MiB (x86_64/cpu.h:145); slots over VM_PROT_IO must be EPT UC with guest PAT in charge.
* Argon item 2: EPT memory type: leave IPAT clear on guest RAM once a non-coherent device is attached (dmar0 ecap 0xf0101a has no snoop control).
* Codex rebuttal :113: memslots must follow COW, protection changes, madvise, truncation, not only unmap; mm contracts at mm/Readme.md:173,175,183,189.
* Codex rebuttal :115: XSETBV validates XCR0 against advertised features; WBINVD honoured once a non-coherent device is assigned.
* Fable :64: decoder about 1,000 lines: mov (sizes, sign/zero extend), movs, stos, arithmetic/logic group with memory operand, test, cmp, xchg, push/pop memory; else INTERNAL_ERROR with the bytes.
* Fable :66: big lock released in guest, taken on each exit (arch/x86/Readme.md:75); x86/vmexit.flat measures.
* Fable :67: lapic.c:166-167 refuses x2APIC on the host; guest x2APIC emulated in-kernel.
* Fable :101: harness CPU model from processor table tools/etude/qemu_profile.c:1179, -accel at :1248.
* Fable :107: sizes: ioctl layer 2,500; VMX 2,500; SVM 1,500; shared NPT builder + LAPIC 1,200; decoder 1,000; tests 800. Total ~9,500. Plan says 9,500 to 13,000.
* Linux uAPI on hand: build/research/src/linux/include/uapi/linux/kvm.h (API 12).
* The Lenovo: i3-4030U family 6 model 0x45; XSAVE features 0x7 (x87, SSE, AVX), context 832 bytes (dmesg.txt:218-222); x2apic enabled on Linux; IOAPIC GSI 0-39.
* F6: testing PC has no nested virt; SVM under TCG there and on the Mac. Mac's QEMU 11.1.3 TCG max offers svm and npt.
* Debian QEMU 10.0.13 (trixie); qemu-system-x86 through the fetcher (not in GNU root: vendor/gnu/Readme.md:3).
