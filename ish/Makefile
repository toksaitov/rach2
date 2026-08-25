# ISH Shell Makefile (Rach2)
#
# Builds the shell for the Rach2 kernel (freestanding ELF32).
# For Linux builds, use: make -f Makefile.linux

# Shared cross toolchain + automatic header-dependency tracking (see ../mk/).
include ../mk/toolchain.mk
include ../mk/deps.mk
include ../mk/build.mk
CC := $(CROSS_CC)

CFLAGS = -m32 -ffreestanding $(DEPFLAGS)
LDFLAGS = -m32 -nostdlib -static -Wl,-Ttext=0x08048000

# Everything builds out of tree under $(BUILD) (build/ish/, from mk/build.mk): the
# objects and the ish binary the RAM-disk staging picks up. The source list
# lives in sources.mk, which guest.mk includes too, so one file describes
# both builds.
include sources.mk
TARGET = $(BUILD)/ish
OBJECTS = $(addprefix $(BUILD)/, $(ISH_SRC:.c=.o))

# The configuration fingerprint (mk/fingerprint.mk): a changed flag or a
# reinstalled compiler rebuilds every object automatically.
CONFIG_TEXT = $(CC) $(CFLAGS) $(LDFLAGS)
include ../mk/fingerprint.mk

.PHONY: all clean

all: $(TARGET)


$(TARGET): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

# The `$(BUILD)/%.o: %.c` compile rule and `-include $(OBJECTS:.o=.d)` come from
# ../mk/rules.mk; a header edit now rebuilds exactly the objects that include it
# (the old hand-maintained $(HEADERS) prerequisite list is gone).
include ../mk/rules.mk

clean:
	rm -rf $(BUILD)
