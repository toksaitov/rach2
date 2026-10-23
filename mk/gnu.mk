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
#
# Each step runs through `etude build` with the layer's tree as its root
# (`-r`): with the host's store it is built in the store, at the path of its
# entry, which the script takes as RACH2_GNU_OUT, and the tree has links to
# its outputs, since the compilers keep the paths they were built at; without
# the store it is built in the tree. A step names the outputs of the steps
# before it that it reads (`-u`), and the store's entry has links to them.
# The key counts the host's compilers, which build the cross toolchain and
# the ports' own tools; ccache, where the store has it, also wraps the
# target's compilers, which the toolchain's key already counts.
GNU_OUT = build$(ARCH_SUFFIX)/vendor/gnu
GNU_TARGET_i686 = i686-linux-gnu
GNU_TARGET_x86_64 = x86_64-rach2-linux-gnu
GNU_TARGET = $(GNU_TARGET_$(ARCH))
GNU_TOOLCHAIN_STAMP = $(GNU_OUT)/toolchain.stamp
GNU_TOOLCHAIN_PINS = $(GNU_OUT)/toolchain.pins
GNU_PORTS_STAMP = $(GNU_OUT)/ports.stamp
GNU_PORTS_LINK = $(GNU_OUT)/ports.link
GNU_CC = $(GNU_OUT)/cross/bin/$(GNU_TARGET)-gcc
GNU_STEP = $(STEP) -r $(GNU_OUT) -c cc -c c++ -w $(GNU_TARGET)-gcc -w $(GNU_TARGET)-g++
GNU_STEP_STAMPS = $(GNU_TOOLCHAIN_STAMP) $(GNU_PORTS_STAMP)
GNU_RECIPES = $(wildcard vendor/gnu/ports/*.sh)
# The sources of each stamp, which mk/gnu-image.mk also reads.
GNU_TOOLCHAIN_SOURCES = vendor/gnu/build_toolchain.sh vendor/gnu/arch.sh vendor/gnu/fetch.sh
GNU_PORTS_SOURCES = vendor/gnu/build_port.sh vendor/gnu/fetch.sh vendor/gnu/sources.txt $(GNU_RECIPES)

.PHONY: gnu-toolchain gnu-ports
gnu-toolchain: $(GNU_TOOLCHAIN_STAMP)
gnu-ports: $(GNU_PORTS_STAMP)

# Rewritten only when the toolchain's lines change, so that its timestamp
# moves only then. This rule, the ports.link rule below and the agents' lists
# (mk/gnu-agents.mk) write a temporary named by the shell's process ID, since
# two suites' makes may run one of them at once.
$(GNU_TOOLCHAIN_PINS): vendor/gnu/sources.txt
	@mkdir -p $(GNU_OUT)
	@t=$@.$$$$; awk '$$1 ~ /^(binutils|gcc|gmp|mpfr|mpc|glibc|linux|libxcrypt)$$/' vendor/gnu/sources.txt >$$t && \
	    if cmp -s $$t $@; then rm -f $$t; else mv $$t $@; fi

# With the store, a layer built in this copy, whose cross/ is a directory
# here and not a link into the store, is out of date: the phony gnu-in-copy
# makes the toolchain's stamp run again when something uses it, and its rule
# first removes the layer's directories and stamps, so that each step takes
# its outputs from the store, or builds them there. A copy that uses no GNU
# target keeps its layer, as a ci runner keeps the one a full run builds.
GNU_IN_COPY := $(if $(RACH2_STORE),$(if $(DRY_RUN),,$(shell [ -d $(GNU_OUT)/cross ] && \
                   [ ! -L $(GNU_OUT)/cross ] && echo yes)))
.PHONY: gnu-in-copy
ifneq ($(GNU_IN_COPY),)
$(GNU_TOOLCHAIN_STAMP): gnu-in-copy
endif

$(GNU_TOOLCHAIN_STAMP): $(GNU_TOOLCHAIN_SOURCES) $(GNU_TOOLCHAIN_PINS) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	@if [ -n "$(GNU_IN_COPY)" ]; then \
	    echo "gnu: removing the layer built in $(GNU_OUT), which the store replaces"; \
	    find $(GNU_OUT) -mindepth 1 -maxdepth 1 \( -type d -o -name '*.stamp' \) \
	        -exec chmod -R u+w {} + -exec rm -rf {} +; fi
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else \
	    $(GNU_STEP) -n gnu-toolchain -t $@ $(STEP_INPUTS) -o cross -o sysroot \
	        -- sh -c 'RACH2_ARCH=$(ARCH) RACH2_GNU_OUT="$$ETUDE_STEP_ROOT" ./vendor/gnu/build_toolchain.sh && \
	            test -x "$$ETUDE_STEP_ROOT/cross/bin/$(GNU_TARGET)-gcc"'; fi
	@test -x $(GNU_CC) || { echo "gnu: toolchain: the build produced no $(GNU_CC)" >&2; exit 1; }

# Rewritten only when GNU_LINK changes, so that the ports are built again
# only then; the phony prerequisite runs the comparison at every make.
.PHONY: gnu-link-check
$(GNU_PORTS_LINK): gnu-link-check
	@mkdir -p $(GNU_OUT)
	@t=$@.$$$$; echo "$(or $(GNU_LINK),shared)" >$$t && \
	    if cmp -s $$t $@; then rm -f $$t; else mv $$t $@; fi

$(GNU_PORTS_STAMP): $(GNU_TOOLCHAIN_STAMP) $(GNU_PORTS_SOURCES) $(GNU_PORTS_LINK) | $(ETUDE)
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else \
	    $(GNU_STEP) -n gnu-ports -t $@ $(STEP_INPUTS) -u cross -u sysroot \
	        -o root -o stage -o setid -o host -o wrap \
	        -- sh -c 'RACH2_ARCH=$(ARCH) RACH2_GNU_OUT="$$ETUDE_STEP_ROOT" ./vendor/gnu/build_port.sh --all'; fi
