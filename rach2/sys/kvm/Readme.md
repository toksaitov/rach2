# `kvm/`: the KVM interface

The `kvm/` directory is the kernel's side of Linux's KVM API, version 12, on the x86-64 kernel alone: `/dev/kvm`, the VM and vCPU descriptors its ioctls make, each vCPU's run page, the state ioctls QEMU calls, the run loop, and the interface a processor's backend, AMD's SVM or Intel's VMX, gives the rest. A program such as QEMU uses it as it uses Linux's, through `linux/kvm.h`. The local APIC is in the kernel, with the split controller's routes, MSIs, irqfds and ioeventfds, and the PIC and IOAPIC stay in the caller. The memory slots, the guest-physical tables and the two backends are stubs here that refuse or answer nothing, so KVM_RUN fails with `EOPNOTSUPP` and QEMU stops at the capabilities it requires. [kvm(4)](../../../man/man4/kvm.4) is the user's contract.

## Interfaces

* `/dev/kvm`, character device 10:232, mode 0666, registered by `kvm_start` (`kernel.c`) when a backend's `probe` succeeds, VMX tried first; the boot logs `kvm: ready: vmx` or `kvm: ready: svm`. Without either there is no node, as Linux loads no KVM module.
* The ioctls of `/dev/kvm`, of a VM and of a vCPU, numbered and laid out as Linux's (`kvm_abi.h`). An ioctl a level does not have is `ENOTTY` on `/dev/kvm` and a VM, and `EINVAL` on a vCPU, as on Linux. The VM and vCPU descriptors are `anon_inode:kvm-vm` and `anon_inode:kvm-vcpu:N`, read-write and close-on-exec.
* A vCPU's mapping: KVM_GET_VCPU_MMAP_SIZE is two pages, the `struct kvm_run` page and the page port I/O data goes through (`KVM_PIO_PAGE_OFFSET` 1). A shared mapping maps both pages cached, and a fork's child shares it; a private one copies each page at its first touch, as a private mapping of a file does here ([memory](../mm/Readme.md)). Either holds a reference to the VM, so the pages outlive the descriptors.
* A VM belongs to the address space that made it: an ioctl of the VM or of its vCPUs from another, a fork's child's, is `EIO`, as Linux's `kvm->mm` check makes it.
* An ioctl of KVM's type, 0xAE, that ends with `EINTR` is never restarted, whether or not a handler runs, since Linux gives KVM_RUN's `EINTR` to the caller as it is (`ioctl_restartable` in `syscall.c`, [signals](../proc/signal.Readme.md)).
* `kvm.h` is the interface of the directory's files to each other: the backend's operations, the VM and vCPU records, and each module's calls, among them `kvm_slot_find`, which the guest-physical tables, VFIO and the transport's buffers call to turn a guest frame into the caller's address.

## Sources and license

Rach2's own. The structures and numbers of `kvm_abi.h` follow Linux's `include/uapi/linux/kvm.h` and `arch/x86/include/uapi/asm/kvm.h`, whose layout is the ABI and is written out here; the capability masks of `cpuid.c` follow Linux's `kvm_set_cpu_caps`.

| File | What it holds |
| --- | --- |
| `kvm_abi.h` | the API's structures, ioctl numbers, capabilities and exit reasons |
| `kvm.h` | the records, the backend's operations, each module's calls |
| `kvm.c` | `/dev/kvm`, the capability table, the VM's descriptor and its ioctls, the kvmclock |
| `vcpu.c` | the vCPU's descriptor, its reset state, the run pages and their mapping, the run loop, the state ioctls and their checks |
| `cpuid.c` | KVM_GET_SUPPORTED_CPUID from the host's CPUID, the guest's XCR0 |
| `msr.c` | the MSR index lists, the feature MSRs, the MSRs a vCPU keeps and their checks |
| `svm.c`, `vmx.c`, `svm_run.s`, `vmx_run.s` | the backends: their probes and guest-physical entry formats; entry is a stub that fails |
| `slots.c`, `tdp.c`, `dirty.c` | stubs of the memory slots, the guest-physical tables and the dirty log |
| `vlapic.c` | the local APIC: its registers through MMIO and the x2APIC MSRs, its timer, delivery, INIT and SIPI, the halted vCPU's wait |
| `irq.c` | the split controller, the MSI routes, KVM_IRQ_LINE, KVM_SIGNAL_MSI, irqfds and the EOI exit |
| `iobus.c` | the ioeventfds' buses of ports and MMIO addresses |
| `decode.h`, `decode.c` | the decoder of the guest instructions that reach an MMIO exit, and the internal error for one it does not know |
| `sources.mk` | the directory's files, which the x86-64 kernel's list includes |

## Design

### The records

