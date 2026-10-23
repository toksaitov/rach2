# Cached sanitizer preparation, separate from the GNU compiler and ports.
GNU_SANITIZERS_STAMP = $(GNU_OUT)/sanitizers.stamp
GNU_SANITIZERS = $(GNU_OUT)/sanitizers
GNU_SANITIZER_ROOT = $(GNU_SANITIZERS)/root
GNU_SANITIZER_COMPILER = $(abspath $(GNU_SANITIZERS)/compiler)
GNU_SANITIZER_FLAGS = -B$(GNU_SANITIZER_COMPILER)/ \
                      -L$(abspath $(GNU_SANITIZER_ROOT))/usr/lib
GNU_SANITIZER_CC = $(GNU_CC) $(GNU_SANITIZER_FLAGS)
GNU_SANITIZER_CXX = $(GNU_OUT)/cross/bin/$(GNU_TARGET)-g++ $(GNU_SANITIZER_FLAGS)
GNU_SANITIZER_SOURCES = vendor/gnu/build_sanitizers.sh vendor/gnu/arch.sh \
                       vendor/gnu/fetch.sh vendor/gnu/sources.txt
GNU_STEP_STAMPS += $(GNU_SANITIZERS_STAMP)

.PHONY: gnu-sanitizers
gnu-sanitizers: $(GNU_SANITIZERS_STAMP)

$(GNU_SANITIZERS_STAMP): $(GNU_SANITIZER_SOURCES) $(GNU_TOOLCHAIN_STAMP) | $(ETUDE)
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else \
	    $(GNU_STEP) -n gnu-sanitizers -t $@ $(STEP_INPUTS) -u cross -u sysroot -o sanitizers \
	        -- $(call GNU_STEP_RUN,vendor/gnu/build_sanitizers.sh); fi
	@test -f $(GNU_SANITIZERS)/compiler/libsanitizer.spec && \
	    test -f $(GNU_SANITIZERS)/compiler/libasan_preinit.o && \
	    test -e $(GNU_SANITIZER_ROOT)/usr/lib/libasan.so && \
	    test -e $(GNU_SANITIZER_ROOT)/usr/lib/libubsan.so || \
	    { echo 'gnu: sanitizers: the prepared overlay is incomplete' >&2; exit 1; }
