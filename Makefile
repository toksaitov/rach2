# Rach2 root Makefile.
#
# Quick start (run `make doctor` first to check your toolchain):
#   make            build the BASE teaching OS: libc, kernel, host tools, tests,
#                   utils, and the RAM disk -- the everyday build, kept fast.
#   make full       + the capstones: busybox, dropbear, and the Doom port.
#   make deps       build the vendored third-party (libc; DEPS=full adds bb+dropbear)
#   make doctor     check the toolchain (PASS/MISSING with install hints)
#   make verify-style / verify-host   the quality gates
#   make clean      remove component build products (objects, .d, binaries, ramdisk)
#   make distclean  + the vendored libc/busybox/dropbear builds (full from-scratch)
#   make help       list every target
# Run the built kernel with the scripts in rach2/ (run_with_ram_disk.sh, run.sh) --
# see `make help`. The build system does not run QEMU; those scripts model the
# disk/audio/network/-cpu option matrix far better than a `make run` could.

# --- components (each builds via its own sub-make) ---
BASE_DIRS = krffs rach2 tests ish utils
FULL_DIRS = ports/doom

# --- vendored third-party: built by their own scripts (which wrap musl's autotools
#     and busybox/dropbear's kbuild), tracked with stamp files so a clean clone
#     "just works" without running the build_*.sh by hand, and each rebuilds only
#     when its real inputs (tarball, build script, config) change. ---
LIBC_STAMP     = libc/.stamp
BUSYBOX_STAMP  = busybox/.stamp
DROPBEAR_STAMP = dropbear/.stamp

# The pinned vendored source tarballs -- the exact names the build_*.sh scripts
# verify by sha256. Used as stamp prerequisites (not a $(wildcard), which would
# silently drop the prereq and look up-to-date if a tarball were deleted) and to
# derive the extracted source dirs that `distclean` removes.
LIBC_TARBALL     = libc/musl-1.2.5.tar.gz
BUSYBOX_TARBALL  = busybox/busybox-1.36.1.tar.bz2
DROPBEAR_TARBALL = dropbear/dropbear-2022.83.tar.bz2

.PHONY: all full deps clean distclean ramdisk verify-style verify-host doctor help \
        busybox dropbear $(BASE_DIRS) $(FULL_DIRS)

# Default: the base teaching OS.
all: ramdisk

# Everything, including the slow capstones (the bootable capstone image is wired in
# the next phase; today `full` builds the binaries + deps).
full: all $(BUSYBOX_STAMP) $(DROPBEAR_STAMP) ports/doom

# Build the vendored third-party. libc is built by default (utils link it); busybox
# and dropbear are added with DEPS=full (they are slow capstones).
deps: $(LIBC_STAMP)
ifeq ($(DEPS),full)
deps: $(BUSYBOX_STAMP) $(DROPBEAR_STAMP)
endif

# Friendly aliases for the third-party stamps.
busybox: $(BUSYBOX_STAMP)
dropbear: $(DROPBEAR_STAMP)

# --- third-party stamp rules. Each removes BOTH the stamp and the expected artifact
#     up front, runs its build script, requires a non-empty artifact, then touches
#     the stamp. Removing the artifact first means a script that exits 0 without
#     restaging cannot pass the assertion on a STALE artifact; removing the stamp
#     first means any failure (build or assertion) leaves no stamp, so the next make
#     retries instead of trusting a missing/old artifact. .DELETE_ON_ERROR is the
#     belt-and-suspenders backstop on the final touch. ---
.DELETE_ON_ERROR:

$(LIBC_STAMP): libc/build_libc.sh $(LIBC_TARBALL)
	@rm -f $@ libc/install/lib/libc.a
	./libc/build_libc.sh
	@test -s libc/install/lib/libc.a || { echo "libc: build produced no libc.a" >&2; exit 1; }
	@touch $@

$(BUSYBOX_STAMP): busybox/build_busybox.sh busybox/rach2.config busybox/muslgcc \
                  $(BUSYBOX_TARBALL) $(LIBC_STAMP)
	@rm -f $@ tests/busybox
	./busybox/build_busybox.sh
	@test -s tests/busybox || { echo "busybox: build produced no tests/busybox" >&2; exit 1; }
	@touch $@

