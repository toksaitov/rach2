# mk/gnu-agents.mk: the agents layer of the GNU system
# (vendor/gnu/agents/Readme.md), the vendors' published builds of coding
# agents and their tools, each fetched and checked by a recipe of
# vendor/gnu/agents/packages/. Like the rest of the GNU layer it builds on a
# Linux host and is not part of `all`, and its packages are for x86-64 only.
# No package is installed unless the builder names it: GNU_AGENTS lists the
# packages the os-real-gnu root gets, or is `all` for every recipe, and is
# empty by default. `make gnu-agents` stages the packages it names, each
# behind a stamp of its own that its recipe and fetch_agents.sh renew.
# agents.list records the packages GNU_AGENTS names and agents-all.list every
# recipe, each rewritten only when the list changes, so that the root image
# that installs them is built again then.
GNU_AGENTS_ALL = $(sort $(patsubst vendor/gnu/agents/packages/%.sh,%,$(wildcard vendor/gnu/agents/packages/*.sh)))
GNU_AGENTS_ON = $(if $(filter all,$(GNU_AGENTS)),$(GNU_AGENTS_ALL),$(sort $(GNU_AGENTS)))
GNU_AGENTS_LIST = $(GNU_OUT)/agents.list
GNU_AGENTS_ALL_LIST = $(GNU_OUT)/agents-all.list
GNU_AGENTS_STAMPS = $(patsubst %,$(GNU_OUT)/agents/%.stamp,$(filter $(GNU_AGENTS_ALL),$(GNU_AGENTS_ON)))
GNU_AGENTS_ALL_STAMPS = $(patsubst %,$(GNU_OUT)/agents/%.stamp,$(GNU_AGENTS_ALL))

.PHONY: gnu-agents gnu-agents-check
gnu-agents: $(GNU_AGENTS_LIST) $(GNU_AGENTS_STAMPS)

$(GNU_AGENTS_LIST): gnu-agents-check
	@for p in $(GNU_AGENTS_ON); do \
	    [ -f vendor/gnu/agents/packages/$$p.sh ] || { echo "gnu: agents: GNU_AGENTS names $$p, which has no recipe in vendor/gnu/agents/packages" >&2; exit 1; }; \
	done
	@if [ -n "$(GNU_AGENTS_ON)" ] && [ "$(ARCH)" != x86_64 ]; then \
	    echo "gnu: agents: the packages are for x86-64 only; build with ARCH=x86_64 or without GNU_AGENTS" >&2; exit 1; \
	fi
	@mkdir -p $(GNU_OUT)
	@echo "$(GNU_AGENTS_ON)" >$@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi

$(GNU_AGENTS_ALL_LIST): gnu-agents-check
	@mkdir -p $(GNU_OUT)
	@echo "$(GNU_AGENTS_ALL)" >$@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi

$(GNU_OUT)/agents/%.stamp: vendor/gnu/agents/packages/%.sh vendor/gnu/agents/fetch_agents.sh vendor/gnu/arch.sh | $(SHA256TOOL)
	@rm -f $@
	@RACH2_ARCH=$(ARCH) ./vendor/gnu/agents/fetch_agents.sh $*
	@touch $@
