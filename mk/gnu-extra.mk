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
GNU_VENUS_STAMP = $(GNU_OUT)/venus.stamp
GNU_STEP_STAMPS += $(GNU_DEBIAN_STAMP) $(GNU_PROTON_STAMP) $(GNU_WEBGL_STAMP) $(GNU_VENUS_STAMP)
GNU_DEBIAN_LISTS = debian/mesa.list debian/desktop.list debian/browsers.list debian/sound.list debian/tools.list
GNU_DEBIAN_SOURCES = vendor/gnu/debian/fetch_debs.sh vendor/gnu/debian/fetch_asset.sh \
                     vendor/gnu/debian/resolve_debs.py \
                     $(addprefix vendor/gnu/,$(GNU_DEBIAN_LISTS)) vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt
GNU_PROTON_SOURCES = vendor/gnu/proton/fetch_proton.sh vendor/gnu/proton/fetch_dinput.sh \
                     vendor/gnu/proton/prepare_dinput.py vendor/gnu/proton/dinput-shutdown.patch \
                     vendor/gnu/proton/sources.txt vendor/gnu/debian/fetch_asset.sh vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt
GNU_VENUS_SOURCES = vendor/gnu/debian/fetch_venus.sh vendor/gnu/debian/fetch_asset.sh vendor/gnu/debian/venus.list
GNU_WEBGL_SOURCES = vendor/gnu/debian/fetch_webgl.sh vendor/gnu/debian/fetch_asset.sh vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt

.PHONY: gnu-desktop gnu-proton gnu-webgl gnu-venus
gnu-desktop: $(GNU_DEBIAN_STAMP)
gnu-proton: $(GNU_PROTON_STAMP)
gnu-webgl: $(GNU_WEBGL_STAMP)
gnu-venus: $(GNU_VENUS_STAMP)

