# krffs/Makefile: build the KRFFS host tools, mkfs, fsck, defrag, edfs, fsviz,
# readfile and stageimage, and the mkhash password hasher into build/krffs/,
# and, with `make mount`, the optional FUSE driver. They run on the build
# machine, so they are compiled with the host compiler and the host flags of
# mk/host.mk, not the cross toolchain.
include ../mk/deps.mk
include ../mk/build.mk
include ../mk/host.mk

# The host policy's flags with this component's own: gnu99 for the tools'
# dialect, the large-file offsets the image tools need, and the header
# dependencies. `HOSTCFLAGS` is where -Wall -Wextra -Werror come from, so a
# warning added there reaches these tools as it reaches every other host
# program.
CFLAGS += $(HOSTCFLAGS) -std=gnu99 $(DEPFLAGS) -D_FILE_OFFSET_BITS=64

# FUSE is needed only by the optional mount tool (`make mount`), so the
# pkg-config probe is expanded only when a FUSE object is built: a default
# build neither runs pkg-config nor needs the FUSE headers.
FUSE_PKG = $(shell pkg-config --exists fuse 2>/dev/null && echo fuse || echo fuse-t)
FUSE_CFLAGS += `pkg-config $(FUSE_PKG) --cflags` -DFUSE_USE_VERSION=26
FUSE_LDLIBS += `pkg-config $(FUSE_PKG) --libs`

# The objects and the tools are written under $(BUILD), build/krffs/
# (mk/build.mk), where the image scripts run the tools from.

# The four objects every KRFFS tool links. Only krffs.c and
# krffs_fuse_operations.c include the FUSE interface, so the core, the
# allocator and the tools' mains compile without the FUSE flags.
CORE = $(addprefix $(BUILD)/, krffs_file_system.o krffs_node.o krffs_platform.o \
                              krffs_utilities.o)

# The two objects of the optional mount tool, the only ones compiled with the
# FUSE flags.
MOUNT_OBJS = $(addprefix $(BUILD)/, krffs_fuse_operations.o krffs.o)
TOOL_OBJS = $(CORE) $(addprefix $(BUILD)/, krffs_allocator.o \
                                   mkfs.krffs.o defrag.krffs.o fsck.krffs.o \
                                   fsviz.krffs.o edfs.krffs.o stageimage.krffs.o \
                                   mkhash.krffs.o readfile.krffs.o \
                                   krffs_reader.o)

OBJECTS = $(MOUNT_OBJS) $(TOOL_OBJS) $(BUILD)/crypt_sha512.o
TARGETS = $(addprefix $(BUILD)/, mkfs.krffs defrag.krffs fsck.krffs \
                                 fsviz.krffs edfs.krffs stageimage.krffs \
                                 mkhash.krffs readfile.krffs)

# mkhash's crypt is musl's SHA-512 implementation, compiled from the extracted
# tree of the pinned tarball, unmodified, so the host makes the hash the
# guest's C library checks.
MUSL_CRYPT = $(TOP)/vendor/musl/musl-1.2.5/src/crypt/crypt_sha512.c

# The configuration fingerprint (mk/fingerprint.mk): a changed flag or host
# compiler rebuilds every object. The mount tool's pkg-config flags stay out
# of the text, so that a default build does not run pkg-config.
CONFIG_TEXT = $(CC) $(CFLAGS)
include ../mk/fingerprint.mk

.PHONY : all mount libc-check fuse-check
all : $(TARGETS)
$(OBJECTS): $(CONFIG)

# The FUSE mount tool is not part of the default build: staging and checking
# images never mounts one, and FUSE is the one host library the other tools do
# not need. `make -C krffs mount` builds it where FUSE is installed.
mount : $(BUILD)/krffs

fuse-check :
	@pkg-config --exists fuse 2>/dev/null || pkg-config --exists fuse-t 2>/dev/null || { \
	    echo "krffs: the mount tool needs FUSE: install fuse-t (macOS) or libfuse-dev and pkg-config" >&2; \
	    exit 1; }

$(TARGETS) : | libc-check

# mkhash compiles musl's crypt, which exists only once the pinned tarball has
# been extracted. The root Makefile builds this component after check-libc;
# this check is for a `make -C krffs` in a clean tree, which would otherwise
# stop with make's "No rule to make target".
libc-check :
	@test -f "$(MUSL_CRYPT)" || { \
	    echo "krffs: vendored musl source not extracted; run the root make first" >&2; \
	    exit 1; }

# The FUSE mount tool: the core, the allocator, the two FUSE objects and the
# FUSE libraries.
$(BUILD)/krffs : $(CORE) $(BUILD)/krffs_allocator.o $(MOUNT_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(FUSE_LDLIBS)

$(BUILD)/krffs $(MOUNT_OBJS) : | fuse-check

# fsck and stageimage also link the allocator: fsck frees the nameless
# reserved nodes a stopped machine leaves, and stageimage allocates the nodes
# it stages.
$(BUILD)/fsck.krffs : $(CORE) $(BUILD)/krffs_allocator.o $(BUILD)/fsck.krffs.o
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/stageimage.krffs : $(CORE) $(BUILD)/krffs_allocator.o $(BUILD)/stageimage.krffs.o
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# readfile also links the reader, the path walk the host test runs on crafted
# regions (tests/host/readfile_hosttest.c).
$(BUILD)/readfile.krffs : $(CORE) $(BUILD)/krffs_reader.o $(BUILD)/readfile.krffs.o
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# The other tools: the core and the tool's own object.
$(addprefix $(BUILD)/, mkfs.krffs defrag.krffs fsviz.krffs edfs.krffs) : $(BUILD)/%.krffs : $(CORE) $(BUILD)/%.krffs.o
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# mkhash links its own main and musl's crypt, and no KRFFS code.
$(BUILD)/mkhash.krffs : $(BUILD)/mkhash.krffs.o $(BUILD)/crypt_sha512.o
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# The one object compiled from a vendored source, and the one compiled
# without the host warning flags: musl's code is written for musl's flags,
# and `-Werror` on it would make an upgrade of the pinned tarball a patching
# job.
$(BUILD)/crypt_sha512.o : $(MUSL_CRYPT)
	@mkdir -p $(dir $@)
	$(CC) $(filter-out $(HOSTCFLAGS),$(CFLAGS)) -O2 -c $< -o $@

# The compile rules: the two mount objects get the FUSE flags, and the others
# do not.
$(MOUNT_OBJS) : $(BUILD)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -c $< -o $@
$(TOOL_OBJS) : $(BUILD)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CFLAGS_$(notdir $*)) -c $< -o $@

# fsviz includes the vendored termbox2 whole, which ignores the result of a
# read and a write that the host's headers mark; that warning is reported
# and not made an error for this one object.
CFLAGS_fsviz.krffs = -Wno-error=unused-result

# The header dependencies the compiler wrote beside each object (-MMD).
-include $(OBJECTS:.o=.d)

.PHONY : clean
clean :
	rm -rf $(BUILD)
