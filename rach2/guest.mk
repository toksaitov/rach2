# Build the Rach2 kernel on Rach2 itself.
#
# This is the POSIX makefile pdpmake reads in the guest, driving the staged
# tcc through its /bin/cc name, spelt out because a system with GCC, the GNU
# root among them, has another cc first on its path; the GNU Makefile beside
# it stays the host build and pdpmake cannot read it. Both include
# sources.mk, so one file lists the kernel sources for the two builds and
# they cannot drift apart.
#
# The link reads the object list from kernel.objs, a response file, which keeps
# the link line short however many objects there are. The response file is
# written by one builtin printf (no execve) into a temporary file renamed only
# after every line landed, so a printf that failed part way cannot leave a
# shorter list that a retry would treat as current. OBJS keeps the
# architecture's loader.o first: the multiboot header is in it and the
# linker keeps input order, so the header stays inside the first 8192 bytes of
# the kernel where the loaders search for it. The architecture's list gives
# the flags of the compile and of the link (ARCH_TCC_CFLAGS, ARCH_TCC_LDFLAGS):
# the i686 kernel's text address, or the x86-64 kernel's linker script.
# /lib/tcc/libtcc1.a supplies the helper routines tcc calls where gcc emits the
# code inline, the 64-bit arithmetic on i686 and va_arg on x86-64; -nostdlib
# keeps the C library and its startup files out of a kernel that must not have
# them.
#
# Run from a writable copy of this tree: make -f guest.mk

KCC = /bin/cc
KCFLAGS = -Wall -Wunsupported -nostdlib $(ARCH_TCC_CFLAGS)

# The machine's own architecture, whose directory under sys/arch/ sources.mk
# takes the processor's files from.
ARCH != uname -m

include sources.mk

OBJS = $(ASM_SRC:.s=.o) $(C_SRC:.c=.o)

# The link lands in kernel.new and is renamed only on success, so a failed
# link cannot leave a newer partial kernel that the next make treats as
# current. The structural check runs on the host, after extraction.
kernel: $(OBJS) kernel.objs
	$(KCC) -static -nostdlib -o kernel.new @kernel.objs $(ARCH_TCC_LDFLAGS) /lib/tcc/libtcc1.a
	mv kernel.new kernel

kernel.objs: guest.mk sources.mk sys/arch/$(ARCH)/sources.mk sys/arch/x86/sources.mk
	printf '%s\n' $(OBJS) > kernel.objs.new
	mv kernel.objs.new kernel.objs

.SUFFIXES: .o .c .s

.c.o:
	$(KCC) $(KCFLAGS) -c -o $@ $<

.s.o:
	$(KCC) -c -o $@ $<

clean:
	rm -f $(OBJS) kernel.objs kernel.objs.new kernel.new kernel
