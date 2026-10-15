# ish/guest.mk: build the ish shell on Rach2 with pdpmake.
#
# The compiler is the staged TinyCC, named as /bin/cc because a system with
# GCC, the GNU root among them, has another cc first on its path. ish has its
# own crt0 and system call wrappers, so it links with -nostdlib as the host
# build does. The host build's word size and -Ttext are left out: TinyCC
# compiles for the machine it runs on, i386 or x86-64, refuses the other's -m
# flag, and places the text where the host build does (mk/arch.mk), so one
# makefile serves both architectures. The sources are listed in sources.mk,
# which the host Makefile includes too; tools/verify/guestcheck.c checks that
# the two object lists agree.
#
# Run from a writable copy of this tree, or from /src/ish as root:
# make -f guest.mk

ICC = /bin/cc

include sources.mk

OBJS = $(ISH_SRC:.c=.o)

# The link writes ish.new and renames it, so a link stopped part way leaves
# no program.
ish: $(OBJS)
	$(ICC) -nostdlib -static -o ish.new $(OBJS)
	mv ish.new ish

.SUFFIXES: .o .c

.c.o:
	$(ICC) -c -o $@ $<

clean:
	rm -f $(OBJS) ish ish.new
