# Unix Utilities Makefile (Rach2)
#
# Builds utilities for the Rach2 kernel (freestanding ELF32).
# For Linux builds, use: make -f Makefile.linux

# Cross-compiler detection
CC := $(shell command -v i686-elf-gcc 2>/dev/null)
ifeq ($(CC),)
	CC := $(shell command -v x86_64-elf-gcc 2>/dev/null)
endif

CFLAGS = -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000

UTILITIES = true  \
            false \
            echo  \
            yes   \
            cat   \
            pwd   \
            touch \
            mkdir \
            rm    \
            mv    \
            cp    \
            ls    \
            ping  \
            nc    \
            httpd

.PHONY: all clean

all: crt0.o $(UTILITIES)

crt0.o: crt0.c
	$(CC) $(CFLAGS) -c -o $@ $<

%: %.c crt0.o
	$(CC) $(CFLAGS) -o $@ crt0.o $<

# ping and nc link the shared stub resolver (resolv.c, declared in netdb.h).
ping: ping.c resolv.c netdb.h crt0.o
	$(CC) $(CFLAGS) -o $@ crt0.o ping.c resolv.c

nc: nc.c resolv.c netdb.h crt0.o
	$(CC) $(CFLAGS) -o $@ crt0.o nc.c resolv.c

clean:
	rm -f $(UTILITIES) crt0.o
