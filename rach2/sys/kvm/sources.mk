# sys/kvm/sources.mk: Linux's KVM interface, for sys/arch/x86_64/sources.mk
# alone, so that the i686 kernel builds without it. One file to a line.

KVM_ASM_SRC = sys/kvm/svm_run.s \
    sys/kvm/vmx_run.s

KVM_C_SRC = sys/kvm/kvm.c \
    sys/kvm/vcpu.c \
    sys/kvm/vfio.c \
    sys/kvm/cpuid.c \
    sys/kvm/msr.c \
    sys/kvm/pmu.c \
    sys/kvm/svm.c \
    sys/kvm/vmx.c \
    sys/kvm/vlapic.c \
    sys/kvm/irq.c \
    sys/kvm/i8259.c \
    sys/kvm/ioapic.c \
    sys/kvm/i8254.c \
    sys/kvm/iobus.c \
    sys/kvm/slots.c \
    sys/kvm/gpamap.c \
    sys/kvm/tdp.c \
    sys/kvm/dirty.c \
    sys/kvm/decode.c \
    sys/kvm/emulate.c \
    sys/kvm/hyperv.c \
    sys/kvm/pvclock.c \
    sys/kvm/binstats.c
