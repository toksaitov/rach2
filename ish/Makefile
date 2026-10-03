# ISH Shell Makefile (Rach2)
#
# Builds the shell for the Rach2 kernel, a freestanding static executable of the
# architecture mk/arch.mk names: ELF32 i386 into build/ish/, or ELF64 x86-64
# into build/x86_64/ish/. For Linux builds, use: make -f Makefile.linux

# Shared cross toolchain + automatic header-dependency tracking (see ../mk/).
include ../mk/toolchain.mk
include ../mk/deps.mk
include ../mk/build.mk
CC := $(CROSS_CC)

CFLAGS = $(ARCH_CFLAGS) -ffreestanding $(DEPFLAGS)
# Flags an analysis adds from the command line (tools/verify/analyze.sh).
EXTRA ?=
CFLAGS += $(EXTRA)
# The text is placed where TinyCC places a program's on the architecture, so a
# guest build of ish has the host build's layout (mk/arch.mk).
LDFLAGS = $(ARCH_CFLAGS) -nostdlib -static -Wl,-Ttext=$(ARCH_USER_TEXT) -Wl,--build-id=none $(ARCH_LDFLAGS)

# Everything builds out of tree under $(BUILD) (build/ish/, from mk/build.mk):
# the objects and the ish binary the RAM-disk staging picks up. The source list
# lives in sources.mk, which guest.mk includes too, so one file describes both
# builds.
include sources.mk
TARGET = $(BUILD)/ish
OBJECTS = $(addprefix $(BUILD)/, $(ISH_SRC:.c=.o))

# The configuration fingerprint (mk/fingerprint.mk): a changed flag or a
# reinstalled compiler rebuilds every object automatically.
CONFIG_TEXT = $(CC) $(CFLAGS) $(LDFLAGS)
include ../mk/fingerprint.mk

.PHONY: all clean

all: $(TARGET)
$(OBJECTS): $(CONFIG)

$(TARGET): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

# The `$(BUILD)/%.o: %.c` compile rule and `-include $(OBJECTS:.o=.d)` come from
# ../mk/rules.mk; a header edit now rebuilds exactly the objects that include it
# (the old hand-maintained $(HEADERS) prerequisite list is gone).
include ../mk/rules.mk

clean:
	rm -rf $(BUILD)
