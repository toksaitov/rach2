# Rach2 Syscall Test Programs Makefile
#
# Builds freestanding ELF32 executables for testing kernel syscalls.
# These programs run in user mode on the Rach2 kernel.

# Shared cross toolchain + automatic header-dependency tracking (see ../mk/).
include ../mk/toolchain.mk
include ../mk/deps.mk
CC := $(CROSS_CC)

CFLAGS = -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 $(DEPFLAGS)

# All test programs
PROGS = hello filetest forktest shelltest dirtest stattest \
        touchtest rmtest mvtest lstest stdintest dup2test \
        pingtest udptest tcptest sockettest dnstest auxvprobe \
        tlsprobe errnoprobe vmprobe vmfault vmro forkro cowprobe dirprobe \
        timeprobe sigprobe ofdprobe waitprobe pipeprobe pollprobe devprobe \
        ttyprobe jobprobe miscprobe ptyprobe rngprobe

.PHONY: all clean

all: $(PROGS)

# Each probe is a single freestanding source compiled+linked in one step; -MMD
# (from $(DEPFLAGS)) writes a <prog>.d, so editing a shared header rebuilds it.
%: %.c
	$(CC) $(CFLAGS) -o $@ $<

# dnstest links the shared stub resolver from utils/ so it can exercise both the
# pure parser (crafted vectors) and resolve_host() (live).
dnstest: dnstest.c ../utils/resolv.c ../utils/netdb.h
	$(CC) $(CFLAGS) -o $@ dnstest.c ../utils/resolv.c

-include $(PROGS:=.d)

clean:
	rm -f $(PROGS) $(PROGS:=.d)
