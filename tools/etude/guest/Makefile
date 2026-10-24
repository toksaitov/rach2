# tools/etude/guest/Makefile: build etude-guest for Rach2 with the cross
# compiler, a hosted C99 program linked statically against the ported musl as
# utils/Makefile links the utilities. The program lands at
# build/tools/etude/guest/etude-guest, or under build/x86_64/ (mk/build.mk),
# and the test image stages it. tests/host/etude_guest_hosttest.c compiles the
# same sources with the host's compiler. fixture.c compiles the fixtures that
# need the kernel under __linux__ or __rach2__, and the cross compiler defines
# no operating system, so the build defines __rach2__.
TOP := ../../..
COMPONENT := tools/etude/guest
include $(TOP)/mk/toolchain.mk
include $(TOP)/mk/build.mk
CC := $(CROSS_CC)

PREFIX  := $(TOP)/$(ARCH_MUSL)
LIBGCC  := $(shell $(CC) $(ARCH_CFLAGS) -print-libgcc-file-name)
CRT1    := $(PREFIX)/lib/crt1.o
CRTI    := $(PREFIX)/lib/crti.o
CRTN    := $(PREFIX)/lib/crtn.o

CFLAGS  = $(ARCH_CFLAGS) -std=c99 -static -nostdlib -nostartfiles -isystem $(PREFIX)/include \
          -D__rach2__ -I . -I ../include -I .. -I $(TOP)/utils -O2 -Wall -Wextra -MMD -MP
LINK_PRE  = $(CRT1) $(CRTI)
LINK_POST = $(PREFIX)/lib/libc.a $(LIBGCC) $(CRTN) -Wl,-z,noexecstack $(ARCH_LDFLAGS)

# The program's own sources, the JSON reader of the history store, which
# reads the invocations, and the SHA-256 of utils/. They are compiled and
# linked in one step, as utils/ links ping and nc, so -MF records the headers
# of the last source only and the sources and headers are named here.
SRC = guest.c fixture.c wire.c ../boot_identity.c ../history_json.c $(TOP)/utils/sha256.c
HDR = fixture.h wire.h ../boot_identity.h ../history_json.h \
      ../include/etude/frame.h $(TOP)/utils/sha256.h

CONFIG_TEXT = $(CC) $(CFLAGS) $(LINK_PRE) $(LINK_POST)
include $(TOP)/mk/fingerprint.mk

.PHONY: all clean libc-check
all: $(BUILD)/etude-guest $(BUILD)/ptydrive

clean:
	rm -rf $(BUILD)

libc-check:
	@test -f "$(PREFIX)/lib/libc.a" || { \
	    echo "etude-guest: $(PREFIX)/lib/libc.a not found; run make musl in the top directory first" >&2; \
	    exit 1; }

$(BUILD)/etude-guest: $(SRC) $(HDR) $(CONFIG) $(CRT1) $(CRTI) $(PREFIX)/lib/libc.a $(LIBGCC) \
                      $(CRTN) | libc-check
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/etude-guest.d -o $@ $(LINK_PRE) $(SRC) $(LINK_POST)

-include $(BUILD)/etude-guest.d

# The SSH scenarios run this terminal driver inside their Rach2 client,
# under the same framed command controller as the client itself.
$(BUILD)/ptydrive: $(TOP)/tests/system/runner/ptydrive.c $(CONFIG) $(CRT1) $(CRTI) \
                   $(PREFIX)/lib/libc.a $(LIBGCC) $(CRTN) | libc-check
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -MF $(BUILD)/ptydrive.d -o $@ $(LINK_PRE) $< $(LINK_POST)

-include $(BUILD)/ptydrive.d
