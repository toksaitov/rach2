# mk/gnu.mk: the GNU layer (vendor/gnu/Readme.md). It builds on a Linux host
# and is not part of `all`. `make gnu-toolchain` builds the cross toolchain
# for the architecture ARCH names, i686-linux-gnu or x86_64-rach2-linux-gnu
# (vendor/gnu/arch.sh says why), under build/vendor/gnu/ or
# build/x86_64/vendor/gnu/ from the tarballs vendor/gnu/fetch.sh downloads and
# checks, and `make gnu-ports` builds every recipe of vendor/gnu/ports/ with
# it into an empty root/ there. The scripts take the architecture from
# RACH2_ARCH (vendor/gnu/arch.sh). Each is behind a stamp. The toolchain's
# depends on its script, fetch.sh and the toolchain's own lines of
# sources.txt, so that pinning a port does not rebuild the compilers; the
# ports' depends on the toolchain, build_port.sh, the recipes, fetch.sh, all
# of sources.txt and the link GNU_LINK asks for, shared unless it says static,
# which a file keeps.
GNU_OUT = build$(ARCH_SUFFIX)/vendor/gnu
GNU_TARGET_i686 = i686-linux-gnu
GNU_TARGET_x86_64 = x86_64-rach2-linux-gnu
GNU_TARGET = $(GNU_TARGET_$(ARCH))
GNU_TOOLCHAIN_STAMP = $(GNU_OUT)/toolchain.stamp
GNU_TOOLCHAIN_PINS = $(GNU_OUT)/toolchain.pins
GNU_PORTS_STAMP = $(GNU_OUT)/ports.stamp
GNU_PORTS_LINK = $(GNU_OUT)/ports.link
GNU_CC = $(GNU_OUT)/cross/bin/$(GNU_TARGET)-gcc
GNU_RECIPES = $(wildcard vendor/gnu/ports/*.sh)

.PHONY: gnu-toolchain gnu-ports
gnu-toolchain: $(GNU_TOOLCHAIN_STAMP)
gnu-ports: $(GNU_PORTS_STAMP)

# Rewritten only when the toolchain's lines change, so that its timestamp
# moves only then.
$(GNU_TOOLCHAIN_PINS): vendor/gnu/sources.txt
	@mkdir -p $(GNU_OUT)
	@awk '$$1 ~ /^(binutils|gcc|gmp|mpfr|mpc|glibc|linux|libxcrypt)$$/' vendor/gnu/sources.txt >$@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi

$(GNU_TOOLCHAIN_STAMP): vendor/gnu/build_toolchain.sh vendor/gnu/arch.sh vendor/gnu/fetch.sh $(GNU_TOOLCHAIN_PINS) | $(SHA256TOOL)
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else RACH2_ARCH=$(ARCH) ./vendor/gnu/build_toolchain.sh; fi
	@test -x $(GNU_CC) || { echo "gnu: toolchain: the build produced no $(GNU_CC)" >&2; exit 1; }
	@touch $@

# Rewritten only when GNU_LINK changes, so that the ports are built again
# only then; the phony prerequisite runs the comparison at every make.
.PHONY: gnu-link-check
$(GNU_PORTS_LINK): gnu-link-check
	@mkdir -p $(GNU_OUT)
	@echo "$(or $(GNU_LINK),shared)" >$@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi

$(GNU_PORTS_STAMP): $(GNU_TOOLCHAIN_STAMP) vendor/gnu/build_port.sh vendor/gnu/fetch.sh vendor/gnu/sources.txt $(GNU_RECIPES) $(GNU_PORTS_LINK)
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else RACH2_ARCH=$(ARCH) ./vendor/gnu/build_port.sh --all; fi
	@touch $@
