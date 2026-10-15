# mk/fingerprint.mk: the component's build-configuration fingerprint.
#
# A Makefile sets CONFIG_TEXT to the resolved flags that shape its objects
# (compiler name, CFLAGS, link flags) and includes this file after build.mk.
# The fingerprint $(CONFIG) under $(BUILD) records that text plus the
# compiler's path and the size and date of the binary it resolves to,
# following a symbolic link so a compiler replaced behind an unchanged link
# is seen. Every object depends on the fingerprint, so a flag change or a
# reinstalled compiler rebuilds the component and one tree can never mix
# objects from two option sets.
CONFIG = $(BUILD)/.config

# The first rule in a makefile names the default goal, and the rule below
# would take that place in a component that has not defined one yet. The goal
# make had before the include is recorded here and put back at the end.
_goal_before_fingerprint := $(.DEFAULT_GOAL)
# What a configuration change removes. The default is the whole $(BUILD)
# tree; a component whose directory is shared with another build (the musl
# test programs live under build/tests/musl) narrows this to its own
# products so a parallel sibling is not deleted mid-compile.
CONFIG_PURGE ?= $(BUILD)

# The fingerprint file is a regular prerequisite of every object and program
# of the component, and its recipe runs on every make: it reads the current
# configuration and writes the file when it is missing or differs. What a
# stale configuration would leave behind is removed first: when the file
# differs, and when there is no file but the build directory already has
# something in it, since those products were built under a configuration
# nothing recorded. The objects are then gone and the file is newer than any
# that a shared directory kept. Make then decides each object by its date,
# after this recipe. The purge runs in this recipe and not in an order-only
# step, since under `make -j` the objects make had already looked at would
# vanish under it and a link would ask for files that were gone. It is also
# what the system make (GNU 3.81) needs, which compares whole seconds: an
# object compiled in the same second the file was written would otherwise
# survive as stale. A dry run does not run the recipe and leaves the file out
# of the decision, so `make -n` prints what the recorded configuration would
# build and removes nothing.
ifeq ($(findstring n,$(firstword -$(MAKEFLAGS))),)
$(CONFIG): FORCE
endif
$(CONFIG):
	@mkdir -p $(BUILD); \
	new=$$( { command -v $(firstword $(CC)); \
	          ls -lnL "$$(command -v $(firstword $(CC)))" 2>/dev/null; \
	          printf '%s\n' "$(CONFIG_TEXT)"; } 2>/dev/null ); \
	if [ ! -f $@ ] || [ "$$new" != "$$(cat $@)" ]; then \
	    if [ -f $@ ]; then \
	        echo "$(COMPONENT): configuration changed: rebuilding from nothing"; \
	        rm -rf $(CONFIG_PURGE); mkdir -p $(BUILD); \
	    elif [ -n "$$(ls -A $(BUILD) 2>/dev/null)" ]; then \
	        echo "$(COMPONENT): no recorded configuration: rebuilding from nothing"; \
	        rm -rf $(CONFIG_PURGE); mkdir -p $(BUILD); \
	    fi; \
	    printf '%s\n' "$$new" > $@; \
	fi
FORCE:
.PHONY: FORCE
.DEFAULT_GOAL := $(_goal_before_fingerprint)
