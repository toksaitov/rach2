# Build the products and controllers once for the GNU test helper.
GNU_HOST_PRODUCTS_STAMP = $(GNU_OUT)/host-products.stamp
GNU_HOST_PRODUCTS_SOURCES = vendor/gnu/build_host_products.sh vendor/gnu/arch.sh \
    tests/cases/host/products.mk $(wildcard tests/cases/host/*.c tests/cases/host/*.h) \
    $(ETUDE_CHECK_SRC) $(ETUDE_RUN_SRC) $(ETUDE_POSIX_SRC) \
    tests/cases/host/host.c $(ETUDE_VOLUME_SRC) $(ETUDE_HDRS) \
    $(wildcard krffs/*.c krffs/*.h vendor/tcc/rach2-*.patch) $(TCC_TARBALL) \
    vendor/tcc/build_tcc.sh ports/doom/crt0.c tests/host/fixtures/doom_libc.c \
    tools/inspect/setidcheck.c tools/verify/sha256.c utils/sha256.c utils/sha256.h
GNU_STEP_STAMPS += $(GNU_HOST_PRODUCTS_STAMP)

.PHONY: gnu-host-products
gnu-host-products: $(GNU_HOST_PRODUCTS_STAMP)

$(GNU_HOST_PRODUCTS_STAMP): $(GNU_HOST_PRODUCTS_SOURCES) $(GNU_PORTS_STAMP) \
    $(GNU_SANITIZERS_STAMP) $(E2FSPROGS_STAMP) | $(ETUDE)
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else \
	    $(GNU_STEP) -n gnu-host-products -t $@ $(STEP_INPUTS) \
	        -u cross -u sysroot -u root -u sanitizers -o host-products \
	        -- $(call GNU_STEP_RUN,vendor/gnu/build_host_products.sh); fi
	@test -x $(GNU_OUT)/host-products/host-flow-check && \
	    test -x $(GNU_OUT)/host-products/host-suite-probe
