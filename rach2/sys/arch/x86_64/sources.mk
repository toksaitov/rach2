# sys/arch/x86_64/sources.mk: the x86-64's files, for rach2/sources.mk. The
# entry with the multiboot header comes first; the PC platform's files are
# sys/arch/x86/sources.mk's.

include sys/arch/x86/sources.mk

# Where TinyCC's link would put the kernel's text on Rach2: KVA_IMAGE plus
# 1 MiB plus the ELF64 header and two program headers, as i686's puts its
# own, so that the multiboot header lies where GNU ld's link puts it.
ARCH_TCC_LDFLAGS = -Wl,-Ttext=0xFFFFFFFF801000B0

ARCH_ASM_SRC = sys/arch/x86_64/loader.s \
    sys/arch/x86_64/isr_handler.s \
    sys/arch/x86_64/switch.s

ARCH_C_SRC = sys/arch/x86_64/mmu.c \
    sys/arch/x86_64/context.c \
    sys/arch/x86_64/usertest.c \
    sys/arch/x86_64/percpu.c \
    sys/arch/x86_64/idt.c \
    sys/arch/x86_64/sigframe.c \
    sys/arch/x86_64/tls.c \
    sys/arch/x86_64/ptrace.c \
    $(PC_C_SRC)