A VM (`struct kvm_vm`) counts its references in `refs`: its descriptor, each vCPU's descriptor and each mapping of a run page. A vCPU lives as long as its VM, as on Linux, so closing a vCPU's descriptor drops its VM reference and nothing else; the last reference frees each vCPU, the modules' shares, the backend's and the VM, and lets go of the address space it held with `mm_hold`. A vCPU's run pages are two pages of the kernel's own, made at KVM_CREATE_VCPU. The descriptors' objects begin with a `struct anon_obj`, as every anonymous descriptor's does ([io](../io/Readme.md)).

### Capabilities

Each module answers only the capabilities it makes true, so a capability turns on with the task that makes it so. `kvm.c` answers its own, then asks `slots.c`, `irq.c`, `iobus.c` and the backend in turn; the stubs answer 0. KVM_CHECK_EXTENSION on a VM gives the same answers as on `/dev/kvm`, since no capability depends on the VM's type.

### The run loop

KVM_RUN (`vcpu.c`) holds the vCPU for the call, swaps in the signal mask KVM_SET_SIGNAL_MASK gave, and gives the backend the caller's answer to the last exit's read, from the port I/O page or the run page's MMIO data, before anything else, so the answer is not lost when `immediate_exit` or a signal ends the call. With `immediate_exit` set it then returns `EINTR` at once. Otherwise, before each entry, it answers a triple fault with KVM_EXIT_SHUTDOWN and a signal or a request to leave with KVM_EXIT_INTR and `EINTR`, injects a waiting event, and enters: with interrupts off it marks the vCPU in the guest, checks the requests once more, lets go of the big lock and calls the backend's `enter`; it takes the lock back after the exit and turns interrupts on, which lets the host's interrupt that ended the guest run. The backend's `exit` then names the exit. A port or MMIO write the bus takes and an exit the kernel handles enter again, and so does a HLT once an interrupt ends it, when the local APIC is in the kernel (`kvm_lapic_halt`); the rest go to the caller through the run page, a port or MMIO read waiting for its answer. `kvm_vcpu_kick` leaves a request and, when the vCPU is in the guest on another processor, interrupts that processor with `smp_kick`, so the guest exits and the loop sees the request.

### The backend

`struct kvm_backend` (`kvm.h`) is the contract: `probe` at boot; `vm_init`, `vm_free`, `vcpu_init` and `vcpu_free`; `enter`, which runs with interrupts off and without the big lock, takes no lock and does not sleep; `exit`, under the big lock, which fills `v->exit` and the interrupt flags the run page reports; `complete`, `save` and `inject`; `check_extension`; and `tdp`, the format of its guest-physical entries, EPT's or the AMD64 format, so that one walker in `tdp.c` builds both.

### The MMIO decoder

An MMIO exit tells the backend the guest-physical address and, on VMX, whether the access read or wrote, but not the operand: that is in the instruction, which `kvm_decode` (`decode.h`) reads from the guest's bytes at its RIP, in the mode of its code segment. It gives the instruction's length, what it does with memory (`KVM_OP_*`: a load, a store, an arithmetic and logic operation, a unary one, XCHG, MOVS, STOS, PUSH or POP), the memory operand's size, the register or immediate, the widening of MOVZX, MOVSX and MOVSXD, the segment and address as base, index, scale and displacement, the address size, and the REP and LOCK prefixes. The backend reads or writes the register, completes the access through the run page, and steps RIP by the length. Prefixes are read as the processor reads them: the last segment override and the last of 0xF2 and 0xF3 count, REX counts only right before the opcode, REX.W outweighs 0x66, and the 16-bit ModRM forms take SS for an address through BP.

An instruction outside that set, one whose ModRM names a register, a LOCK the processor would refuse with #UD, and one that would pass 15 bytes are refused. The backend then does what Linux's `handle_emulation_failure` does: it injects #UD into a guest above CPL 0, and at CPL 0 reports KVM_EXIT_INTERNAL_ERROR through `kvm_decode_fail`, which lays out the run page as Linux's `prepare_emulation_failure_exit`: suberror KVM_INTERNAL_ERROR_EMULATION, the flag that the instruction's bytes follow, the bytes padded with 0x90, then the exit's reason and qualifications.

### Interrupts

The split controller is Linux's: KVM_ENABLE_CAP of KVM_CAP_SPLIT_IRQCHIP on the VM, before its first vCPU, with the caller's IOAPIC's pin count in `args[0]`, at most 4096, puts each vCPU's local APIC in the kernel and leaves the PIC and IOAPIC to the caller. KVM_CREATE_IRQCHIP, the whole controller in the kernel, is `ENXIO`, or `EEXIST` after the split one. QEMU 10's pc and q35 machines ask for the whole controller by default and stop when it fails, so they run with `-machine kernel-irqchip=split`, or with `off`, which keeps every controller, the local APIC too, in QEMU and injects through KVM_INTERRUPT.

