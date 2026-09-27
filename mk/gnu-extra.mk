# The programs of the GNU layer that have build scripts of their own rather
# than port recipes. Like gnu-toolchain, each target builds on a Linux host,
# is not part of `all`, and runs its script from nothing behind a stamp,
# again whenever the script or a list of sources it reads changes, and again
# when the cross toolchain is rebuilt: `make gnu-native` builds GCC and
# binutils to run on Rach2, and `make gnu-gdb` GDB and gdbserver
# (vendor/gnu/native).
GNU_NATIVE_STAMP = $(GNU_OUT)/native.stamp
GNU_GDB_STAMP = $(GNU_OUT)/gdb.stamp

.PHONY: gnu-native gnu-gdb
gnu-native: $(GNU_NATIVE_STAMP)
gnu-gdb: $(GNU_GDB_STAMP)

$(GNU_NATIVE_STAMP): vendor/gnu/native/build_native.sh vendor/gnu/fetch.sh vendor/gnu/sources.txt $(GNU_TOOLCHAIN_STAMP)
	@rm -f $@
	+@./vendor/gnu/native/build_native.sh
	@test -x $(GNU_OUT)/native/usr/bin/gcc || { echo "gnu: native: the build produced no $(GNU_OUT)/native/usr/bin/gcc" >&2; exit 1; }
	@touch $@

$(GNU_GDB_STAMP): vendor/gnu/native/build_gdb.sh vendor/gnu/fetch.sh vendor/gnu/sources.txt vendor/gnu/sources-extra.txt $(GNU_TOOLCHAIN_STAMP)
	@rm -f $@
	+@./vendor/gnu/native/build_gdb.sh
	@test -x $(GNU_OUT)/gdb/usr/bin/gdb || { echo "gnu: gdb: the build produced no $(GNU_OUT)/gdb/usr/bin/gdb" >&2; exit 1; }
	@touch $@
