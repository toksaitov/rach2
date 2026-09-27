# Build the ish shell on Rach2 itself.
#
# The POSIX makefile pdpmake reads in the guest, driving the staged tcc
# through /bin/cc, spelt out because a system with GCC, the GNU root among
# them, has another cc first on its path; the GNU Makefile beside it stays
# the host build. ish
# has its own crt0 and raw syscall stubs, so it links -nostdlib the way
# the host build does; the one host flag that is dropped is -Ttext, because
# tcc's default i386 text base is the same 0x08048000 the host build names
# explicitly. Both makefiles include sources.mk, so one file lists the
# sources for the two builds; tools/verify/check_guest.py checks the derivation.
#
# Run from a writable copy of this tree: make -f guest.mk

ICC = /bin/cc

include sources.mk

OBJS = $(ISH_SRC:.c=.o)

ish: $(OBJS)
	$(ICC) -m32 -nostdlib -static -o ish.new $(OBJS)
	mv ish.new ish

.SUFFIXES: .o .c

.c.o:
	$(ICC) -m32 -c -o $@ $<

clean:
	rm -f $(OBJS) ish ish.new
