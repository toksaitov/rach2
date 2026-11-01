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
# that installs them is built again then. Each rule writes a temporary named
# by the shell's process ID, as mk/gnu.mk says. A package's stamp runs
# through `etude build` as the steps of mk/gnu.mk do, with the package's
# stage as its output.
GNU_AGENTS_ALL = $(sort $(patsubst vendor/gnu/agents/packages/%.sh,%,$(wildcard vendor/gnu/agents/packages/*.sh)))
GNU_AGENTS_ON = $(if $(filter all,$(GNU_AGENTS)),$(GNU_AGENTS_ALL),$(sort $(GNU_AGENTS)))
GNU_AGENTS_LIST = $(GNU_OUT)/agents.list
GNU_AGENTS_ALL_LIST = $(GNU_OUT)/agents-all.list
GNU_AGENTS_STAMPS = $(patsubst %,$(GNU_OUT)/agents/%.stamp,$(filter $(GNU_AGENTS_ALL),$(GNU_AGENTS_ON)))
GNU_AGENTS_ALL_STAMPS = $(patsubst %,$(GNU_OUT)/agents/%.stamp,$(GNU_AGENTS_ALL))
# The sources of every package's stamp, besides the package's own recipe.
GNU_AGENTS_SOURCES = vendor/gnu/agents/fetch_agents.sh vendor/gnu/arch.sh
GNU_STEP_STAMPS += $(wildcard $(GNU_OUT)/agents/*.stamp)

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
	@t=$@.$$$$; echo "$(GNU_AGENTS_ON)" >$$t && \
	    if cmp -s $$t $@; then rm -f $$t; else mv $$t $@; fi

$(GNU_AGENTS_ALL_LIST): gnu-agents-check
	@mkdir -p $(GNU_OUT)
	@t=$@.$$$$; echo "$(GNU_AGENTS_ALL)" >$$t && \
	    if cmp -s $$t $@; then rm -f $$t; else mv $$t $@; fi

$(GNU_OUT)/agents/%.stamp: vendor/gnu/agents/packages/%.sh $(GNU_AGENTS_SOURCES) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	@$(STEP) -r $(GNU_OUT) -n gnu-agents-$* -t $@ $(STEP_INPUTS) -o agents/$* \
	    -- sh -c 'RACH2_ARCH=$(ARCH) RACH2_GNU_OUT="$$ETUDE_STEP_ROOT" ./vendor/gnu/agents/fetch_agents.sh $*'

# The agent rows compare the running binary with the verified package bytes.
# Reading the archive needs no target executable or GNU image on the host:
# the package must list one bin/codex, a regular file that is not empty, which
# is extracted into scratch and hashed, and the digest is published whole.
build/tests/cases/machine/codex.sha256: vendor/gnu/agents/packages/codex.sh \
    $(GNU_AGENTS_SOURCES) | $(SHA256TOOL)
	@set -e; . vendor/gnu/agents/packages/codex.sh; \
	    archive=$$(RACH2_ARCH=x86_64 sh vendor/gnu/agents/fetch_agents.sh -f codex "$$PACKAGE"); \
	    listed=$$(tar -tvzf "$$archive" | awk '$$NF == "bin/codex" || $$NF == "./bin/codex"'); \
	    name=$${listed##* }; work=; \
	    trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f $@.tmp' EXIT; \
	    mkdir -p build/tests $(@D); work=$$(mktemp -d build/tests/rach2-codex-digest.XXXXXX); \
	    if [ "$$(printf '%s\n' "$$listed" | wc -l)" -ne 1 ] || [ "$${listed%"$${listed#?}"}" != - ] || \
	        ! tar -xzf "$$archive" -C "$$work" "$$name" || [ -h "$$work/$$name" ] || \
	        [ ! -f "$$work/$$name" ] || [ ! -s "$$work/$$name" ]; then \
	        echo "codex digest: $$archive must have one regular bin/codex" >&2; exit 1; \
	    fi; \
	    digest=$$($(SHA256TOOL) "$$work/$$name"); \
	    printf '%s\n' "$${digest%% *}" > $@.tmp; mv $@.tmp $@

# The same for OpenCode, whose tarball must list one regular file, opencode,
# the program the recipe installs as /opt/opencode/bin/opencode.
build/tests/cases/machine/opencode.sha256: vendor/gnu/agents/packages/opencode.sh \
    $(GNU_AGENTS_SOURCES) | $(SHA256TOOL)
	@set -e; . vendor/gnu/agents/packages/opencode.sh; \
	    archive=$$(RACH2_ARCH=x86_64 sh vendor/gnu/agents/fetch_agents.sh -f opencode \
	        "opencode-$$VERSION-linux-x64-baseline.tar.gz"); \
	    listed=$$(tar -tvzf "$$archive" | awk '$$NF == "opencode" || $$NF == "./opencode"'); \
	    name=$${listed##* }; work=; \
	    trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f $@.tmp' EXIT; \
	    mkdir -p build/tests $(@D); work=$$(mktemp -d build/tests/rach2-opencode-digest.XXXXXX); \
	    if [ "$$(printf '%s\n' "$$listed" | wc -l)" -ne 1 ] || [ "$${listed%"$${listed#?}"}" != - ] || \
	        ! tar -xzf "$$archive" -C "$$work" "$$name" || [ -h "$$work/$$name" ] || \
	        [ ! -f "$$work/$$name" ] || [ ! -s "$$work/$$name" ]; then \
	        echo "opencode digest: $$archive must have one regular opencode" >&2; exit 1; \
	    fi; \
	    digest=$$($(SHA256TOOL) "$$work/$$name"); \
	    printf '%s\n' "$${digest%% *}" > $@.tmp; mv $@.tmp $@