`vlapic.c` keeps each APIC's 1 KiB register page and follows Linux's `lapic.c`: the readable registers, the LVT masks, the ID, LDR and DFR rules, SPIV's software disable masking every LVT entry, and the base MSR's moves between disabled, xAPIC and x2APIC modes, which the guest's CPUID allows. Version 0x14 has six LVT entries and directed EOI when the guest's CPUID offers x2APIC. The page is reached through the backend's MMIO exits at the APIC base; in x2APIC mode, or with the APIC disabled, a read of it gives all ones and a write is dropped, as Linux's LAPIC_MMIO_HOLE quirk does. The x2APIC registers are MSRs 0x800 to 0x8FF, whose reserved bits and read-only registers raise #GP. Delivery to a fixed, physical, flat or cluster destination, a broadcast and a shorthand is Linux's `kvm_apic_match_dest`, and lowest priority goes to one vCPU by Linux's vector hashing. The boot vCPU's LINT0 takes ExtINT at reset, as Linux's LINT0_REENABLED quirk sets it, so KVM_INTERRUPT's vector, the caller's PIC's, reaches the guest before it programs its APIC; a second KVM_INTERRUPT while one waits is `EEXIST`. KVM_GET_LAPIC and KVM_SET_LAPIC give the page in Linux's layout, the x2APIC id in bits 31:24 as without KVM_CAP_X2APIC_API, and the timer's TMCCT, from which KVM_SET_LAPIC restarts the count.

The timer counts at Linux's APIC bus of 1 GHz in host nanoseconds, by TDCR's divisor, one-shot, periodic with Linux's floor of 200 us, or against IA32_TSC_DEADLINE in the guest's TSC when the guest's CPUID offers that mode. `kvm_lapic_prepare` checks it before each entry, and the host's 1 ms tick makes the guest exit, so an expiry reaches the guest within a tick. Before each entry it also takes an INIT, which resets the vCPU's registers and the APIC and leaves an application processor waiting for SIPI, a SIPI, which starts that processor at the vector's page in real mode, and the NMIs other vCPUs sent. A HLT stays in the kernel: the halted vCPU, or one waiting for SIPI, sleeps until an interrupt it can take, its timer's tick or a signal, which ends KVM_RUN with KVM_EXIT_INTR and leaves the vCPU halted. KVM_GET_MP_STATE and KVM_SET_MP_STATE give and take that state; SIPI_RECEIVED is taken as a wait for SIPI with one pending.

`irq.c` holds the routes KVM_SET_GSI_ROUTING gives, MSI routes alone, one for each GSI below 4096, since the IOAPIC's are the caller's. KVM_IRQ_LINE and KVM_IRQ_LINE_STATUS raise a GSI's MSI; KVM_SIGNAL_MSI sends one directly and returns the count of APICs that took it, or `EPERM`, Linux's -1, when none did. An MSI's assert bit is ignored, as Linux's `kvm_set_msi_irq` ignores it. The EOI of a vector an APIC took level-triggered through a route of a GSI below the pin count asks its vCPU to exit with KVM_EXIT_IOAPIC_EOI and the vector, so the caller's IOAPIC can lower its line or raise it again.

An irqfd (KVM_IRQFD) is a hook on an eventfd ([io](../io/Readme.md)) that raises its GSI's MSI at each rise of the counter and takes the counter, as Linux's irqfd does. The hook runs under the eventfd's spinlock, possibly in another process's write, so it delivers from a copy of the route that KVM_SET_GSI_ROUTING refreshes under `irq.c`'s spinlock. A counter above 0 at KVM_IRQFD raises the GSI at once. An eventfd may back one irqfd of a VM (`EBUSY`); KVM_IRQFD_FLAG_RESAMPLE is `EINVAL` with the split controller, as Linux's `kvm_arch_irqfd_allowed` makes it, and QEMU then resamples in its own IOAPIC. The irqfd ends at its deassign, the VM's end, or the eventfd's last close, whose hangup frees it; when the VM's end finds the hook already taken off by a close whose hangup has yet to run, it leaves the record to that hangup.

`iobus.c` keeps the ioeventfds (KVM_IOEVENTFD) on Linux's buses, ports, MMIO addresses and s390's notifications, at most 1000 on each: a guest's write at an ioeventfd's address, of its length, or of any length for length 0, and of its value when it has a data match, signals its eventfd and enters the guest again without an exit. A string OUT signals once for each element. Two ioeventfds that some write would both match are `EEXIST`, as in Linux's `ioeventfd_check_collision`. A write that the bus does not take goes to the caller, and a read is never the bus's.

### Lock order

