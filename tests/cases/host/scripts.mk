# scripts.mk: one native scenario and immutable sources for host.list.
HOST_SCRIPT_CASE_SRC = tests/cases/host/host_scripts_scenario.c \
    tests/cases/host/host_script_inventory.c \
    tests/cases/host/host_assets.c \
    tests/cases/host/host_scripts.c tests/cases/host/host_build.c tests/cases/host/host_doom.c \
    tests/cases/host/host_image.c tests/cases/host/host_image_accounts.c \
    tests/cases/host/host_image_krffs.c tests/cases/host/host_image_overlay.c \
    tests/cases/host/host_image_tooling.c tests/cases/host/host_image_collection.c \
    tests/cases/machine/netcase.c
build/tests/cases/host/host_scripts_scenario: $(HOST_SCRIPT_CASE_SRC) \
    tests/cases/host/host_script_inventory.h tools/etude/history_json.h \
    tests/cases/host/host_scripts.h tests/cases/host/host_build.h tests/cases/host/host_doom.h \
    tests/cases/host/host_image.h tests/cases/host/host_assets.h \
    tests/cases/machine/netcase.h $(ETUDE_CASE_LIB_SRC) $(ETUDE_CASE_LIB_HDRS)
	@mkdir -p $(@D)
	$(ETUDE_LINK) -std=c99 -D_DEFAULT_SOURCE -Itools/etude/include -Itools/etude \
	    -Itools/etude/posix -Itools/etude/guest -Iutils

HOST_SCRIPT_ASSETS = $(wildcard $(shell git ls-files tools tests mk Makefile krffs \
    utils rach2 ish ports vendor/gnu vendor/tcc/asm-test vendor/tcc/ld-test))
build/tests/host-suite/scripts-assets.tar: $(HOST_SCRIPT_ASSETS)
	@mkdir -p $(@D)
	COPYFILE_DISABLE=1 tar --format=ustar -czf $@ $(HOST_SCRIPT_ASSETS)

build/tests/host-suite/sources/%: %
	@mkdir -p $(@D)
	cp $< $@

# The GNU products use their declared x86-64 kernel on either collection ABI.
ifneq ($(ARCH),x86_64)
.PHONY: prepare-host-products-x86_64
prepare-host-products-x86_64:
	+$(MAKE) --no-print-directory ARCH=x86_64 rach2 rach2-recovery \
	    build/x86_64/images/gnu-test.disk build/x86_64/images/etude-image.krffs
build/x86_64/rach2/kernel build/x86_64/images/gnu-test.disk \
build/x86_64/rach2-recovery/kernel build/x86_64/images/etude-image.krffs: | prepare-host-products-x86_64
	@test -s $@
endif

# These native binaries are preparation goals of host.host.scripts themselves.
HOST_KERNEL_FIXTURES = chown krffs_open krffs_root_lock krffs_stat pagecache kstack endothers
HOST_KERNEL_FIXTURE_INPUTS = tests/cases/host/fixtures.py tests/cases/host/routing.py \
    tests/host/fixtures/extract.py $(wildcard tests/host/fixtures/*.c tests/host/fixtures/*.h) \
    tests/host/pagecache_hosttest.c tests/host/krffs_hosttest.c \
    tests/host/krffsmount_hosttest.c tests/host/krffs_hash_vectors.h \
    tests/host/compat/vfs_name_none.c rach2/sys/fs/vfs.c rach2/sys/fs/vfs.h \
    rach2/sys/fs/fd.c rach2/sys/fs/krffs.c rach2/sys/fs/krffsmount.c \
    rach2/sys/mm/pagecache.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c \
    rach2/sys/arch/arch.h rach2/sys/arch/x86/isr.h rach2/sys/arch/x86/pic.h \
    rach2/sys/block/block.h rach2/sys/display/textcon.h rach2/sys/io/inotify.h \
    rach2/sys/mm/kheap.h rach2/sys/mm/pagecache.h rach2/sys/mm/vm.h \
    rach2/sys/fpu.h rach2/sys/irq.h rach2/sys/kio.h rach2/sys/klog.h \
    rach2/sys/panic.h rach2/sys/rtc.h rach2/sys/serial.h \
    rach2/sys/syscall_fs.c rach2/sys/syscall_proc.c rach2/sys/syscall_internal.h \
    rach2/sys/proc/process.h rach2/sys/arch/i686/cpu.h rach2/sys/arch/i686/uabi.h \
    $(wildcard rach2/sys/lib/*.h rach2/sys/fs/*.h rach2/sys/proc/*.h)
$(addprefix build/tests/host-suite/scripts/,$(HOST_KERNEL_FIXTURES)): \
    build/tests/host-suite/scripts/%: $(HOST_KERNEL_FIXTURE_INPUTS)
	@mkdir -p $(@D)
	CC="$(HOSTCC)" python3 -B tests/cases/host/fixtures.py $* $@

# The i686 freestanding Doom ABI runs on a matching small recovery helper.
HOST_DOOM_LIBC_SRC = ports/doom/crt0.c tests/host/fixtures/doom_libc.c
build/tests/fixtures/doom-libc-i686: $(HOST_DOOM_LIBC_SRC) $(wildcard ports/doom/include/*.h ports/doom/include/sys/*.h)
	@mkdir -p $(@D)
	i686-elf-gcc -std=gnu99 -Os -ffreestanding -fno-pie -no-pie -nostdlib -static \
	    -Wl,--build-id=none -Iports/doom/include $(HOST_DOOM_LIBC_SRC) -o $@

ifneq ($(ARCH),i686)
.PHONY: prepare-host-doom-i686
prepare-host-doom-i686:
	+$(MAKE) --no-print-directory ARCH=i686 rach2-recovery build/images/etude-image.krffs
build/rach2-recovery/kernel build/images/etude-image.krffs: | prepare-host-doom-i686
	@test -s $@
endif
