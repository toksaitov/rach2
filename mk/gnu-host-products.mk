# Build the products and controllers once for the GNU test helper.
GNU_HOST_PRODUCTS_STAMP = $(GNU_OUT)/host-products.stamp
GNU_HOST_PRODUCTS_SOURCES = vendor/gnu/build_host_products.sh vendor/gnu/arch.sh \
    tests/cases/host/products.mk $(wildcard tests/cases/host/*.c tests/cases/host/*.h) \
    $(ETUDE_CHECK_SRC) $(ETUDE_RUN_SRC) $(ETUDE_POSIX_SRC) \
    tests/cases/host/host.c $(ETUDE_VOLUME_SRC) $(ETUDE_HDRS) \
    $(wildcard krffs/*.c krffs/*.h vendor/tcc/rach2-*.patch) $(TCC_TARBALL) \
    vendor/tcc/build_tcc.sh ports/doom/crt0.c ports/doom/format.c ports/doom/format.h \
    tests/host/fixtures/doom_libc.c \
    $(wildcard ports/doom/include/*.h ports/doom/include/sys/*.h) \
    tools/inspect/setidcheck.c tools/verify/sha256.c utils/sha256.c utils/sha256.h \
    $(wildcard utils/grow*.c utils/grow*.h utils/boot*.c utils/boot*.h) \
    utils/reflash.c utils/sectorcopy.c utils/sectorcopy.h utils/diskctl.c utils/gptdisk.c \
    utils/gptdisk.h utils/grubslot.c utils/grubslot.h tests/host/mkfatfixture.c \
    tools/inspect/binfind.c tests/system/fixtures/pty_secret.c tests/host/mkfatfixture.h
GNU_STEP_STAMPS += $(GNU_HOST_PRODUCTS_STAMP)

.PHONY: gnu-host-products
gnu-host-products: $(GNU_HOST_PRODUCTS_STAMP)

$(GNU_HOST_PRODUCTS_STAMP): $(GNU_HOST_PRODUCTS_SOURCES) $(GNU_PORTS_STAMP) \
    $(GNU_SANITIZERS_STAMP) $(E2FSPROGS_STAMP) $(MTOOLS_STAMP) | $(ETUDE)
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else \
	    $(GNU_STEP) -n gnu-host-products -t $@ $(STEP_INPUTS) \
	        -u cross -u sysroot -u root -u sanitizers -o host-products \
	        -- $(call GNU_STEP_RUN,vendor/gnu/build_host_products.sh); fi
	@test -x $(GNU_OUT)/host-products/host-flow-check && \
	    test -x $(GNU_OUT)/host-products/host-suite-probe
