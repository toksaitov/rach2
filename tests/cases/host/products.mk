# products.mk: compile the POSIX products and C fixtures before a collection run.
# The caller supplies a GNU cross compiler and an absolute output directory.
include tools/etude/sources.mk

PRODUCT_CFLAGS = -O2 -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
    -D_FILE_OFFSET_BITS=64 -Iutils -Itools/etude -Itools/etude/include -Itools/etude/posix
PRODUCT_KRFFS = krffs/krffs_file_system.c krffs/krffs_node.c \
    krffs/krffs_platform.c krffs/krffs_utilities.c
PRODUCT_FLOW = $(wildcard tests/cases/host/flow_*.c) tests/cases/host/fixture.c utils/sha256.c \
    tools/etude/volume/volume.c
PRODUCT_BUILD = $(wildcard tests/cases/host/build_*.c) tests/cases/host/fixture.c utils/sha256.c
PRODUCT_FAMILY = $(filter-out tests/cases/host/family_scenario.c,\
    $(wildcard tests/cases/host/family_*.c)) tests/cases/host/fixture.c utils/sha256.c
PRODUCT_PROGRAMS = host-flow-check host-build-check host-family-check drive stageimage.krffs mkfs.krffs \
    setidcheck sha256 etude etude-volume host host-suite-probe doom_libc-i686 doom_libc-x86_64 \
    growset growboot growroot growroot-memory reflash diskctl bootctl mkfatfixture \
    readfile.krffs binfind pty-secret image-fixture

.PHONY: all
all: $(addprefix $(PREFIX)/,$(PRODUCT_PROGRAMS))

$(PREFIX)/host-flow-check: $(PRODUCT_FLOW)
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -Itools/etude/volume -o $@ $^
$(PREFIX)/host-build-check: $(PRODUCT_BUILD)
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/host-family-check: $(PRODUCT_FAMILY)
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/drive: tests/cases/host/host_products_drive.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/image-fixture: tests/cases/host/image_fixture.c tests/cases/host/fixture.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/stageimage.krffs: $(PRODUCT_KRFFS) krffs/krffs_allocator.c krffs/stageimage.krffs.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/mkfs.krffs: $(PRODUCT_KRFFS) krffs/mkfs.krffs.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/readfile.krffs: $(PRODUCT_KRFFS) krffs/krffs_reader.c krffs/readfile.krffs.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/binfind: tools/inspect/binfind.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/pty-secret: tests/system/fixtures/pty_secret.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/setidcheck: tools/inspect/setidcheck.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/sha256: tools/verify/sha256.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/growset: utils/growset.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/growboot: utils/growboot.c utils/bootslot.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/growroot: utils/growroot.c utils/gptdisk.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/growroot-memory: utils/growroot.c utils/gptdisk.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -DGROWROOT_MEMORY_MIB=1048576u -o $@ $^
$(PREFIX)/reflash: utils/reflash.c utils/sectorcopy.c utils/sha256.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/diskctl: utils/diskctl.c utils/gptdisk.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/bootctl: utils/bootctl.c utils/bootslot.c utils/bootverb.c utils/grubslot.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/mkfatfixture: tests/host/mkfatfixture.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/etude: $(ETUDE_SRC)
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/etude-volume: $(ETUDE_VOLUME_SRC)
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/host: $(ETUDE_HOSTCASE_SRC)
	@mkdir -p $(@D)
	$(CC) $(PRODUCT_CFLAGS) -o $@ $^
$(PREFIX)/host-suite-probe: tests/cases/host/host_suite_probe.c
	@mkdir -p $(@D)
	$(SANITIZER_CC) $(PRODUCT_CFLAGS) -fsanitize=address,undefined \
	    -fno-sanitize-recover=all -O1 -g -o $@ $^
$(PREFIX)/doom_libc-%: ports/doom/crt0.c tests/host/fixtures/doom_libc.c
	@mkdir -p $(@D)
	$*-elf-gcc -std=gnu99 -Os -ffreestanding -fno-pie -no-pie -nostdlib -static \
	    -Wl,--build-id=none -Iports/doom/include $^ -o $@
