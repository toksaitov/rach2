# mk/fingerprint.mk: the component's build-configuration fingerprint.
#
# A Makefile sets CONFIG_TEXT to the resolved flags that shape its objects
# (compiler name, CFLAGS, link flags) and includes this file after build.mk.
# The fingerprint $(CONFIG) under $(BUILD) records that text plus the
# compiler binary's identity. When the recorded configuration differs from
# the current one, the `fingerprint` step below removes the $(BUILD) tree, so
# a flag change or a reinstalled compiler rebuilds the component from
# nothing and one tree can never mix objects from two option sets. This
# replaces the old kernel `kflags` stamp, which refused such a tree instead
# of rebuilding it.
CONFIG = $(BUILD)/.config

# What a configuration change removes. The default is the whole $(BUILD)
# tree; a component whose directory is shared with another build (the musl
# test programs live under build/tests/musl) narrows this to its own
# products so a parallel sibling is not deleted mid-compile.
CONFIG_PURGE ?= $(BUILD)

# The step a build runs before its first object: `fingerprint` reads the
# current configuration, compares it with the recorded one and, on a change,
# removes what a stale configuration would leave behind and records the new
# text. A first run records without purging. Every object and program of the
# component lists this step as an order-only prerequisite, so it runs before
# any compile in the same make, once per invocation; a dry run prints the
# step and removes nothing, since a recipe is the only place the purge lives.
# The purge is deliberate where a timestamp prerequisite would not be: the
# system make (GNU 3.81) compares whole seconds, and an object landing in the
# fingerprint's second would survive as stale.
.PHONY: fingerprint
fingerprint:
	@mkdir -p $(BUILD); \
	new=$$( { command -v $(firstword $(CC)); \
	          ls -ln "$$(command -v $(firstword $(CC)))" 2>/dev/null; \
	          printf '%s\n' "$(CONFIG_TEXT)"; } 2>/dev/null ); \
	if [ ! -f $(CONFIG) ]; then \
	    printf '%s\n' "$$new" > $(CONFIG); \
	elif [ "$$new" != "$$(cat $(CONFIG))" ]; then \
	    echo "$(COMPONENT): configuration changed: rebuilding from nothing"; \
	    rm -rf $(CONFIG_PURGE); mkdir -p $(BUILD); \
	    printf '%s\n' "$$new" > $(CONFIG); \
	fi
