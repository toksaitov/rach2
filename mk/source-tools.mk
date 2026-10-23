# Pinned native judges. These targets prepare tools; they run no suite.
SOURCE_TOOL_PREFIX ?= $(abspath build/host-tools/native)
SOURCE_TOOL_SCRIPT = tools/setup/build_host_tool.sh

.PHONY: host-source-tools host-pmccabe host-mandoc source-tools-force
source-tools-force:
host-source-tools: host-pmccabe host-mandoc
host-pmccabe: $(SOURCE_TOOL_PREFIX)/pmccabe.json
host-mandoc: $(SOURCE_TOOL_PREFIX)/mandoc.json

$(SOURCE_TOOL_PREFIX)/pmccabe.json: $(SOURCE_TOOL_SCRIPT) mk/source-tools.mk source-tools-force
	sh $(SOURCE_TOOL_SCRIPT) pmccabe $(SOURCE_TOOL_PREFIX)

$(SOURCE_TOOL_PREFIX)/mandoc.json: $(SOURCE_TOOL_SCRIPT) mk/source-tools.mk \
    $(wildcard tools/setup/mandoc-win/*.[ch] tools/setup/mandoc-win/sys/*.h) \
    tools/setup/mandoc-batch.mk source-tools-force
	sh $(SOURCE_TOOL_SCRIPT) mandoc $(SOURCE_TOOL_PREFIX)
