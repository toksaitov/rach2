# The GNU layer of milestone 54 (vendor/gnu/Readme.md). It builds on a Linux
# host and is not part of `all`: `make gnu-toolchain` builds the i686-linux-gnu
# cross toolchain under build/vendor/gnu/ from the tarballs vendor/gnu/fetch.sh
# downloads and checks.
GNU_OUT = build/vendor/gnu
GNU_TOOLCHAIN_STAMP = $(GNU_OUT)/toolchain.stamp
GNU_CC = $(GNU_OUT)/cross/bin/i686-linux-gnu-gcc

.PHONY: gnu-toolchain
gnu-toolchain: $(GNU_TOOLCHAIN_STAMP)

$(GNU_TOOLCHAIN_STAMP): vendor/gnu/build_toolchain.sh vendor/gnu/fetch.sh vendor/gnu/sources.txt
	@rm -f $@
	+@./vendor/gnu/build_toolchain.sh
	@test -x $(GNU_CC) || { echo "gnu: toolchain: the build produced no $(GNU_CC)" >&2; exit 1; }
	@touch $@
