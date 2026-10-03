# sys/arch/x86_64/sources.mk: the x86-64's files, for rach2/sources.mk. The
# entry with the multiboot header comes first; the PC platform's files are
# sys/arch/x86/sources.mk's.

include sys/arch/x86/sources.mk

# What TinyCC on Rach2 compiles and links the kernel with: no SSE register,
# as GCC's -mno-sse has it, since a variadic function's prologue would save
# them; and link.ld, the layout GNU ld's link has, with segments congruent
# modulo 4 KiB, as its -z max-page-size=0x1000 has them.
ARCH_TCC_CFLAGS = -mno-sse
ARCH_TCC_LDFLAGS = -Wl,-T,sys/arch/x86_64/link.ld -Wl,-section-alignment=0x1000

ARCH_ASM_SRC = sys/arch/x86_64/loader.s \
    sys/arch/x86_64/isr_handler.s \
    sys/arch/x86_64/switch.s \
    sys/arch/x86_64/trampoline.s

ARCH_C_SRC = sys/arch/x86_64/mmu.c \
    sys/arch/x86_64/context.c \
    sys/arch/x86_64/usertest.c \
    sys/arch/x86_64/percpu.c \
    sys/arch/x86_64/idt.c \
    sys/arch/x86_64/sigframe.c \
    sys/arch/x86_64/tls.c \
    sys/arch/x86_64/ptrace.c \
    sys/arch/x86_64/systab.c \
    $(PC_C_SRC)
