# mk/gnu-extra.mk: the programs of the GNU layer that have build scripts of
# their own rather than port recipes (vendor/gnu/native, spidermonkey and
# chromium). Like gnu-toolchain, each target builds on a Linux host, is not
# part of `all`, and runs its script from nothing behind a stamp, again
# whenever the script or a list of sources it reads changes: `make gnu-native`
# builds GCC and binutils to run on Rach2, `make gnu-gdb` GDB and gdbserver,
# `make gnu-js` the SpiderMonkey shell, and `make gnu-chromium` unpacks
# Debian's chromium-headless-shell with its libraries. The three that compile
# are rebuilt when the cross toolchain is; the fourth needs only its sysroot.
# Each runs through `etude build` as the steps of mk/gnu.mk do. Chromium's
# outputs are its root and the lists of its packages' files under
# work/chromium, which vendor/gnu/install_root.sh reads.
GNU_NATIVE_STAMP = $(GNU_OUT)/native.stamp
GNU_GDB_STAMP = $(GNU_OUT)/gdb.stamp
GNU_JS_STAMP = $(GNU_OUT)/js.stamp
GNU_CHROMIUM_STAMP = $(GNU_OUT)/chromium.stamp
GNU_STEP_STAMPS += $(GNU_NATIVE_STAMP) $(GNU_GDB_STAMP) $(GNU_JS_STAMP) $(GNU_CHROMIUM_STAMP)
# The command of a step: the script, with the root `etude build` gives it.
GNU_STEP_RUN = sh -c 'RACH2_ARCH=$(ARCH) RACH2_GNU_OUT="$$ETUDE_STEP_ROOT" ./$(1)'
GNU_NATIVE_SOURCES = vendor/gnu/native/build_native.sh vendor/gnu/fetch.sh vendor/gnu/sources.txt
GNU_GDB_SOURCES = vendor/gnu/native/build_gdb.sh vendor/gnu/fetch.sh vendor/gnu/sources.txt \
                  vendor/gnu/sources-extra.txt
GNU_JS_SOURCES = vendor/gnu/spidermonkey/build_js.sh vendor/gnu/spidermonkey/python-3.14.patch \
                 vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt
GNU_CHROMIUM_SOURCES = vendor/gnu/chromium/fetch_chromium.sh vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt

.PHONY: gnu-native gnu-gdb gnu-js gnu-chromium
gnu-native: $(GNU_NATIVE_STAMP)
gnu-gdb: $(GNU_GDB_STAMP)
gnu-js: $(GNU_JS_STAMP)
gnu-chromium: $(GNU_CHROMIUM_STAMP)

$(GNU_NATIVE_STAMP): $(GNU_NATIVE_SOURCES) $(GNU_TOOLCHAIN_STAMP) | $(ETUDE)
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else \
	    $(GNU_STEP) -n gnu-native -t $@ $(STEP_INPUTS) -u cross -u sysroot -o native \
	        -- $(call GNU_STEP_RUN,vendor/gnu/native/build_native.sh); fi
	@test -x $(GNU_OUT)/native/usr/bin/gcc || { echo "gnu: native: the build produced no $(GNU_OUT)/native/usr/bin/gcc" >&2; exit 1; }

$(GNU_GDB_STAMP): $(GNU_GDB_SOURCES) $(GNU_TOOLCHAIN_STAMP) | $(ETUDE)
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else \
	    $(GNU_STEP) -n gnu-gdb -t $@ $(STEP_INPUTS) -u cross -u sysroot -o gdb \
	        -- $(call GNU_STEP_RUN,vendor/gnu/native/build_gdb.sh); fi
	@test -x $(GNU_OUT)/gdb/usr/bin/gdb || { echo "gnu: gdb: the build produced no $(GNU_OUT)/gdb/usr/bin/gdb" >&2; exit 1; }

$(GNU_JS_STAMP): $(GNU_JS_SOURCES) $(GNU_TOOLCHAIN_STAMP) | $(ETUDE)
	@rm -f $@
	@$(GNU_STEP) -n gnu-js -t $@ $(STEP_INPUTS) -u cross -u sysroot -o js \
	    -- $(call GNU_STEP_RUN,vendor/gnu/spidermonkey/build_js.sh)
	@test -x $(GNU_OUT)/js/usr/bin/js || { echo "gnu: js: the build produced no $(GNU_OUT)/js/usr/bin/js" >&2; exit 1; }

