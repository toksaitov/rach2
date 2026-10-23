# Compile POSIX host fixtures once for the declared x86-64 GNU helper.
GNU_HOST_CHECKS_STAMP = $(GNU_OUT)/host-checks.stamp
GNU_HOST_CHECKS_SOURCES = vendor/gnu/build_host_inventory.sh \
    $(wildcard $(shell git ls-files Makefile tests tools mk utils krffs rach2 ish ports vendor \
        2>/dev/null || tr '\0' '\n' < build/gnu-source-files 2>/dev/null))
GNU_STEP_STAMPS += $(GNU_HOST_CHECKS_STAMP)

.PHONY: gnu-host-checks
gnu-host-checks: $(GNU_HOST_CHECKS_STAMP)

$(GNU_HOST_CHECKS_STAMP): $(GNU_HOST_CHECKS_SOURCES) $(GNU_HOST_PRODUCTS_STAMP) \
    $(GNU_SANITIZERS_STAMP) build/tests/host-suite/tools/exfat_external/exfat_external.img | $(ETUDE)
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else \
	    $(GNU_STEP) -n gnu-host-checks -t $@ $(STEP_INPUTS) \
	        -u cross -u sysroot -u root -u sanitizers -u host-products -o host-checks \
	        -- $(call GNU_STEP_RUN,vendor/gnu/build_host_inventory.sh); fi
	@test -x $(GNU_OUT)/host-checks/driver && \
	    test -f $(GNU_OUT)/host-checks/tree/build/tests/host-suite/etude.inventory.json
