# Unix Utilities Makefile (Rach2)
#
# Builds the utilities as ordinary hosted C programs linked statically against
# the ported musl (see vendor/musl/build_libc.sh), the same way tests/musl/build.sh
# links its programs: musl's crt1/crti + libc.a + crtn via the cross compiler,
# with the freestanding crt0.c and hand-rolled int-0x80 stubs dropped. Each
# program now uses the real libc headers (<stdio.h>, <unistd.h>, <dirent.h>, ...).
#
# Requires vendor/musl/install (run vendor/musl/build_libc.sh once first). For Linux host
# builds, use: make -f Makefile.linux

# Shared header-dependency tracking + the toolchain guard (see ../mk/). The musl
# utilities are special: they link the i686 libc AND a 32-bit libgcc that
# build_libc.sh's i686-elf-gcc produced, so they need i686-elf-gcc specifically:
# a non-multilib x86_64-elf-gcc cannot supply a 32-bit libgcc (the freestanding
# components link no libgcc, so they use the generic CROSS_CC, which may be
# x86_64-elf-gcc). So prefer i686-elf-gcc, falling back to CROSS_CC; -m32 is then a
# no-op but keeps the i686 ABI explicit.
include ../mk/toolchain.mk
include ../mk/deps.mk
include ../mk/build.mk
CC := $(or $(shell command -v i686-elf-gcc 2>/dev/null),$(CROSS_CC))

# Ported musl install tree (headers + static libc.a + crt objects).
PREFIX  := ../vendor/musl/install
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
            httpd \
            sudo  \
            su

# init is PID 1, staged at /sbin/init, not a user tool. It builds into a sbin/
# subdir so the os-simple `exec-dir / build/utils` sweep (which stages only the
# regular files directly in build/utils, skipping subdirs) does not also drop it
# at the image root as /init.
SBIN_INIT = $(BUILD)/sbin/init

TARGETS = $(addprefix $(BUILD)/, $(UTILITIES))

.PHONY: all clean libc-check

all: $(TARGETS) $(SBIN_INIT)

$(TARGETS) $(SBIN_INIT): | libc-check

# init: the same musl link as the utils, but staged at /sbin/init (see above).
$(SBIN_INIT): init.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/init.d -o $@ $(LINK_PRE) $< $(LINK_POST)

# Fail early with a clear message if the ported libc has not been built.
libc-check:
	@test -f "$(PREFIX)/lib/libc.a" || { \
	    echo "error: $(PREFIX)/lib/libc.a not found; run vendor/musl/build_libc.sh first" >&2; \
	    exit 1; }

# Each <name>.c compiles+links in one step into a static musl guest binary under
# $(BUILD) (build/utils/, from mk/build.mk); -MF puts the GCC-written .d there too.
# There are no .o intermediates and nothing lands in the source tree.
$(BUILD)/%: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/$*.d -o $@ $(LINK_PRE) $< $(LINK_POST)

# ping and nc link the shared stub resolver (resolv.c, declared in netdb.h). These
# compile+link two sources in one step (-MF records only the last source's headers),
# so the inputs (the program source, resolv.c, netdb.h) are explicit prerequisites; the
# $(BUILD) .d adds the resolver's transitive headers.
$(BUILD)/ping: ping.c resolv.c netdb.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/ping.d -o $@ $(LINK_PRE) ping.c resolv.c $(LINK_POST)

$(BUILD)/nc: nc.c resolv.c netdb.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/nc.d -o $@ $(LINK_PRE) nc.c resolv.c $(LINK_POST)

-include $(addprefix $(BUILD)/, $(UTILITIES:=.d)) $(BUILD)/sbin/init.d

clean:
	rm -rf $(BUILD)
