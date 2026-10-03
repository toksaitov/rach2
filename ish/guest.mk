# Build the ish shell on Rach2 itself.
#
# The POSIX makefile pdpmake reads in the guest, driving the staged tcc
# through /bin/cc, spelt out because a system with GCC, the GNU root among
# them, has another cc first on its path; the GNU Makefile beside it stays
# the host build. ish
# has its own crt0 and raw syscall stubs, so it links -nostdlib the way
# the host build does. The host flags that are dropped are the word size and
# -Ttext: the staged tcc compiles for the machine it runs on, i386 or x86-64,
# and refuses the other's -m flag, and its default text base on each is the
# one the host build names explicitly (mk/arch.mk). So the makefile is the
# same for both architectures. Both makefiles include sources.mk, so one
# file lists the sources for the two builds; tools/verify/guestcheck.c
# checks the derivation.
#
# Run from a writable copy of this tree: make -f guest.mk

ICC = /bin/cc

include sources.mk

OBJS = $(ISH_SRC:.c=.o)

ish: $(OBJS)
	$(ICC) -nostdlib -static -o ish.new $(OBJS)
	mv ish.new ish

.SUFFIXES: .o .c

.c.o:
	$(ICC) -c -o $@ $<

clean:
	rm -f $(OBJS) ish ish.new
