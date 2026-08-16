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
            halt

# init is PID 1, and su and sudo climb privilege; none of the three is a user tool
# to be swept into an image root. They build into a sbin/ subdir so the os-simple
# `exec-dir / build/utils` sweep (which stages only the regular files directly in
# build/utils, skipping subdirs) cannot stage them at all. Every image that wants
# one names it explicitly, which is also what makes the set-id allowlist in
# tools/check_setid.py a complete list rather than a sample: a privileged program
# reaches an image only through a `file` line naming it.
SBIN_PROGS = init-shell init-login init-metal login su sudo passwd adduser adduser-crash install-kernel

TARGETS      = $(addprefix $(BUILD)/, $(UTILITIES))
SBIN_TARGETS = $(addprefix $(BUILD)/sbin/, $(SBIN_PROGS))

.PHONY: all clean libc-check prune-stale

all: prune-stale $(TARGETS) $(SBIN_TARGETS)

$(TARGETS) $(SBIN_TARGETS): | libc-check

# `exec-dir` stages every regular file directly in build/utils, so a binary an
# earlier build left there (one since renamed, dropped from UTILITIES, or moved
# into sbin/) would go on being staged from a tree that no longer builds it. The
# build is out of tree and incremental, so nothing else removes it. Sweep the top
# level down to the current targets first; the sbin/ subdirectory is untouched,
# and its contents are staged by name rather than by sweep.
prune-stale:
	@keep=" $(UTILITIES) "; \
	for f in $(BUILD)/*; do \
	    [ -f "$$f" ] || continue; \
	    b=`basename "$$f"`; \
	    case "$$b" in *.d) b=`echo "$$b" | sed 's/\.d$$//'`;; esac; \
	    case "$$keep" in \
	        *" $$b "*) ;; \
	        *) echo "utils: removing stale $$f"; rm -f "$$f";; \
	    esac; \
	done

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

# The sbin family: the same musl link, into build/utils/sbin/ (see above).
$(BUILD)/sbin/%: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/$*.d -o $@ $(LINK_PRE) $< $(LINK_POST)

# su links the credential library (pwauth.c, declared in pwauth.h), the way ping
# and nc link the resolver above and for the same reason: the -MF .d records only
# the last source's headers, so the inputs are explicit prerequisites here.
$(BUILD)/sbin/su: su.c pwauth.c pwauth.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/su.d -o $@ $(LINK_PRE) su.c pwauth.c $(LINK_POST)

# passwd links the credential library for the same reason su does, and is the
# first program that writes through it.
$(BUILD)/sbin/passwd: passwd.c pwauth.c pwauth.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/passwd.d -o $@ $(LINK_PRE) passwd.c pwauth.c $(LINK_POST)

# init builds three times from the one source, and no artifact is named plain
# `init`: a manifest left on the old path fails to stage rather than silently
# getting whichever init the tree happens to build. init-shell is today's
# behavior unchanged; init-login execs /bin/login and nothing else, with the
# shell table and its exec loop compiled out, so the strict artifact holds no
# code path and no string that names a shell; init-metal is init-login plus a
# supervised SSH server, for the machine nobody is standing in front of.
$(BUILD)/sbin/init-shell: init.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/init-shell.d -o $@ $(LINK_PRE) init.c $(LINK_POST)

$(BUILD)/sbin/init-login: init.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DINIT_CONSOLE_LOGIN -MF $(BUILD)/sbin/init-login.d -o $@ \
	    $(LINK_PRE) init.c $(LINK_POST)

$(BUILD)/sbin/init-metal: init.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DINIT_CONSOLE_LOGIN -DINIT_METAL -MF $(BUILD)/sbin/init-metal.d -o $@ \
	    $(LINK_PRE) init.c $(LINK_POST)

# login links the credential library: it is the console's authentication.
$(BUILD)/sbin/login: login.c pwauth.c pwauth.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/login.d -o $@ $(LINK_PRE) login.c pwauth.c $(LINK_POST)

# adduser links the credential library the same way, and builds twice from the
# one source: as shipped, and as a crash build that dies the instant a named
# publication has landed, so the ordering the account's consistency rests on is
# proved by injection rather than by a reading of the source. Everything the
# second build adds lives under -DADDUSER_CRASH_POINT, so the shipped binary has
# neither the extra argument nor the code, and only the test image stages it.
$(BUILD)/sbin/adduser: adduser.c pwauth.c pwauth.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/adduser.d -o $@ $(LINK_PRE) adduser.c pwauth.c $(LINK_POST)

# install-kernel links the boot-slot library (bootslot.c), the pure core its
# host seam test drives over fixture images with fault injection.
$(BUILD)/sbin/install-kernel: install-kernel.c bootslot.c bootslot.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/install-kernel.d -o $@ $(LINK_PRE) install-kernel.c bootslot.c $(LINK_POST)

$(BUILD)/sbin/adduser-crash: adduser.c pwauth.c pwauth.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DADDUSER_CRASH_POINT -MF $(BUILD)/sbin/adduser-crash.d -o $@ \
	    $(LINK_PRE) adduser.c pwauth.c $(LINK_POST)

-include $(addprefix $(BUILD)/, $(UTILITIES:=.d)) \
        $(addprefix $(BUILD)/sbin/, $(SBIN_PROGS:=.d))

clean:
	rm -rf $(BUILD)