$(GNU_DEBIAN_STAMP): $(GNU_DEBIAN_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	@$(GNU_ASSET_STEP) -n gnu-desktop -t $@ $(STEP_INPUTS) \
	    -o debian/desktop -o work/debian/desktop/files -o work/debian/desktop/closure.txt \
	    -- sh -c 'RACH2_ARCH=$(ARCH) RACH2_GNU_OUT="$$ETUDE_STEP_ROOT" \
	        GNU_DEBIAN_LISTS="$(GNU_DEBIAN_LISTS)" ./vendor/gnu/debian/fetch_debs.sh desktop'

# Mesa 26.1.6's Venus driver alone, beside the desktop's Mesa 25.0.7.
$(GNU_VENUS_STAMP): $(GNU_VENUS_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	@$(GNU_ASSET_STEP) -n gnu-venus -t $@ $(STEP_INPUTS) -o venus \
	    -- $(call GNU_STEP_RUN,vendor/gnu/debian/fetch_venus.sh)

# The x86-64 root's i386 layer (vendor/gnu/Readme.md, The i386 layer), in one
# step: the closure of debian/i386.list, with each package's Multi-Arch
# field, and Mesa 26.1.6's i386 Venus driver in venus-i386/. games/i386.list
# adds the i386 SuperTuxKart without its data, which the games layer's amd64
# closure has at the same version. The scripts run as for the i686 root,
# whose Debian architecture is i386, and write the x86-64 tree. The i686 root
# has no such layer, since its own packages are i386's.
GNU_I386_STAMP = $(GNU_OUT)/i386.stamp
GNU_STEP_STAMPS += $(GNU_I386_STAMP)
GNU_I386_SOURCES = vendor/gnu/debian/fetch_debs.sh vendor/gnu/debian/fetch_venus.sh vendor/gnu/debian/fetch_asset.sh \
                   vendor/gnu/debian/resolve_debs.py vendor/gnu/debian/multiarch.sh vendor/gnu/debian/i386.list \
                   vendor/gnu/games/i386.list vendor/gnu/debian/venus.list vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt
GNU_I386_RUN = RACH2_ARCH=i686 RACH2_GNU_OUT="$$ETUDE_STEP_ROOT"

.PHONY: gnu-i386
gnu-i386: $(GNU_I386_STAMP)

$(GNU_I386_STAMP): $(GNU_I386_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@test "$(ARCH)" = x86_64 || { echo "gnu: i386: the i386 layer is the x86-64 root's; run make ARCH=x86_64 gnu-i386" >&2; exit 1; }
	@rm -f $@
	@$(GNU_ASSET_STEP) -n gnu-i386 -t $@ $(STEP_INPUTS) \
	    -o debian/i386 -o work/debian/i386/files -o work/debian/i386/closure.txt \
	    -o work/debian/i386/multiarch.txt -o venus-i386 \
	    -- sh -c '$(GNU_I386_RUN) GNU_DEBIAN_LISTS="debian/i386.list games/i386.list" \
	        GNU_DEBIAN_UNFOLLOWED=supertuxkart-data ./vendor/gnu/debian/fetch_debs.sh i386 && \
	        $(GNU_I386_RUN) ./vendor/gnu/debian/multiarch.sh i386 && \
	        $(GNU_I386_RUN) ./vendor/gnu/debian/fetch_venus.sh venus-i386'
	@test -L $(GNU_OUT)/debian/i386/usr/lib/ld-linux.so.2 || { echo "gnu: i386: the layer has no usr/lib/ld-linux.so.2" >&2; exit 1; }

$(GNU_PROTON_STAMP): $(GNU_PROTON_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	+@$(GNU_ASSET_STEP) -c clang -n gnu-proton -t $@ $(STEP_INPUTS) -o proton \
	    -- $(call GNU_STEP_RUN,vendor/gnu/proton/fetch_proton.sh)

$(GNU_WEBGL_STAMP): $(GNU_WEBGL_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	@$(GNU_ASSET_STEP) -n gnu-webgl -t $@ $(STEP_INPUTS) -o webgl \
	    -- $(call GNU_STEP_RUN,vendor/gnu/debian/fetch_webgl.sh)

# The games layer (vendor/gnu/games/Readme.md): a Debian closure of its own,
# from the same snapshot, which install_root.sh installs after the desktop's,
# and on x86-64 umu-launcher with the Steam Linux Runtime's archive in umu/.
GNU_GAMES_STAMP = $(GNU_OUT)/games.stamp
GNU_STEP_STAMPS += $(GNU_GAMES_STAMP)
GNU_GAMES_LISTS = games/layer.list games/native.list
GNU_GAMES_SOURCES = vendor/gnu/debian/fetch_debs.sh vendor/gnu/debian/fetch_asset.sh \
                    vendor/gnu/debian/resolve_debs.py \
                    $(addprefix vendor/gnu/,$(GNU_GAMES_LISTS)) vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt \
                    vendor/gnu/games/fetch_umu.sh vendor/gnu/games/umu.list

.PHONY: gnu-games
gnu-games: $(GNU_GAMES_STAMP)

$(GNU_GAMES_STAMP): $(GNU_GAMES_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	@$(GNU_ASSET_STEP) -n gnu-games -t $@ $(STEP_INPUTS) \
	    -o debian/games -o work/debian/games/files -o work/debian/games/closure.txt -o umu \
	    -- sh -c 'RACH2_ARCH=$(ARCH) RACH2_GNU_OUT="$$ETUDE_STEP_ROOT" \
	        GNU_DEBIAN_LISTS="$(GNU_GAMES_LISTS)" ./vendor/gnu/debian/fetch_debs.sh games && \
	        RACH2_ARCH=$(ARCH) RACH2_GNU_OUT="$$ETUDE_STEP_ROOT" ./vendor/gnu/games/fetch_umu.sh'

# The video layer (vendor/gnu/debian/Readme.md): libva, Intel's i965 VA
# driver and vainfo, a Debian closure of its own from the desktop's
# snapshot, which install_root.sh installs after the desktop's.
GNU_VA_STAMP = $(GNU_OUT)/va.stamp
GNU_STEP_STAMPS += $(GNU_VA_STAMP)
GNU_VA_LISTS = debian/va.list
GNU_VA_SOURCES = vendor/gnu/debian/fetch_debs.sh vendor/gnu/debian/fetch_asset.sh \
                 vendor/gnu/debian/resolve_debs.py \
                 $(addprefix vendor/gnu/,$(GNU_VA_LISTS)) vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt

.PHONY: gnu-va
gnu-va: $(GNU_VA_STAMP)

$(GNU_VA_STAMP): $(GNU_VA_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	@$(GNU_ASSET_STEP) -n gnu-va -t $@ $(STEP_INPUTS) \
	    -o debian/va -o work/debian/va/files -o work/debian/va/closure.txt \
	    -- sh -c 'RACH2_ARCH=$(ARCH) RACH2_GNU_OUT="$$ETUDE_STEP_ROOT" \
	        GNU_DEBIAN_LISTS="$(GNU_VA_LISTS)" ./vendor/gnu/debian/fetch_debs.sh va'

# The KVM layer (vendor/gnu/kvm/Readme.md), for x86-64 alone: Debian's QEMU,
# a closure of its own from the desktop's snapshot, and kvm-unit-tests and
# Linux's KVM selftests, built with the cross toolchain, in one step.
GNU_KVM_STAMP = $(GNU_OUT)/kvm.stamp
GNU_STEP_STAMPS += $(GNU_KVM_STAMP)
GNU_KVM_SOURCES = vendor/gnu/kvm/build_kvm.sh vendor/gnu/kvm/qemu.list vendor/gnu/kvm/sources.txt \
                  vendor/gnu/debian/fetch_debs.sh vendor/gnu/debian/fetch_asset.sh \
                  vendor/gnu/debian/resolve_debs.py vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt

.PHONY: gnu-kvm
gnu-kvm: $(GNU_KVM_STAMP)

$(GNU_KVM_STAMP): $(GNU_KVM_SOURCES) $(GNU_TOOLCHAIN_STAMP) | $(SHA256TOOL) $(ETUDE)
	@test "$(ARCH)" = x86_64 || { echo "gnu: kvm: the KVM layer is the x86-64 root's; run make ARCH=x86_64 gnu-kvm" >&2; exit 1; }
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else \
	    $(GNU_STEP) -n gnu-kvm -t $@ $(STEP_INPUTS) -u cross -u sysroot \
	        -o debian/kvm -o work/debian/kvm/files -o work/debian/kvm/closure.txt -o kvm \
	        -- $(call GNU_STEP_RUN,vendor/gnu/kvm/build_kvm.sh); fi
	@test -x $(GNU_OUT)/kvm/opt/kvm-unit-tests/run_tests.sh || { echo "gnu: kvm: the layer has no opt/kvm-unit-tests/run_tests.sh" >&2; exit 1; }

# IGT's tests, built from Debian's intel-gpu-tools source in the Debian
# builder guest (vendor/gnu/igt/build_igt.sh), for x86-64 alone. The guest is
# an x86-64 QEMU machine, so the step runs where the layer builds on an
# x86-64 Linux host, and mk/gnu-image.mk's rule for that host alone stages
# it; a root made in the tooling machine has no IGT. The builder's files are
# inputs, so that a changed guest builds the tests again.
GNU_IGT_STAMP = $(GNU_OUT)/igt.stamp
GNU_STEP_STAMPS += $(GNU_IGT_STAMP)
GNU_IGT_SOURCES = vendor/gnu/igt/build_igt.sh vendor/gnu/igt/sources.txt vendor/gnu/debian/fetch_asset.sh \
                  vendor/gnu/debian/venus.list \
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

# Weston 14.0.2 rebuilt from Debian's source package with Rach2's patches in
# the Debian builder guest (vendor/gnu/games/weston/build_weston.sh), for
# x86-64 alone. Like IGT, it builds where the layer builds on an x86-64 Linux
# host, and mk/gnu-image.mk's rule for that host alone stages it in place of
# the desktop closure's Weston; a root made in the tooling machine keeps
# Debian's. The builder's files are inputs, as IGT's are.
GNU_WESTON_STAMP = $(GNU_OUT)/weston.stamp
GNU_STEP_STAMPS += $(GNU_WESTON_STAMP)
GNU_WESTON_SOURCES = $(addprefix vendor/gnu/games/weston/,build_weston.sh sources.txt series debs.sha256 \
                         kiosk-active-tree.patch kiosk-fullscreen-centre.patch xwm-emulated-mode.patch \
                         xwm-mapped-managed.patch pointer-constraint-view.patch version.patch) \
                     vendor/gnu/debian/fetch_asset.sh \
                     $(addprefix tests/graphics/debian-ref/,debian-ref.sh image packages setup.sh.in \
                         user-data.in meta-data network-config) \
                     tests/graphics/guest-env.sh rach2/qemu_devices.sh

.PHONY: gnu-weston
gnu-weston: $(GNU_WESTON_STAMP)

$(GNU_WESTON_STAMP): $(GNU_WESTON_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	@$(GNU_ASSET_STEP) -n gnu-weston -t $@ $(STEP_INPUTS) -o weston -o work/weston \
	    -- $(call GNU_STEP_RUN,vendor/gnu/games/weston/build_weston.sh)
	@test -x $(GNU_OUT)/weston/usr/bin/weston || { echo "gnu: weston: the stage has no usr/bin/weston" >&2; exit 1; }

# VK-GL-CTS 1.4.1.1 and 1.4.7.1, deqp-vk and glcts for the surfaceless EGL
# target, built in the same builder guest as IGT's tests
# (vendor/gnu/vk-gl-cts/build_vk_gl_cts.sh), for x86-64 alone and only where
# that guest runs.
GNU_VK_GL_CTS_STAMP = $(GNU_OUT)/vk-gl-cts.stamp
GNU_STEP_STAMPS += $(GNU_VK_GL_CTS_STAMP)
GNU_VK_GL_CTS_SOURCES = vendor/gnu/vk-gl-cts/build_vk_gl_cts.sh vendor/gnu/vk-gl-cts/sources.txt \
                        vendor/gnu/debian/fetch_asset.sh vendor/gnu/debian/venus.list \
                        $(addprefix tests/graphics/debian-ref/,debian-ref.sh image packages setup.sh.in \
                            user-data.in meta-data network-config) \
                        tests/graphics/guest-env.sh rach2/qemu_devices.sh

.PHONY: gnu-vk-gl-cts
gnu-vk-gl-cts: $(GNU_VK_GL_CTS_STAMP)

$(GNU_VK_GL_CTS_STAMP): $(GNU_VK_GL_CTS_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	@$(GNU_ASSET_STEP) -n gnu-vk-gl-cts -t $@ $(STEP_INPUTS) -o vk-gl-cts -o work/vk-gl-cts \
	    -- $(call GNU_STEP_RUN,vendor/gnu/vk-gl-cts/build_vk_gl_cts.sh)
	@test -x $(GNU_OUT)/vk-gl-cts/opt/vk-gl-cts-1.4.7.1/vulkan/deqp-vk || { echo "gnu: vk-gl-cts: the root has no opt/vk-gl-cts-1.4.7.1/vulkan/deqp-vk" >&2; exit 1; }
