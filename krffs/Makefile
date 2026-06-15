# KRFFS host tools: the FUSE mount plus the mkfs/defrag/fsck/fsviz/edfs/stagetree
# utilities. These run on the HOST, so they use the host compiler (cc), not the
# cross toolchain -- only the shared header-dependency tracking (../mk/deps.mk)
# applies, so editing a krffs header rebuilds exactly the objects that include it.
include ../mk/deps.mk

CFLAGS += -g -std=gnu99 $(DEPFLAGS) -D_FILE_OFFSET_BITS=64

FUSE_PKG := $(shell pkg-config --exists fuse 2>/dev/null && echo fuse || echo fuse-t)
FUSE_CFLAGS += `pkg-config $(FUSE_PKG) --cflags` -DFUSE_USE_VERSION=26
FUSE_LDLIBS += `pkg-config $(FUSE_PKG) --libs`

# Out-of-tree intermediates under .build/ (git-ignored); the host-tool binaries stay
# in place -- the RAM-disk scripts run ./mkfs.krffs, ./krffs, ./stagetree.krffs here.
BUILD = .build

# The four objects every tool shares.
CORE = $(addprefix $(BUILD)/, krffs_file_system.o krffs_node.o krffs_platform.o \
                              krffs_utilities.o)

# Objects compiled WITH the FUSE cflags: the mount tool and everything linked into
# it, including the shared core. The standalone tool mains are compiled WITHOUT the
# FUSE cflags -- exactly the split the previous per-target rules produced.
FUSE_OBJS = $(CORE) $(addprefix $(BUILD)/, krffs_allocator.o krffs_fuse_operations.o \
                                           krffs.o)
TOOL_OBJS = $(addprefix $(BUILD)/, mkfs.krffs.o defrag.krffs.o fsck.krffs.o \
                                   fsviz.krffs.o edfs.krffs.o stagetree.krffs.o)

OBJECTS = $(FUSE_OBJS) $(TOOL_OBJS)
TARGETS = krffs mkfs.krffs defrag.krffs fsck.krffs fsviz.krffs edfs.krffs \
          stagetree.krffs

.PHONY : all
all : $(TARGETS)

# The FUSE mount tool: the FUSE objects + the FUSE libraries.
krffs : $(FUSE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(FUSE_LDLIBS)

# stagetree also needs the allocator (it writes KRFFS nodes directly).
stagetree.krffs : $(CORE) $(BUILD)/krffs_allocator.o $(BUILD)/stagetree.krffs.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# The remaining standalone tools: the shared core + the tool's own object.
mkfs.krffs defrag.krffs fsck.krffs fsviz.krffs edfs.krffs : %.krffs : $(CORE) $(BUILD)/%.krffs.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Compile rules: FUSE objects get the FUSE cflags, tool mains do not. Each makes its
# .build/ output dir first.
$(FUSE_OBJS) : $(BUILD)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -c $< -o $@
$(TOOL_OBJS) : $(BUILD)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Pull in the per-object header dependencies GCC wrote (-MMD), now beside the objects.
-include $(OBJECTS:.o=.d)

.PHONY : clean
clean :
	rm -rf $(BUILD)
	rm -f $(TARGETS)
	@# Sweep any stray in-tree objects/deps; prune mount_point so a still-mounted FUSE
	@# image is never traversed.
	@find . -path ./mount_point -prune -o \( -name '*.o' -o -name '*.d' \) -type f -exec rm -f {} +
