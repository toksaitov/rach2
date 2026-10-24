# Compile POSIX host fixtures once for the declared x86-64 GNU helper.
GNU_HOST_CHECKS_STAMP = $(GNU_OUT)/host-checks.stamp
GNU_HOST_CHECKS_SOURCES = vendor/gnu/build_host_inventory.sh \
    $(wildcard $(shell git ls-files Makefile CLAUDE.md AGENTS.md Readme.md \
        tests tools mk utils krffs rach2 ish ports vendor \
        2>/dev/null || tr '\0' '\n' < build/gnu-source-files 2>/dev/null))
GNU_HOST_CHECKS_MUSL = $(notdir $(LIBC_TARBALL:.tar.gz=))
GNU_HOST_CHECKS_E2FS = $(notdir $(E2FSPROGS_TARBALL:.tar.gz=))
GNU_HOST_CHECKS_EXTRA = \
    vendor/musl/$(GNU_HOST_CHECKS_MUSL)/src/crypt/crypt_sha512.c \
    build/x86_64/vendor-work/musl/$(GNU_HOST_CHECKS_MUSL)/src/crypt/crypt_sha512.c \
    vendor/e2fsprogs/$(GNU_HOST_CHECKS_E2FS)/lib \
    build/x86_64/vendor-work/e2fsprogs/$(GNU_HOST_CHECKS_E2FS)/lib \
    build/tests/host/exfat_writer build/tests/host-suite/tools/exfat_external
GNU_HOST_CHECKS_INPUTS = $(foreach f,$(filter-out gnu-in-copy gnu-host-check-inputs,$^),$(if $(filter %.stamp,$f),-d,-i) $f)
GNU_STEP_STAMPS += $(GNU_HOST_CHECKS_STAMP)

.PHONY: gnu-host-checks gnu-host-check-inputs
gnu-host-checks: $(GNU_HOST_CHECKS_STAMP)

# Both ABI inventories compile the actual patched upstream source trees.
# The step rechecks their bytes after preparation, including generated headers
# and every independent exFAT fixture file, before taking a cached output.
gnu-host-check-inputs:
	+$(MAKE) --no-print-directory ARCH=i686 musl e2fsprogs
	+$(MAKE) --no-print-directory ARCH=x86_64 musl e2fsprogs

$(GNU_HOST_CHECKS_STAMP): $(GNU_HOST_CHECKS_SOURCES) $(GNU_HOST_PRODUCTS_STAMP) \
    $(GNU_SANITIZERS_STAMP) build/tests/host-suite/tools/exfat_external/exfat_external.img \
    gnu-host-check-inputs | $(ETUDE)
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else \
	    $(GNU_STEP) -n gnu-host-checks -t $@ $(GNU_HOST_CHECKS_INPUTS) \
	        $(foreach f,$(GNU_HOST_CHECKS_EXTRA),-i $(f)) \
	        -u cross -u sysroot -u root -u sanitizers -u host-products -o host-checks \
	        -- $(call GNU_STEP_RUN,vendor/gnu/build_host_inventory.sh); fi
	@test -x $(GNU_OUT)/host-checks/driver && \
	    test -f $(GNU_OUT)/host-checks/tree/build/tests/host-suite/etude.inventory.json
