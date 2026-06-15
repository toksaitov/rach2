# Rach2 Root Makefile
#
# Builds all components in the correct order.

# Subdirectories in build order
SUBDIRS = krffs    \
          rach2    \
          tests    \
          ish      \
          utils    \
          ports/doom

.PHONY: all clean ramdisk verify-style verify-host doctor $(SUBDIRS)

all: $(SUBDIRS) ramdisk

# Toolchain preflight (see tools/doctor.sh): report each required/optional tool as
# PASS/MISSING/OPTIONAL with an install hint; non-zero exit if a required tool is
# missing. Run this first on a fresh machine.
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

$(SUBDIRS):
	$(MAKE) -C $@

# Enforce build order (dependencies between directories)
rach2: krffs
tests: rach2
ish: rach2
utils: rach2
ports/doom: rach2

# Build RAM disk after all binaries are compiled
ramdisk: $(SUBDIRS)
	cd krffs && ./build_ramdisk.sh

clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean; \
	done
