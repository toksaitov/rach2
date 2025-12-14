# Rach2 Syscall Test Programs Makefile
#
# Builds freestanding ELF32 executables for testing kernel syscalls.
# These programs run in user mode on the Rach2 kernel.

# Try to find a suitable cross-compiler
CC := $(shell command -v i686-elf-gcc 2>/dev/null)
ifeq ($(CC),)
	CC := $(shell command -v x86_64-elf-gcc 2>/dev/null)
endif
ifeq ($(CC),)
	CC := gcc
endif

CFLAGS = -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000

# All test programs
PROGS = hello filetest forktest shelltest dirtest stattest \
        touchtest rmtest mvtest lstest stdintest dup2test

.PHONY: all clean

all: $(PROGS)

# Generic rule: build program from .c file
%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGS)
