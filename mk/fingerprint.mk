# mk/fingerprint.mk: the component's build-configuration fingerprint.
#
# A Makefile sets CONFIG_TEXT to the resolved flags that shape its objects
# (compiler name, CFLAGS, link flags) and includes this file after build.mk.
# The fingerprint $(CONFIG) under $(BUILD) records that text plus the
# compiler binary's identity. When the recorded configuration differs from
# the current one, the whole $(BUILD) tree is removed HERE, at parse time,
# so a flag change or a reinstalled compiler rebuilds the component from
# nothing and one tree can never mix objects from two option sets. This
# replaces the old kernel `kflags` stamp, which refused such a tree instead
# of rebuilding it. The purge is deliberate where a timestamp prerequisite
# would not be: the system make (GNU 3.81) compares whole seconds, and an
# object landing in the fingerprint's second would survive as stale.
CONFIG = $(BUILD)/.config

# What a configuration change removes. The default is the whole $(BUILD)
# tree; a component whose directory is shared with another build (the musl
# test programs live under build/tests/musl) narrows this to its own
# products so a parallel sibling is not deleted mid-compile.
CONFIG_PURGE ?= $(BUILD)

# A first parse (no fingerprint yet) records the configuration without
# purging: a fresh tree has nothing stale, and under `make -j` a purge here
# would race a sibling already building into a shared directory.
_ := $(shell mkdir -p $(BUILD); \
    new=$$( { command -v $(firstword $(CC)); \
              ls -ln "$$(command -v $(firstword $(CC)))" 2>/dev/null; \
              printf '%s\n' "$(CONFIG_TEXT)"; } 2>/dev/null ); \
    if [ ! -f $(CONFIG) ]; then \
        printf '%s\n' "$$new" > $(CONFIG); \
    elif [ "$$new" != "$$(cat $(CONFIG))" ]; then \
        rm -rf $(CONFIG_PURGE); mkdir -p $(BUILD); \
        printf '%s\n' "$$new" > $(CONFIG); \
    fi)
