# Rach2 root Makefile.
#
# The build produces purpose-built RAM-disk images, each its own file under
# build/images/ so they coexist: `test` (the automated end-to-end test binaries),
# `os-simple` (our own shell and simple Unix tools, for teaching), and `os-real`
# (a real Unix-like environment: busybox, dropbear, Doom).
#
# Quick start (run `make doctor` first to check your toolchain):
#   make            build the kernel + the os-simple image: the everyday teaching
#                   build, kept fast.
#   make full       all the RAM-disk images (test and os-simple).
#   make images     the RAM-disk images.
#   make test       boot the test image and check every probe passes.
#   make deps       build the vendored third-party (libc; DEPS=full adds bb+dropbear)
#   make doctor     check the toolchain (PASS/MISSING with install hints)
#   make verify-style / verify-host   the quality gates
#   make clean      remove the in-house build products and the images
#   make distclean  + the vendored musl/busybox/dropbear builds (full from-scratch)
#   make help       list every target
# Run a built image with rach2/run.sh <test|os-simple|os-real>; the build system
# does not run QEMU (that script models the disk/audio/network/-cpu matrix).

# --- components (each builds via its own sub-make) ---
BASE_DIRS = krffs rach2 tests ish utils
FULL_DIRS = ports/doom

# --- vendored third-party: built by their own scripts (which wrap musl's autotools
#     and busybox/dropbear's kbuild), tracked with stamp files so a clean clone
#     "just works" without running the build_*.sh by hand, and each rebuilds only
#     when its real inputs (tarball, build script, config) change. ---
LIBC_STAMP     = vendor/musl/.stamp
BUSYBOX_STAMP  = vendor/busybox/.stamp
DROPBEAR_STAMP = vendor/dropbear/.stamp

# The staged capstone binaries the build_*.sh scripts produce, out of tree under
# build/vendor/ (mirroring their vendor/<name>/ sources). A routine `make clean` never
# reaches build/vendor, so these expensive artifacts survive it; only `make distclean`
# removes them. They are the REAL prerequisites of the capstone image (see the check
# targets below): a stamp only proves its build RAN.
BUSYBOX_BIN  = build/vendor/busybox/busybox
DROPBEAR_BIN = build/vendor/dropbear/dropbearmulti

# The pinned vendored source tarballs: the exact names the build_*.sh scripts
# verify by sha256. Used as stamp prerequisites (not a $(wildcard), which would
# silently drop the prereq and look up-to-date if a tarball were deleted) and to
# derive the extracted source dirs that `distclean` removes.
LIBC_TARBALL     = vendor/musl/musl-1.2.5.tar.gz
BUSYBOX_TARBALL  = vendor/busybox/busybox-1.36.1.tar.bz2
DROPBEAR_TARBALL = vendor/dropbear/dropbear-2022.83.tar.bz2

# --- RAM-disk images. Each is built out of tree from a checked-in manifest by the
#     manifest-driven stageimage.krffs (see krffs/mkimage.sh and krffs/manifests/),
#     which writes the whole tree directly as KRFFS v2 nodes (no FUSE mount). Each
#     image is its own file, so they coexist. ---
IMAGES_DIR      = build/images
TEST_IMAGE      = $(IMAGES_DIR)/test.krffs
OS_SIMPLE_IMAGE = $(IMAGES_DIR)/os-simple.krffs
MKIMAGE         = krffs/mkimage.sh

.PHONY: all full deps clean distclean images musl-programs test \
        test-image os-simple-image \
        check-busybox check-dropbearmulti compile-commands check-sources \
        verify-style verify-host doctor help \
        busybox dropbear $(BASE_DIRS) $(FULL_DIRS)

# Default: the kernel and the os-simple image (our shell + simple Unix tools).
all: os-simple-image

# Everything: all the RAM-disk images. Each is its own file under build/images/, so
# they coexist instead of overwriting one shared image.
full: images

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

$(LIBC_STAMP): vendor/musl/build_libc.sh $(LIBC_TARBALL)
	@rm -f $@ vendor/musl/install/lib/libc.a
	./vendor/musl/build_libc.sh
	@test -s vendor/musl/install/lib/libc.a || { echo "libc: build produced no libc.a" >&2; exit 1; }
	@touch $@

