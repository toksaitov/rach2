# ISH Shell Makefile (Rach2)
#
# Builds the shell for the Rach2 kernel (freestanding ELF32).
# For Linux builds, use: make -f Makefile.linux

# Shared cross toolchain + automatic header-dependency tracking (see ../mk/).
include ../mk/toolchain.mk
include ../mk/deps.mk
CC := $(CROSS_CC)

CFLAGS = -m32 -ffreestanding $(DEPFLAGS)
LDFLAGS = -m32 -nostdlib -static -Wl,-Ttext=0x08048000

TARGET = ish

OBJECTS = crt0.o ish.o ish_shell_utilities.o ish_cstring_utilities.o ish_syscalls.o

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

# The `%.o: %.c` compile rule and `-include $(OBJECTS:.o=.d)` come from
# ../mk/rules.mk -- a header edit now rebuilds exactly the objects that include it
# (the old hand-maintained $(HEADERS) prerequisite list is gone).
include ../mk/rules.mk

clean:
	rm -f $(TARGET) $(OBJECTS) $(OBJECTS:.o=.d)
