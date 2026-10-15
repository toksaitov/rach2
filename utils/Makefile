# utils/Makefile: build the Rach2 utilities with the cross compiler, as hosted
# C programs linked statically against the ported musl.
#
# The link is the one tests/musl/build.sh makes: musl's crt1.o and crti.o, the
# program, then libc.a, the compiler's libgcc and crtn.o. The programs include
# musl's headers. musl's install tree is $(ARCH_MUSL) (mk/arch.mk), which
# `make musl` in the top directory builds. For native Linux builds of the file
# utilities: make -f Makefile.linux

# Shared header-dependency tracking and the toolchain check (../mk/). The
# utilities link the ported libc of the architecture mk/arch.mk names and the
# libgcc of the compiler that built it, so they take that architecture's
# compiler, which mk/toolchain.mk gives as CROSS_CC: i686-elf-gcc, whose -m32
# is a no-op but keeps the i686 ABI explicit, or x86_64-elf-gcc with -m64. The
# programs land in build/utils/, or build/x86_64/utils/ (mk/build.mk).
include ../mk/toolchain.mk
include ../mk/deps.mk
include ../mk/build.mk
CC := $(CROSS_CC)

# The ported musl's install tree: its headers, libc.a and crt objects.
PREFIX  := $(TOP)/$(ARCH_MUSL)
LIBGCC  := $(shell $(CC) $(ARCH_CFLAGS) -print-libgcc-file-name)
CRT1    := $(PREFIX)/lib/crt1.o
CRTI    := $(PREFIX)/lib/crti.o
CRTN    := $(PREFIX)/lib/crtn.o

CFLAGS  = $(ARCH_CFLAGS) -static -nostdlib -nostartfiles -isystem $(PREFIX)/include \
          -O2 -Wall -Wextra $(DEPFLAGS)
# Flags an analysis adds from the command line (tools/verify/analyze.sh).
EXTRA ?=
CFLAGS += $(EXTRA)
# crt1.o and crti.o come before the program; libc.a, libgcc and crtn.o after.
LINK_PRE  = $(CRT1) $(CRTI)
LINK_POST = $(PREFIX)/lib/libc.a $(LIBGCC) $(CRTN) -Wl,-z,noexecstack $(ARCH_LDFLAGS)

# The program lists and the per-program source map are in programs.mk, which
# guest.mk includes too, so one file describes both builds.
#
# The SBIN_PROGS, the inits, the programs that check or change credentials and
# bootctl, are not user tools to be swept into an image root: init is PID 1, and
# su and sudo climb privilege. They build into a sbin/ subdirectory, which the
# os-simple manifest's `exec-dir /bin build/utils` does not stage, since
# `exec-dir` stages only the regular files directly in its directory. Every
# image that wants one names it explicitly, which is also what makes the set-id
# allowlist in tools/inspect/setid.allow a complete list rather than a sample: a
# privileged program reaches an image only through a `file` line naming it.
include programs.mk

TARGETS      = $(addprefix $(BUILD)/, $(UTILITIES))
SBIN_TARGETS = $(addprefix $(BUILD)/sbin/, $(SBIN_PROGS))

# The configuration fingerprint (mk/fingerprint.mk): a changed flag, link input,
# or reinstalled compiler rebuilds every program automatically.
CONFIG_TEXT = $(CC) $(CFLAGS) $(LINK_PRE) $(LINK_POST)
include ../mk/fingerprint.mk

.PHONY: all clean libc-check prune-stale

all: prune-stale $(TARGETS) $(SBIN_TARGETS)
$(TARGETS) $(SBIN_TARGETS): $(CONFIG) $(CRT1) $(CRTI) \
    $(PREFIX)/lib/libc.a $(LIBGCC) $(CRTN)

$(TARGETS) $(SBIN_TARGETS): | libc-check

