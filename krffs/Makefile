# KRFFS host tools: the mkfs/defrag/fsck/fsviz/edfs/stageimage utilities, the
# mkhash password hasher, and the optional FUSE mount (`make mount`). These run
# on the HOST, so they use the host compiler (cc), not the cross toolchain; only
# the shared header-dependency tracking (../mk/deps.mk) applies, so editing a
# krffs header rebuilds exactly the objects that include it.
include ../mk/deps.mk
include ../mk/build.mk

CFLAGS += -g -std=gnu99 $(DEPFLAGS) -D_FILE_OFFSET_BITS=64

# FUSE is needed only by the optional mount tool (`make mount`), so the
# pkg-config probe is lazy (=): a default build neither runs pkg-config nor
# needs the FUSE headers installed.
FUSE_PKG = $(shell pkg-config --exists fuse 2>/dev/null && echo fuse || echo fuse-t)
FUSE_CFLAGS += `pkg-config $(FUSE_PKG) --cflags` -DFUSE_USE_VERSION=26
FUSE_LDLIBS += `pkg-config $(FUSE_PKG) --libs`

# Everything builds out of tree under $(BUILD) (build/krffs/, from mk/build.mk):
# the shared objects and the host-tool binaries the RAM-disk scripts run from
# there.

# The four objects every tool shares. Only krffs.c and krffs_fuse_operations.c
# include the FUSE interface, so the core, the allocator, and the tool mains
# compile without the FUSE cflags.
CORE = $(addprefix $(BUILD)/, krffs_file_system.o krffs_node.o krffs_platform.o \
                              krffs_utilities.o)

# The two objects of the optional mount tool, the only ones compiled WITH the
# FUSE cflags.
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

# mkhash's crypt is the vendored musl SHA-512 implementation, compiled from the
# pinned tarball's tree into build/ beside the tool, so the host mints exactly
# the hash the guest's libc verifies. The vendored source is used in place,
# never modified.
MUSL_CRYPT = $(TOP)/vendor/musl/musl-1.2.5/src/crypt/crypt_sha512.c

# The configuration fingerprint (mk/fingerprint.mk): a changed flag or host
# compiler rebuilds every object automatically. The optional mount tool's
# pkg-config flags stay out of the text so a default build never runs
# pkg-config.
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

# mkhash compiles the vendored musl crypt, which exists only once the pinned
# tarball has been extracted. Say so, rather than letting make report "No rule
# to make target" for a source nobody wrote by hand. The top-level build orders
# this behind check-libc; this is for a standalone `make -C krffs` in a clean
# tree.
libc-check :
	@test -f "$(MUSL_CRYPT)" || { \
	    echo "krffs: vendored musl source not extracted; run the root make first" >&2; \
	    exit 1; }

# The FUSE mount tool: the shared core and allocator + the two FUSE objects +
# the FUSE libraries.
$(BUILD)/krffs : $(CORE) $(BUILD)/krffs_allocator.o $(MOUNT_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(FUSE_LDLIBS)

$(BUILD)/krffs $(MOUNT_OBJS) : | fuse-check

# stageimage and fsck also need the allocator: one writes KRFFS nodes directly,
# the other frees the orphans a crash left behind.
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

# The remaining standalone tools: the shared core + the tool's own object.
$(addprefix $(BUILD)/, mkfs.krffs defrag.krffs fsviz.krffs edfs.krffs) : $(BUILD)/%.krffs : $(CORE) $(BUILD)/%.krffs.o
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# mkhash stands alone: its own main plus the vendored crypt, no KRFFS core.
$(BUILD)/mkhash.krffs : $(BUILD)/mkhash.krffs.o $(BUILD)/crypt_sha512.o
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/crypt_sha512.o : $(MUSL_CRYPT)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile rules: the two mount objects get the FUSE cflags, everything else does
# not. Each makes its $(BUILD) output dir first.
$(MOUNT_OBJS) : $(BUILD)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -c $< -o $@
$(TOOL_OBJS) : $(BUILD)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Pull in the per-object header dependencies GCC wrote (-MMD), now beside the
# objects.
-include $(OBJECTS:.o=.d)

.PHONY : clean
clean :
	rm -rf $(BUILD)
