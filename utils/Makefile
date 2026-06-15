# Unix Utilities Makefile (Rach2)
#
# Builds the utilities as ordinary hosted C programs linked statically against
# the ported musl (see libc/build_libc.sh), the same way tests/musl/build.sh
# links its programs: musl's crt1/crti + libc.a + crtn via the cross compiler,
# with the freestanding crt0.c and hand-rolled int-0x80 stubs dropped. Each
# program now uses the real libc headers (<stdio.h>, <unistd.h>, <dirent.h>, ...).
#
# Requires libc/install (run libc/build_libc.sh once first). For Linux host
# builds, use: make -f Makefile.linux

# Shared header-dependency tracking + the toolchain guard (see ../mk/). The musl
# utilities are special: they link the i686 libc AND a 32-bit libgcc that
# build_libc.sh's i686-elf-gcc produced, so they need i686-elf-gcc specifically --
# a non-multilib x86_64-elf-gcc cannot supply a 32-bit libgcc (the freestanding
# components link no libgcc, so they use the generic CROSS_CC, which may be
# x86_64-elf-gcc). So prefer i686-elf-gcc, falling back to CROSS_CC; -m32 is then a
# no-op but keeps the i686 ABI explicit.
include ../mk/toolchain.mk
include ../mk/deps.mk
CC := $(or $(shell command -v i686-elf-gcc 2>/dev/null),$(CROSS_CC))

# Ported musl install tree (headers + static libc.a + crt objects).
PREFIX  := ../libc/install
LIBGCC  := $(shell $(CC) -m32 -print-libgcc-file-name)
CRT1    := $(PREFIX)/lib/crt1.o
CRTI    := $(PREFIX)/lib/crti.o
CRTN    := $(PREFIX)/lib/crtn.o

CFLAGS  = -m32 -static -nostdlib -nostartfiles -isystem $(PREFIX)/include \
          -O2 -Wall -Wextra $(DEPFLAGS)
# crt1/crti come before the program objects; libc + libgcc + crtn after.
LINK_PRE  = $(CRT1) $(CRTI)
LINK_POST = $(PREFIX)/lib/libc.a $(LIBGCC) $(CRTN) -Wl,-z,noexecstack

UTILITIES = true  \
            false \
            echo  \
            yes   \
            cat   \
            pwd   \
            touch \
            mkdir \
            rm    \
            mv    \
            cp    \
            ls    \
            ping  \
            nc    \
            httpd

.PHONY: all clean libc-check

all: $(UTILITIES)

$(UTILITIES): | libc-check

# Fail early with a clear message if the ported libc has not been built.
libc-check:
	@test -f "$(PREFIX)/lib/libc.a" || { \
	    echo "error: $(PREFIX)/lib/libc.a not found; run libc/build_libc.sh first" >&2; \
	    exit 1; }

# Each <name>.c links into a static musl guest binary <name>.
%: %.c
	$(CC) $(CFLAGS) -o $@ $(LINK_PRE) $< $(LINK_POST)

# ping and nc link the shared stub resolver (resolv.c, declared in netdb.h).
ping: ping.c resolv.c netdb.h
	$(CC) $(CFLAGS) -o $@ $(LINK_PRE) ping.c resolv.c $(LINK_POST)

nc: nc.c resolv.c netdb.h
	$(CC) $(CFLAGS) -o $@ $(LINK_PRE) nc.c resolv.c $(LINK_POST)

-include $(UTILITIES:=.d)

clean:
	rm -f $(UTILITIES) $(UTILITIES:=.d)
