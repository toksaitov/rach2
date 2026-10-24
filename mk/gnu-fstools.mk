# Independent filesystem helper tools, without changing GNU ports or root.
GNU_FSTOOLS_STAMP = $(GNU_OUT)/fstools.stamp
GNU_FSTOOLS_SOURCES = vendor/gnu/fstools/build.sh vendor/gnu/fstools/sources.txt \
                      vendor/gnu/arch.sh mk/arch.sh
GNU_STEP_STAMPS += $(GNU_FSTOOLS_STAMP)

.PHONY: gnu-fstools
gnu-fstools: $(GNU_FSTOOLS_STAMP)

$(GNU_FSTOOLS_STAMP): $(GNU_FSTOOLS_SOURCES) $(GNU_TOOLCHAIN_STAMP) | $(ETUDE) $(SHA256TOOL)
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else \
	    $(GNU_STEP) -n gnu-fstools -t $@ $(STEP_INPUTS) -u cross -u sysroot -o fstools \
	        -- $(call GNU_STEP_RUN,vendor/gnu/fstools/build.sh); fi
	@test -x $(GNU_OUT)/fstools/usr/sbin/mkfs.exfat && \
	    test -x $(GNU_OUT)/fstools/usr/sbin/fsck.exfat || \
	    { echo 'gnu: fstools: the independent helper tools are incomplete' >&2; exit 1; }
