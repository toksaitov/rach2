# `kvm/`: the KVM interface

The `kvm/` directory is the kernel's side of Linux's KVM API, version 12, on the x86-64 kernel alone: `/dev/kvm`, the VM and vCPU descriptors its ioctls make, each vCPU's run page, the state ioctls QEMU calls, the run loop, and the interface a processor's backend, AMD's SVM or Intel's VMX, gives the rest. A program such as QEMU uses it as it uses Linux's, through `linux/kvm.h`. The memory slots, the guest-physical tables, the in-kernel interrupt controllers, the bus of ioeventfds and the two backends are stubs here that refuse or answer nothing, so KVM_RUN fails with `EOPNOTSUPP` and QEMU stops at the capabilities it requires. [kvm(4)](../../../man/man4/kvm.4) is the user's contract.

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
| `irq.c`, `vlapic.c` | stubs of the interrupt routing and the local APIC in the kernel |
| `iobus.c` | a stub of the ioeventfds' bus |
| `decode.c` | a stub of the instruction decoder for MMIO |
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

### Lock order

1. The big kernel lock, which every ioctl runs under.
2. The vCPU, held for one ioctl (`vcpu_lock`), as Linux's `vcpu->mutex`. A second ioctl of the same vCPU sleeps until the first ends, letting go of the big lock meanwhile; a signal does not end that wait, since the holder is a KVM_RUN its own signal ends or a state ioctl that does not sleep long.
3. Spinlocks of other subsystems, such as an eventfd's, which an irqfd's hook runs under ([io](../io/Readme.md)).

The run loop lets go of the big lock only inside `enter`, with interrupts off and the vCPU held, so nothing else runs the vCPU meanwhile, and another thread changes it only by `kvm_vcpu_kick`'s requests. Nothing sleeps with interrupts off, and no code takes the big lock while it holds a spinlock.

### State

The registers, special registers, XSAVE image, XCR0, events, debug registers, MP state, CPUID, MSRs, TSC offset and machine-check banks live in the vCPU's record; `dirty` names the parts a set changed, for the backend to load before the next entry, and `save` brings the backend's copy back before a get. The checks follow Linux's: KVM_SET_SREGS refuses EFER, CR0 and CR4 that the processor would and bits the guest's CPUID does not offer; KVM_SET_XSAVE refuses components past the guest's XCR0, a reserved MXCSR bit and a compacted image; KVM_SET_CPUID2 after the first KVM_RUN accepts only the same table; KVM_SET_MSRS stops at the first MSR it refuses and returns the count before it. A new vCPU has the power-on state Linux gives one, at the reset vector with the boot processor's APIC base.

## Limits

* At most 64 vCPUs in a VM, with ids below 256; 256 memory slots, which `slots.c` will hold; 256 entries for KVM_SET_CPUID2 and KVM_GET_CPUID2, and fewer than 256 MSRs at once for KVM_GET_MSRS and KVM_SET_MSRS, as Linux's bounds.
* Only the machine type 0. The in-kernel interrupt controller will be the split irqchip alone, the local APIC in the kernel and the PIC and IOAPIC in the caller; there is no in-kernel PIT, no coalesced MMIO, no dirty ring, and no SMM or nested virtualization.
* KVM_GET_SUPPORTED_CPUID offers no paravirtual leaf, no performance counters, no PKU and no AMX.
* Until the backends land, KVM_RUN fails with `EOPNOTSUPP`, and QEMU 10, which requires the slots' and interrupts' capabilities, stops before it.

## Tests

* `tests/host/kvm_hosttest.c` includes `kvm.c` and `vcpu.c` and links `cpuid.c` and `msr.c` over a model of the Lenovo's Haswell and a fake backend: the structures' sizes and the ioctl and capability numbers against Linux's; QEMU 10's start-up in the order of its sources, to KVM_RUN and the backend's refusal; each exit kind through the run page, with port and MMIO answers, immediate_exit, a signal, a kick and a triple fault; the big lock let go and interrupts off for the entry; the state ioctls' refusals; the shared and private mappings; `EIO` from another address space; and every allocation given back.
* `tests/host/eventfd_hosttest.c` covers the eventfd interface that irqfd and ioeventfd will use.
* `tests/musl/kvmmusl.c`, a native case on the Lenovo: the run page mapped shared and private, read by KVM_RUN under `immediate_exit`, and shared with a fork's child, whose KVM_RUN is `EIO`. It runs under QEMU's TCG with `-cpu max`, which offers SVM, too.