1. The big kernel lock, which every ioctl runs under.
2. The vCPU, held for one ioctl (`vcpu_lock`), as Linux's `vcpu->mutex`. A second ioctl of the same vCPU sleeps until the first ends, letting go of the big lock meanwhile; a signal does not end that wait, since the holder is a KVM_RUN its own signal ends or a state ioctl that does not sleep long.
3. Spinlocks of other subsystems, such as an eventfd's, which an irqfd's hook runs under ([io](../io/Readme.md)).
4. `irq.c`'s spinlock, over the routes and the irqfds' copies of them, which the hook takes under the eventfd's.

The run loop lets go of the big lock only inside `enter`, with interrupts off and the vCPU held, so nothing else runs the vCPU meanwhile, and another thread changes it only by `kvm_vcpu_kick`'s requests. Nothing sleeps with interrupts off, and no code takes the big lock while it holds a spinlock.

### State

The registers, special registers, XSAVE image, XCR0, events, debug registers, MP state, CPUID, MSRs, TSC offset and machine-check banks live in the vCPU's record; `dirty` names the parts a set changed, for the backend to load before the next entry, and `save` brings the backend's copy back before a get. The checks follow Linux's: KVM_SET_SREGS refuses EFER, CR0 and CR4 that the processor would and bits the guest's CPUID does not offer; KVM_SET_XSAVE refuses components past the guest's XCR0, a reserved MXCSR bit and a compacted image; KVM_SET_CPUID2 after the first KVM_RUN accepts only the same table; KVM_SET_MSRS stops at the first MSR it refuses and returns the count before it. A new vCPU has the power-on state Linux gives one, at the reset vector with the boot processor's APIC base.

## Limits

* At most 64 vCPUs in a VM, with ids below 256; 256 memory slots, which `slots.c` will hold; 256 entries for KVM_SET_CPUID2 and KVM_GET_CPUID2, and fewer than 256 MSRs at once for KVM_GET_MSRS and KVM_SET_MSRS, as Linux's bounds.
* Only the machine type 0. The in-kernel interrupt controller is the split one alone: no in-kernel PIC, IOAPIC or PIT, no routes but MSI routes, and no irqfd resample. There is no KVM_CAP_X2APIC_API, no coalesced MMIO, no dirty ring, and no SMM, SMI or nested virtualization.
* The MMIO decoder knows the instructions `decode.h` lists, which compilers emit for device registers; Linux's emulator knows nearly all of x86, so an MMIO access through another instruction, such as BT, CMPXCHG, a string compare or an SSE move, is an internal error here and emulated there.
* KVM_GET_SUPPORTED_CPUID offers no paravirtual leaf, no performance counters, no PKU and no AMX.
* Until the backends land, KVM_RUN fails with `EOPNOTSUPP`, and QEMU 10, which requires the slots' capabilities, stops before it.

## Tests

* `tests/host/kvm_hosttest.c` includes `kvm.c` and `vcpu.c` and links `cpuid.c` and `msr.c` over a model of the Lenovo's Haswell and a fake backend: the structures' sizes and the ioctl and capability numbers against Linux's; QEMU 10's start-up in the order of its sources, to KVM_RUN and the backend's refusal; each exit kind through the run page, with port and MMIO answers, immediate_exit, a signal, a kick and a triple fault; the big lock let go and interrupts off for the entry; the state ioctls' refusals; the shared and private mappings; `EIO` from another address space; and every allocation given back.
* `tests/host/kvm_decode_hosttest.c` decodes a table of encodings in the three modes, each row with its length and decoded access, and refused rows, and checks `kvm_decode_fail`'s layout; `tests/fuzz/x86dec_fuzz.c` puts random bytes through the decoder in each mode and checks each decode against `decode.h`'s rules.
* `tests/host/kvm_irq_hosttest.c` includes `vlapic.c`, `irq.c`, `iobus.c` and the eventfd's `eventfd.c` over four vCPUs and one model of the host's clocks: the APIC's registers against the SDM and Linux, priorities, acknowledgement and EOI, each destination and delivery mode, INIT and SIPI, the timer's three modes, x2APIC mode and its #GP cases, KVM_GET_LAPIC and KVM_SET_LAPIC, ExtINT, the halted vCPU's wakes, the split controller's and the routes' refusals, the EOI exit, KVM_SIGNAL_MSI, irqfds raised by a program's write to the eventfd and freed each way they end, ioeventfds with and without a data match, and every allocation given back.
* `tests/host/eventfd_hosttest.c` covers the eventfd interface that irqfd and ioeventfd use.
* `tests/musl/kvmmusl.c`, a native case on the Lenovo: the run page mapped shared and private, read by KVM_RUN under `immediate_exit`, and shared with a fork's child, whose KVM_RUN is `EIO`. It runs under QEMU's TCG with `-cpu max`, which offers SVM, too.
