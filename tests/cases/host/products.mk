# products.mk: compile the POSIX products and C fixtures before a collection run.
# The caller supplies a GNU cross compiler and an absolute output directory.
include tools/etude/sources.mk

PRODUCT_CFLAGS = -O2 -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
    -D_FILE_OFFSET_BITS=64 -Iutils -Itools/etude -Itools/etude/include -Itools/etude/posix
PRODUCT_KRFFS = krffs/krffs_file_system.c krffs/krffs_node.c \
    krffs/krffs_platform.c krffs/krffs_utilities.c
PRODUCT_FLOW = $(wildcard tests/cases/host/flow_*.c) tests/cases/host/fixture.c utils/sha256.c
PRODUCT_BUILD = $(wildcard tests/cases/host/build_*.c) tests/cases/host/fixture.c utils/sha256.c
PRODUCT_PROGRAMS = host-flow-check host-build-check drive stageimage.krffs mkfs.krffs \
    setidcheck sha256 prepared/etude prepared/etude-volume prepared/host \
    prepared/doom_libc-i686 prepared/doom_libc-x86_64

.PHONY: all
all: $(addprefix $(PREFIX)/,$(PRODUCT_PROGRAMS))

$(PREFIX)/host-flow-check: $(PRODUCT_FLOW)
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/host-build-check: $(PRODUCT_BUILD)
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/drive: tests/cases/host/host_products_drive.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/stageimage.krffs: $(PRODUCT_KRFFS) krffs/krffs_allocator.c krffs/stageimage.krffs.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/mkfs.krffs: $(PRODUCT_KRFFS) krffs/mkfs.krffs.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/setidcheck: tools/inspect/setidcheck.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/sha256: tools/verify/sha256.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/prepared/etude: $(ETUDE_SRC)
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/prepared/etude-volume: $(ETUDE_VOLUME_SRC)
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/prepared/host: $(ETUDE_HOSTCASE_SRC)
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/prepared/doom_libc-%: ports/doom/crt0.c tests/host/fixtures/doom_libc.c
	@mkdir -p $(@D)
	$*-elf-gcc -std=gnu99 -Os -ffreestanding -fno-pie -no-pie -nostdlib -static \
	    -Wl,--build-id=none -Iports/doom/include $^ -o $@
