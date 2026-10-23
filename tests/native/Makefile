# Makefile: cross-build the local collection against Rach2's ported musl.
TOP := ../..
COMPONENT := tests/native
include $(TOP)/mk/toolchain.mk
include $(TOP)/mk/build.mk
include sources.mk
include $(TOP)/tools/etude/sources.mk
CC := $(CROSS_CC)
PREFIX := $(TOP)/$(ARCH_MUSL)
LIBGCC := $(shell $(CC) $(ARCH_CFLAGS) -print-libgcc-file-name)
PRE = $(PREFIX)/lib/crt1.o $(PREFIX)/lib/crti.o
POST = $(PREFIX)/lib/libc.a $(LIBGCC) $(PREFIX)/lib/crtn.o -Wl,-z,noexecstack $(ARCH_LDFLAGS)
CFLAGS = $(ARCH_CFLAGS) -std=c99 -static -nostdlib -nostartfiles -isystem $(PREFIX)/include \
    -D__rach2__ -D_DEFAULT_SOURCE -O2 -Wall -Wextra -I$(TOP)/tools/etude/include \
    -I$(TOP)/tools/etude -I$(TOP)/tools/etude/posix -I$(TOP)/tools/etude/guest -I$(TOP)/utils
SRC = $(addprefix $(TOP)/,$(ETUDE_RACH2_SRC))
HDR = $(addprefix $(TOP)/,$(NATIVE_COLLECTION_HDR)) $(wildcard $(TOP)/tools/etude/*.h)
HDR += $(wildcard $(TOP)/tools/etude/include/etude/*.h)
CONFIG_TEXT = $(CC) $(CFLAGS) $(PRE) $(POST)
include $(TOP)/mk/fingerprint.mk
.PHONY: all
all: $(BUILD)/etude
$(BUILD)/etude: $(SRC) $(HDR) $(CONFIG) $(PRE) $(PREFIX)/lib/libc.a $(LIBGCC) $(PREFIX)/lib/crtn.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(PRE) $(SRC) $(POST)
