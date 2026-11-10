# term/guest.mk: build term and libtsm on Rach2 with pdpmake and the staged
# TinyCC.
#
# The compiler is /bin/cc, spelt out because a system with GCC, the GNU root
# among them, has another cc first on its path. It compiles for the machine
# it runs on, so one makefile serves both architectures. libtsm's patched
# sources are staged beside this file as the archive vendor/libtsm.tar.bz2,
# which vendor/libtsm/build_libtsm.sh makes, and their list as
# vendor/libtsm.mk, the host's vendor/libtsm/sources.mk. term's first
# prerequisite unpacks the archive into vendor/libtsm, with the time of
# unpacking and the user's ownership, so that pdpmake, which takes the
# prerequisites in order, finds every source before it infers how to make an
# object. The objects of both are linked directly, with musl's libc.a and
# TinyCC's libtcc1.a.
#
# A POSIX makefile has one rule for every .c, so libtsm's flags, those of
# build_libtsm.sh, are term's too. TinyCC does not define
# __SIZEOF_POINTER__, which libtsm's shl-macro.h tests, so it is given here
# from the machine's architecture. -Dwcwidth=tsm_wcwidth keeps libtsm's
# wcwidth() apart from musl's, as on the host.
#
# Run from a writable copy of this tree, or from /src/term as root:
# make -f guest.mk

TCC = /bin/cc

LIBTSM = vendor/libtsm
# The pointer size of $(TCC), from its ELF header's class, and not of the
# kernel `uname -m` names, which may be x86-64 under an i686 system.
POINTER_BYTES != od -An -tx1 -j4 -N1 $(TCC) | sed -e 's/^ *01$$/4/' -e 's/^ *02$$/8/'
TCFLAGS = -Wall -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -Dwcwidth=tsm_wcwidth \
	-D__SIZEOF_POINTER__=$(POINTER_BYTES) -I$(LIBTSM)/src/tsm -I$(LIBTSM)/src/shared \
	-I$(LIBTSM)/external -I$(LIBTSM)/external/wcwidth

include sources.mk
include vendor/libtsm.mk

OBJS = $(TERM_SRC:.c=.o) $(LIBTSM_SRC:.c=.o)

# The link writes term.new and renames it, so a link stopped part way leaves
# no program.
term: $(LIBTSM)/.unpacked $(OBJS)
	$(TCC) -static -o term.new $(OBJS)
	mv term.new term

$(LIBTSM)/.unpacked: $(LIBTSM).tar.bz2
	rm -rf $(LIBTSM)
	mkdir $(LIBTSM)
	tar -xmojf $(LIBTSM).tar.bz2 -C $(LIBTSM)
	touch $@

.SUFFIXES: .o .c

.c.o:
	$(TCC) $(TCFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) term term.new
	rm -rf $(LIBTSM)