$(GNU_CHROMIUM_STAMP): $(GNU_CHROMIUM_SOURCES) | $(GNU_TOOLCHAIN_STAMP) $(ETUDE)
	@rm -f $@
	@$(GNU_STEP) -n gnu-chromium -t $@ $(STEP_INPUTS) -u cross -u sysroot \
	    -o chromium -o work/chromium/files -o work/chromium/closure.txt \
	    -- $(call GNU_STEP_RUN,vendor/gnu/chromium/fetch_chromium.sh)
	@test -x $(GNU_OUT)/chromium/usr/lib/chromium/chromium-headless-shell || { echo "gnu: chromium: the root has no usr/lib/chromium/chromium-headless-shell" >&2; exit 1; }

# The desktop is one Debian closure, including chromium-headless-shell, so
# shared libraries are installed once and all clients use the same Mesa.
GNU_ASSET_STEP = $(STEP) -r $(GNU_OUT) -c cc
GNU_DEBIAN_STAMP = $(GNU_OUT)/debian.stamp
GNU_PROTON_STAMP = $(GNU_OUT)/proton.stamp
GNU_WEBGL_STAMP = $(GNU_OUT)/webgl.stamp
GNU_STEP_STAMPS += $(GNU_DEBIAN_STAMP) $(GNU_PROTON_STAMP) $(GNU_WEBGL_STAMP)
GNU_DEBIAN_LISTS = debian/mesa.list debian/desktop.list debian/browsers.list debian/sound.list debian/tools.list
GNU_DEBIAN_SOURCES = vendor/gnu/debian/fetch_debs.sh vendor/gnu/debian/fetch_asset.sh \
                     vendor/gnu/debian/resolve_debs.py \
                     $(addprefix vendor/gnu/,$(GNU_DEBIAN_LISTS)) vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt
GNU_PROTON_SOURCES = vendor/gnu/proton/fetch_proton.sh vendor/gnu/debian/fetch_asset.sh vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt
GNU_WEBGL_SOURCES = vendor/gnu/debian/fetch_webgl.sh vendor/gnu/debian/fetch_asset.sh vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt

.PHONY: gnu-desktop gnu-proton gnu-webgl
gnu-desktop: $(GNU_DEBIAN_STAMP)
gnu-proton: $(GNU_PROTON_STAMP)
gnu-webgl: $(GNU_WEBGL_STAMP)

$(GNU_DEBIAN_STAMP): $(GNU_DEBIAN_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	@$(GNU_ASSET_STEP) -n gnu-desktop -t $@ $(STEP_INPUTS) \
	    -o debian/desktop -o work/debian/desktop/files -o work/debian/desktop/closure.txt \
	    -- sh -c 'RACH2_ARCH=$(ARCH) RACH2_GNU_OUT="$$ETUDE_STEP_ROOT" \
	        GNU_DEBIAN_LISTS="$(GNU_DEBIAN_LISTS)" ./vendor/gnu/debian/fetch_debs.sh desktop'

$(GNU_PROTON_STAMP): $(GNU_PROTON_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	@$(GNU_ASSET_STEP) -n gnu-proton -t $@ $(STEP_INPUTS) -o proton \
	    -- $(call GNU_STEP_RUN,vendor/gnu/proton/fetch_proton.sh)

$(GNU_WEBGL_STAMP): $(GNU_WEBGL_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	@$(GNU_ASSET_STEP) -n gnu-webgl -t $@ $(STEP_INPUTS) -o webgl \
	    -- $(call GNU_STEP_RUN,vendor/gnu/debian/fetch_webgl.sh)

# IGT's tests, built from Debian's intel-gpu-tools source in the Debian
# builder guest (vendor/gnu/igt/build_igt.sh), for x86-64 alone. The guest is
# an x86-64 QEMU machine, so the step runs where the layer builds on an
# x86-64 Linux host, and mk/gnu-image.mk's rule for that host alone stages
# it; a root made in the tooling machine has no IGT. The builder's files are
# inputs, so that a changed guest builds the tests again.
GNU_IGT_STAMP = $(GNU_OUT)/igt.stamp
GNU_STEP_STAMPS += $(GNU_IGT_STAMP)
GNU_IGT_SOURCES = vendor/gnu/igt/build_igt.sh vendor/gnu/igt/sources.txt vendor/gnu/debian/fetch_asset.sh \
                  $(addprefix tests/graphics/debian-ref/,debian-ref.sh image packages setup.sh.in \
                      user-data.in meta-data network-config) \
                  tests/graphics/guest-env.sh rach2/qemu_devices.sh

.PHONY: gnu-igt
gnu-igt: $(GNU_IGT_STAMP)

$(GNU_IGT_STAMP): $(GNU_IGT_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	@$(GNU_ASSET_STEP) -n gnu-igt -t $@ $(STEP_INPUTS) -o igt -o work/igt \
	    -- $(call GNU_STEP_RUN,vendor/gnu/igt/build_igt.sh)
	@test -x $(GNU_OUT)/igt/usr/bin/igt_runner || { echo "gnu: igt: the root has no usr/bin/igt_runner" >&2; exit 1; }
