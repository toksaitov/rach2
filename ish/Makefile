# ISH Shell Makefile (Rach2)
#
# Builds shell for the Rach2 kernel (freestanding ELF32).
# For Linux builds, use: make -f Makefile.linux

# Cross-compiler detection
CC := $(shell command -v i686-elf-gcc 2>/dev/null)
ifeq ($(CC),)
	CC := $(shell command -v x86_64-elf-gcc 2>/dev/null)
endif

CFLAGS = -m32 -ffreestanding
LDFLAGS = -m32 -nostdlib -static -Wl,-Ttext=0x08048000

TARGET = ish

OBJECTS = crt0.o ish.o ish_shell_utilities.o ish_cstring_utilities.o ish_syscalls.o
HEADERS = ish_common.h ish_syscalls.h ish_shell_utilities.h ish_cstring_utilities.h

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJECTS)
