# ish/Makefile: build the ish shell for Rach2, a freestanding static program
# for the architecture mk/arch.mk names, ELF32 i386 into build/ish/ or ELF64
# x86-64 into build/x86_64/ish/. Makefile.linux builds it for Linux.

# The cross toolchain, the header dependencies and the build directory (mk/).
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

# The objects and the program are written under $(BUILD), build/ish/
# (mk/build.mk), where the image manifests take the program from. The sources
# are listed in sources.mk, which guest.mk includes too.
include sources.mk
TARGET = $(BUILD)/ish
OBJECTS = $(addprefix $(BUILD)/, $(ISH_SRC:.c=.o))

# The configuration fingerprint (mk/fingerprint.mk): a changed flag or a
# reinstalled compiler rebuilds every object.
CONFIG_TEXT = $(CC) $(CFLAGS) $(LDFLAGS)
include ../mk/fingerprint.mk

.PHONY: all clean

all: $(TARGET)
$(OBJECTS): $(CONFIG)

$(TARGET): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

# The `$(BUILD)/%.o: %.c` compile rule and `-include $(OBJECTS:.o=.d)` come from
# ../mk/rules.mk, so an edited header rebuilds the objects that include it.
include ../mk/rules.mk

clean:
	rm -rf $(BUILD)
