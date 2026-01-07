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

.PHONY: all clean ramdisk $(SUBDIRS)

all: $(SUBDIRS) ramdisk

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