$(DROPBEAR_STAMP): dropbear/build_dropbear.sh dropbear/localoptions.h dropbear/muslgcc \
                   $(DROPBEAR_TARBALL) $(LIBC_STAMP)
	@rm -f $@ tests/dropbearmulti
	./dropbear/build_dropbear.sh
	@test -s tests/dropbearmulti || { echo "dropbear: build produced no tests/dropbearmulti" >&2; exit 1; }
	@touch $@

# --- component sub-makes + the build-order DAG ---
$(BASE_DIRS) $(FULL_DIRS):
	$(MAKE) -C $@

rach2: krffs
tests: rach2
ish: rach2
utils: rach2 $(LIBC_STAMP)
ports/doom: rach2

# Base RAM disk. Force RAMDISK_WITH_DOOM=0 so the Make-driven build is always
# capstone-free: a stray `RAMDISK_WITH_DOOM=1 make -j full` can then never make the
# staging script copy ../ports/doom/doom while the sibling ports/doom target is
# rebuilding it. (The env var is honored only for a standalone `./build_ramdisk.sh`;
# the build/stage split + a Make-wired Doom capstone image are the next phase.)
ramdisk: $(BASE_DIRS)
	cd krffs && RAMDISK_WITH_DOOM=0 ./build_ramdisk.sh

# --- toolchain preflight + quality gates ---
doctor:
	@./tools/doctor.sh

# Lightweight quality gate (see tools/check.sh): -Werror build, scoped cppcheck,
# shellcheck, the convention grep, clang-format, and a readelf ABI check.
verify-style:
	@./tools/check.sh

# Dual-target seam tests (see tools/hosttest.sh): pure kernel logic compiled and
# run natively under AddressSanitizer + UndefinedBehaviorSanitizer.
verify-host:
	@./tools/hosttest.sh

# clean: the everyday reset -- component build products (each sub-make removes its
# own objects/.d/binaries) plus the generated RAM-disk image. The expensive vendored
# third-party (libc/install, the busybox/dropbear builds) is LEFT in place so a
# routine clean does not force a multi-minute musl rebuild; use distclean for that.
clean:
	@for dir in $(BASE_DIRS) $(FULL_DIRS); do $(MAKE) -C $$dir clean; done
	rm -f krffs/file_system.krffs

# distclean: a full from-scratch reset -- clean plus every third-party build product:
# the stamps, libc/install, the extracted vendored source trees, and the staged
# capstone binaries. The next build rebuilds libc/busybox/dropbear from the tarballs.
distclean: clean
	rm -f $(LIBC_STAMP) $(BUSYBOX_STAMP) $(DROPBEAR_STAMP)
	rm -rf libc/install \
	       $(LIBC_TARBALL:.tar.gz=) $(BUSYBOX_TARBALL:.tar.bz2=) $(DROPBEAR_TARBALL:.tar.bz2=)
	rm -f tests/busybox tests/dropbearmulti

help:
	@echo "Rach2 build targets:"
	@echo "  make            base teaching OS (libc, kernel, host tools, tests, utils, ramdisk)"
	@echo "  make full       + capstones (busybox, dropbear, Doom)"
	@echo "  make deps       vendored third-party (libc; DEPS=full adds busybox+dropbear)"
	@echo "  make busybox / make dropbear   one capstone's third-party build"
	@echo "  make ramdisk    rebuild the RAM disk image"
	@echo "  make doctor     check the toolchain (PASS/MISSING with install hints)"
	@echo "  make verify-style / verify-host   quality gates"
	@echo "  make clean      remove component build products (objects, .d, binaries, ramdisk)"
	@echo "  make distclean  + the vendored libc/busybox/dropbear builds (full from-scratch)"
	@echo ""
	@echo "Run the kernel (the build system does not run QEMU):"
	@echo "  cd rach2 && ./run_with_ram_disk.sh        # boot with the RAM disk"
	@echo "  NET_HOSTFWD=hostfwd=tcp::2222-:22 ./run_with_ram_disk.sh   # + SSH forward"