$(BUSYBOX_STAMP): vendor/busybox/build_busybox.sh vendor/busybox/rach2.config vendor/busybox/muslgcc \
                  $(BUSYBOX_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(BUSYBOX_BIN)
	./vendor/busybox/build_busybox.sh
	@test -s $(BUSYBOX_BIN) || { echo "busybox: build produced no $(BUSYBOX_BIN)" >&2; exit 1; }
	@touch $@

$(DROPBEAR_STAMP): vendor/dropbear/build_dropbear.sh vendor/dropbear/localoptions.h vendor/dropbear/muslgcc \
                   $(DROPBEAR_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(DROPBEAR_BIN)
	./vendor/dropbear/build_dropbear.sh
	@test -s $(DROPBEAR_BIN) || { echo "dropbear: build produced no $(DROPBEAR_BIN)" >&2; exit 1; }
	@touch $@

# --- component sub-makes + the build-order DAG ---
$(BASE_DIRS) $(FULL_DIRS):
	$(MAKE) -C $@

rach2: krffs
tests: rach2 $(LIBC_STAMP)
ish: rach2
utils: rach2 $(LIBC_STAMP)
ports/doom: rach2

# --- the RAM-disk images. Each target builds its inputs (via the component
#     sub-makes), then stages the image from its manifest with mkimage.sh
#     (dd + mkfs.krffs + stageimage.krffs), out of tree into build/images/. The
#     image targets are .PHONY: a re-stage is sub-second, so they always refresh. ---

# All the images (os-real, with the busybox/dropbear/Doom capstones, is added later).
images: test-image os-simple-image

# The musl integration programs (tests/musl/*.c -> build/tests/musl/), built by
# their own script. They are end-to-end libc tests staged into the test image.
musl-programs: $(LIBC_STAMP)
	sh tests/musl/build.sh

# The test image: the freestanding syscall probes plus the musl integration
# programs. `make test` boots it and asserts every probe's success token.
test-image: krffs rach2 tests musl-programs
	$(MKIMAGE) krffs/manifests/test.manifest $(TEST_IMAGE) 10

# The os-simple image: our ish shell (booted as init) and the hand-written utils.
os-simple-image: krffs rach2 ish utils
	$(MKIMAGE) krffs/manifests/os-simple.manifest $(OS_SIMPLE_IMAGE) 10

# `make test`: the automated end-to-end pass over the test image. Boots it headless
# (-cpu max) and runs every probe, failing if any does not exit 0. The few probes
# with a special contract (fork-exit-code, stdin, faults, network) are run by hand
# under tools/harness.py; see tools/runtests.py.
test: test-image
	python3 tools/runtests.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE)

# The staged capstone binaries are the REAL prerequisites of the os-real image. A
# stamp only proves its build RAN; the artifact can be deleted OR truncated afterwards
# while the stamp lingers, so these PHONY check targets (depending on the stamp so it
# builds first) run `test -s` on EVERY build and catch a missing OR zero-byte artifact.
check-busybox: $(BUSYBOX_STAMP)
	@test -s $(BUSYBOX_BIN) || { echo "busybox: $(BUSYBOX_BIN) missing or empty; rm vendor/busybox/.stamp and rebuild (make busybox)" >&2; exit 1; }
check-dropbearmulti: $(DROPBEAR_STAMP)
	@test -s $(DROPBEAR_BIN) || { echo "dropbear: $(DROPBEAR_BIN) missing or empty; rm vendor/dropbear/.stamp and rebuild (make dropbear)" >&2; exit 1; }

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

# --- IDE support + source-list guard ---
# clangd compilation database, generated from the build's own commands (GCC has no
# clang-style -MJ). Output: compile_commands.json at the repo root (git-ignored). The
# checked-in .clangd points clangd at the i686-elf cross GCC; see mk/Readme.md.
compile-commands:
	@./tools/gen_compile_commands.sh

# Kernel source-list guard: fail if a rach2 .c on disk is missing from C_SRC (so a new
# kernel source cannot be silently dropped from the build). Also run inside the gate.
check-sources:
	@./tools/check_sources.sh

# clean: the everyday reset. Each sub-make removes its own build/<component> tree
# (objects, .d, binaries), and the RAM-disk images under build/images go too. The
# expensive vendored third-party is LEFT in place: vendor/musl/install, the extracted
# busybox/dropbear trees, and the staged capstones under build/vendor (which no
# sub-make owns). Use distclean for those.
clean:
	@for dir in $(BASE_DIRS) $(FULL_DIRS); do $(MAKE) -C $$dir clean; done
	rm -rf build/images

# distclean: a full from-scratch reset; clean plus every third-party build product: the
# stamps, vendor/musl/install, the extracted vendored source trees, and the entire
# out-of-tree build/ (including the staged capstones under build/vendor that a routine
# clean leaves in place). The next build rebuilds musl/busybox/dropbear from the tarballs.
distclean: clean
	rm -f $(LIBC_STAMP) $(BUSYBOX_STAMP) $(DROPBEAR_STAMP)
	rm -rf vendor/musl/install \
	       $(LIBC_TARBALL:.tar.gz=) $(BUSYBOX_TARBALL:.tar.bz2=) $(DROPBEAR_TARBALL:.tar.bz2=)
	rm -rf build

help:
	@echo "Rach2 build targets:"
	@echo "  make            kernel + the os-simple image (ish shell + simple Unix tools)"
	@echo "  make full       all the RAM-disk images"
	@echo "  make images     the RAM-disk images (test, os-simple)"
	@echo "  make test-image / os-simple-image   one image"
	@echo "  make test       boot the test image and check every probe passes"
	@echo "  make deps       vendored third-party (libc; DEPS=full adds busybox+dropbear)"
	@echo "  make busybox / make dropbear   one capstone's third-party build"
	@echo "  make doctor     check the toolchain (PASS/MISSING with install hints)"
	@echo "  make verify-style / verify-host   quality gates"
	@echo "  make compile-commands   regenerate compile_commands.json for clangd (IDE)"
	@echo "  make check-sources      verify the kernel C_SRC list matches the sources on disk"
	@echo "  make clean      remove the in-house build products and the images"
	@echo "  make distclean  + the vendored musl/busybox/dropbear builds (full from-scratch)"
	@echo ""
	@echo "Run an image (the build system does not run QEMU):"
	@echo "  cd rach2 && ./run.sh os-simple              # boot the os-simple image"
	@echo "  cd rach2 && ./run.sh test                   # boot the test image"
