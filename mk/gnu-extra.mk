# The programs of the GNU layer that have build scripts of their own rather
# than port recipes (vendor/gnu/native, spidermonkey and chromium). Like
# gnu-toolchain, each target builds on a Linux host, is not part of `all`,
# and runs its script from nothing behind a stamp, again whenever the script
# or a list of sources it reads changes: `make gnu-native` builds GCC and
# binutils to run on Rach2, `make gnu-gdb` GDB and gdbserver, `make gnu-js`
# the SpiderMonkey shell, and `make gnu-chromium` unpacks Debian's
# chromium-headless-shell with its libraries. The three that compile are
# rebuilt when the cross toolchain is; the fourth needs only its sysroot.
GNU_NATIVE_STAMP = $(GNU_OUT)/native.stamp
GNU_GDB_STAMP = $(GNU_OUT)/gdb.stamp
GNU_JS_STAMP = $(GNU_OUT)/js.stamp
GNU_CHROMIUM_STAMP = $(GNU_OUT)/chromium.stamp

.PHONY: gnu-native gnu-gdb gnu-js gnu-chromium
gnu-native: $(GNU_NATIVE_STAMP)
gnu-gdb: $(GNU_GDB_STAMP)
gnu-js: $(GNU_JS_STAMP)
gnu-chromium: $(GNU_CHROMIUM_STAMP)

$(GNU_NATIVE_STAMP): vendor/gnu/native/build_native.sh vendor/gnu/fetch.sh vendor/gnu/sources.txt $(GNU_TOOLCHAIN_STAMP)
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else RACH2_ARCH=$(ARCH) ./vendor/gnu/native/build_native.sh; fi
	@test -x $(GNU_OUT)/native/usr/bin/gcc || { echo "gnu: native: the build produced no $(GNU_OUT)/native/usr/bin/gcc" >&2; exit 1; }
	@touch $@

$(GNU_GDB_STAMP): vendor/gnu/native/build_gdb.sh vendor/gnu/fetch.sh vendor/gnu/sources.txt vendor/gnu/sources-extra.txt $(GNU_TOOLCHAIN_STAMP)
	@rm -f $@
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else RACH2_ARCH=$(ARCH) ./vendor/gnu/native/build_gdb.sh; fi
	@test -x $(GNU_OUT)/gdb/usr/bin/gdb || { echo "gnu: gdb: the build produced no $(GNU_OUT)/gdb/usr/bin/gdb" >&2; exit 1; }
	@touch $@

$(GNU_JS_STAMP): vendor/gnu/spidermonkey/build_js.sh vendor/gnu/spidermonkey/python-3.14.patch vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt $(GNU_TOOLCHAIN_STAMP)
	@rm -f $@
	@RACH2_ARCH=$(ARCH) ./vendor/gnu/spidermonkey/build_js.sh
	@test -x $(GNU_OUT)/js/usr/bin/js || { echo "gnu: js: the build produced no $(GNU_OUT)/js/usr/bin/js" >&2; exit 1; }
	@touch $@

$(GNU_CHROMIUM_STAMP): vendor/gnu/chromium/fetch_chromium.sh vendor/gnu/fetch.sh vendor/gnu/sources-extra.txt | $(GNU_TOOLCHAIN_STAMP)
	@rm -f $@
	@RACH2_ARCH=$(ARCH) ./vendor/gnu/chromium/fetch_chromium.sh
	@test -x $(GNU_OUT)/chromium/usr/lib/chromium/chromium-headless-shell || { echo "gnu: chromium: the root has no usr/lib/chromium/chromium-headless-shell" >&2; exit 1; }
	@touch $@