# `exec-dir` stages every regular file directly in build/utils, so a binary an
# earlier build left there (one since renamed, dropped from UTILITIES, or moved
# into sbin/) would go on being staged from a tree that no longer builds it. The
# build is out of tree and incremental, so nothing else removes it.
#
# sbin/ is swept too. Its programs are staged by name, so a stale binary there
# is reachable only through a manifest line naming it, and the set-id check
# (tools/inspect/setidcheck.c) compares a `file` line's declared source with
# its own table and does not look at the host file. A rename that missed one
# manifest line would go on staging the old binary with nothing to report it.
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
	@keep=" $(SBIN_PROGS) "; \
	for f in $(BUILD)/sbin/*; do \
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

# Each NAME.c is compiled and linked in one step into a static guest program
# under $(BUILD) (mk/build.mk), and -MF puts GCC's .d file there too. There are
# no .o files, and nothing is written in the source tree.
$(BUILD)/%: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/$*.d -o $@ $(LINK_PRE) $< $(LINK_POST)

# ping and nc link the stub resolver (resolv.c, declared in netdb.h). Each is
# compiled and linked from two sources in one step, and -MF records the
# headers of the last source only, so the sources and the headers HDR_* names
# are explicit prerequisites; the .d file adds the resolver's own headers.
$(BUILD)/ping: $(SRC_ping) $(HDR_ping)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/ping.d -o $@ $(LINK_PRE) $(SRC_ping) $(LINK_POST)

$(BUILD)/nc: $(SRC_nc) $(HDR_nc)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/nc.d -o $@ $(LINK_PRE) $(SRC_nc) $(LINK_POST)

# The sbin family: the same musl link, into build/utils/sbin/ (see above).
$(BUILD)/sbin/%: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/$*.d -o $@ $(LINK_PRE) $< $(LINK_POST)

# su links the credential library (pwauth.c, declared in pwauth.h), the way ping
# and nc link the resolver above and for the same reason: the -MF .d records
# only the last source's headers, so the inputs are explicit prerequisites here.
$(BUILD)/sbin/su: $(SRC_su) $(HDR_su)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/su.d -o $@ $(LINK_PRE) $(SRC_su) $(LINK_POST)

# passwd links the credential library for the same reason su does, and writes
# the account files through it.
$(BUILD)/sbin/passwd: $(SRC_passwd) $(HDR_passwd)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/passwd.d -o $@ $(LINK_PRE) $(SRC_passwd) $(LINK_POST)

# init builds four times from the one source, and no program is named plain
# `init`: a manifest that names that path fails to stage rather than getting
# whichever init the tree builds. init-shell gives a shell at the console and
# supervises nothing; init-login execs /bin/login and nothing else, with the
# shell table and its exec loop compiled out, so it has no code path and no
# string that names a shell; init-metal is init-login with a supervised SSH
# server, for a machine nobody is standing in front of; init-recovery is the
# recovery system's, described in programs.mk.
$(BUILD)/sbin/init-shell: $(SRC_init_shell)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/init-shell.d -o $@ $(LINK_PRE) $(SRC_init_shell) $(LINK_POST)

$(BUILD)/sbin/init-login: $(SRC_init_login)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEF_init_login) -MF $(BUILD)/sbin/init-login.d -o $@ \
	    $(LINK_PRE) $(SRC_init_login) $(LINK_POST)

$(BUILD)/sbin/init-metal: $(SRC_init_metal)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEF_init_metal) -MF $(BUILD)/sbin/init-metal.d -o $@ \
	    $(LINK_PRE) $(SRC_init_metal) $(LINK_POST)

$(BUILD)/sbin/init-recovery: $(SRC_init_recovery) $(HDR_init_recovery)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEF_init_recovery) -MF $(BUILD)/sbin/init-recovery.d -o $@ \
	    $(LINK_PRE) $(SRC_init_recovery) $(LINK_POST)

# login links the credential library: it is the console's authentication.
$(BUILD)/sbin/login: $(SRC_login) $(HDR_login)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/login.d -o $@ $(LINK_PRE) $(SRC_login) $(LINK_POST)

# adduser links the credential library the same way, and builds twice from the
# one source: as shipped, and as a crash build that dies the instant a named
# publication has landed, so the ordering the account's consistency rests on is
# proved by injection rather than by a reading of the source. Everything the
# second build adds is under -DADDUSER_CRASH_POINT, so the shipped binary has
# neither the extra argument nor the code, and only the test image stages it.
$(BUILD)/sbin/adduser: $(SRC_adduser) $(HDR_adduser)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/adduser.d -o $@ $(LINK_PRE) $(SRC_adduser) $(LINK_POST)

# dhcpc links the message library (dhcpmsg.c), the half of the client that needs
# no system call: the framing, the checksums and the option parsing that its
# host test drives with the messages a server will not produce on request. netup
# links the configuration reader (netcfg.c) for the same reason. Both link
# ipcheck.c, the one judgement of whether an IPv4 configuration is one a machine
# can hold, which a lease off the wire and a stanza in a file are equally
# subject to.
$(BUILD)/dhcpc: $(SRC_dhcpc) $(HDR_dhcpc)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/dhcpc.d -o $@ $(LINK_PRE) $(SRC_dhcpc) \
	    $(LINK_POST)

# reflash links the copy library (sectorcopy.c): the numbers that decide where a
# write lands, and the loop that turns a stream into whole sectors.
$(BUILD)/reflash: $(SRC_reflash) $(HDR_reflash)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/reflash.d -o $@ $(LINK_PRE) $(SRC_reflash) \
	    $(LINK_POST)

$(BUILD)/growboot: $(SRC_growboot) $(HDR_growboot)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/growboot.d -o $@ $(LINK_PRE) $(SRC_growboot) $(LINK_POST)

# growroot and diskctl link the GUID table library (gptdisk.c), which reads a
# disk's identity and rewrites its table with the backup at the disk's end.
$(BUILD)/growroot: $(SRC_growroot) $(HDR_growroot)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/growroot.d -o $@ $(LINK_PRE) $(SRC_growroot) $(LINK_POST)

$(BUILD)/diskctl: $(SRC_diskctl) $(HDR_diskctl)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/diskctl.d -o $@ $(LINK_PRE) $(SRC_diskctl) $(LINK_POST)

$(BUILD)/netup: $(SRC_netup) $(HDR_netup)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/netup.d -o $@ $(LINK_PRE) $(SRC_netup) \
	    $(LINK_POST)

# bootctl links the boot-slot library (bootslot.c), which reads and writes
# through callbacks, so that its host test drives it over images in memory with
# injected faults.
$(BUILD)/sbin/bootctl: $(SRC_bootctl) $(HDR_bootctl)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/sbin/bootctl.d -o $@ $(LINK_PRE) $(SRC_bootctl) $(LINK_POST)

$(BUILD)/sbin/adduser-crash: $(SRC_adduser_crash) $(HDR_adduser_crash)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEF_adduser_crash) -MF $(BUILD)/sbin/adduser-crash.d -o $@ \
	    $(LINK_PRE) $(SRC_adduser_crash) $(LINK_POST)

-include $(addprefix $(BUILD)/, $(UTILITIES:=.d)) \
        $(addprefix $(BUILD)/sbin/, $(SBIN_PROGS:=.d))

clean:
	rm -rf $(BUILD)
