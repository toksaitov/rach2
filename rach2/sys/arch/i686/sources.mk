# sys/arch/i686/sources.mk: the i686's files, for rach2/sources.mk. The
# entry with the multiboot header comes first; the PC platform's files are
# sys/arch/x86/sources.mk's.

include sys/arch/x86/sources.mk

# Where TinyCC's link puts the kernel's text on Rach2: 1 MiB plus the ELF
# and program headers, so that the multiboot header lies where GNU ld's
# link puts it.
ARCH_TCC_LDFLAGS = -Wl,-Ttext=0x100074

ARCH_ASM_SRC = sys/arch/i686/loader.s \
    sys/arch/i686/isr_handler.s \
    sys/arch/i686/gdt_handler.s \
    sys/arch/i686/idt_handler.s \
    sys/arch/i686/switch.s

ARCH_C_SRC = sys/arch/i686/context.c \
    sys/arch/i686/usertest.c \
    sys/arch/i686/gdt.c \
    sys/arch/i686/tss.c \
    sys/arch/i686/idt.c \
    sys/arch/i686/sigframe.c \
    sys/arch/i686/tls.c \
    $(PC_C_SRC)
