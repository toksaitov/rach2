# mk/build.mk: every in-house component builds OUT OF TREE into one repo-root
# `build/` directory whose layout mirrors the source tree. A component states
# where the repo root is (TOP) and its own name (COMPONENT, defaulting to the
# directory name), then includes this file. Its intermediates and its final
# binary then land under $(BUILD), which is `build/<component>/`: the kernel at
# build/rach2/kernel beside build/rach2/sys/mm/paging.o, the utilities at
# build/utils/cat, and so on. mk/rules.mk's `$(BUILD)/%.o` compile rule and each
# component's own link rule both write through $(BUILD), so the source tree
# never has an object or a binary, and one `rm -rf build/` cleans everything.
#
# BUILDROOT is kept relative (`$(TOP)/build`). Each sub-make runs with CURDIR
# set to its own component directory, whether built standalone (`make -C utils`)
# or through the top-level make, so `../build` resolves to the same repo-root
# build/ either way, the echoed compiler commands stay short, and a repo path
# that contains a space does not get embedded in every object path (which GNU
# Make, splitting on spaces, cannot handle).
TOP       ?= ..
COMPONENT ?= $(notdir $(CURDIR))
BUILDROOT ?= $(TOP)/build
BUILD     ?= $(BUILDROOT)/$(COMPONENT)
