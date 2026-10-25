# The compiler leaf of native host preparation. Etude supplies the selected
# output and source metadata; Make compiles and consumes its dependency files.
HOSTCC ?= cc
include tools/etude/sources.mk
HOST_CHECK_SCAN_FLAG ?= -MM
HOST_CHECK_WORK = $(HOST_CHECK_OUTPUT)-sources
HOST_CHECK_SCAN_WORK ?= $(HOST_CHECK_WORK)
HOST_CHECK_LINK_OUTPUT ?= $(HOST_CHECK_OUTPUT)
HOST_CHECK_OBJECTS = $(addprefix $(HOST_CHECK_WORK)/,$(addsuffix .o,$(HOST_CHECK_SOURCES)))
HOST_CHECK_SCANS = $(addprefix $(HOST_CHECK_SCAN_WORK)/,$(addsuffix .scan,$(HOST_CHECK_SOURCES)))
HOST_CHECK_PREPROCESSED = $(addprefix $(HOST_CHECK_SCAN_WORK)/,$(addsuffix .i,$(HOST_CHECK_SOURCES)))
HOST_CHECK_COMPILE = $(filter-out -l% -L% -Wl%,$(HOST_CHECK_FLAGS))
HOST_CHECK_LINK = $(filter -fsanitize=% -g -pthread -static -municode -L% -Wl%,$(HOST_CHECK_FLAGS))
HOST_CHECK_LIBS = $(filter -l%,$(HOST_CHECK_FLAGS)) $(HOST_CHECK_LIBRARIES)
HOST_CHECK_FORCED = $(if $(HOST_CHECK_FORCE),host-check-build)

.PHONY: all scan preprocess host-check-scan host-check-build print-sources print-compiler
all: $(HOST_CHECK_OUTPUT)
scan: $(HOST_CHECK_SCANS)
preprocess: $(HOST_CHECK_PREPROCESSED)
host-check-scan host-check-build:
print-sources:
	@printf '%s\n' "$($(HOST_CHECK_SOURCE_LIST))"
print-compiler:
ifeq ($(OS),Windows_NT)
	@cygpath -m "$$(command -v "$(HOST_CHECK_COMPILER_NAME)")"
else
	@command -v "$(HOST_CHECK_COMPILER_NAME)"
endif

ifneq ($(strip $(HOST_CHECK_OUTPUT)),)
ifeq ($(HOST_CHECK_OBJECT_MODE),1)
ifeq ($(shell uname -s),Darwin)
HOST_CHECK_LINK += -Wl,-reproducible
endif
$(HOST_CHECK_OUTPUT): $(HOST_CHECK_OBJECTS) $(HOST_CHECK_FORCED)
	$(HOSTCC) $(HOST_CHECK_LINK) -o $(HOST_CHECK_LINK_OUTPUT) $(HOST_CHECK_OBJECTS) $(HOST_CHECK_LIBS)
else
$(HOST_CHECK_OUTPUT): $(HOST_CHECK_SOURCES) $(HOST_CHECK_FORCED)
	@mkdir -p $(@D)
	$(HOSTCC) $(HOST_CHECK_FLAGS) -o $(HOST_CHECK_LINK_OUTPUT) $(HOST_CHECK_SOURCES) $(HOST_CHECK_LIBRARIES)
endif
ifeq ($(OS),Windows_NT)
	@if test -f $(HOST_CHECK_LINK_OUTPUT).exe; then mv $(HOST_CHECK_LINK_OUTPUT).exe $(HOST_CHECK_LINK_OUTPUT); fi
endif

define host_check_source
$(HOST_CHECK_SCAN_WORK)/$(1).scan: $(1) host-check-scan
	@mkdir -p $$(@D)
	$(HOSTCC) $(HOST_CHECK_COMPILE) $(HOST_CHECK_SCAN_FLAG) -MT host-input $(1) > $$@
$(HOST_CHECK_WORK)/$(1).o: $(1) $(HOST_CHECK_FORCED)
	@mkdir -p $$(@D)
	$(HOSTCC) $(HOST_CHECK_COMPILE) -MMD -MP -MF $$@.d -c $(1) -o $$@
$(HOST_CHECK_SCAN_WORK)/$(1).i: $(1) host-check-scan
	@mkdir -p $$(@D)
	$(HOSTCC) $(HOST_CHECK_COMPILE) -E $(1) > $$@
endef
$(foreach source,$(HOST_CHECK_SOURCES),$(eval $(call host_check_source,$(source))))
-include $(HOST_CHECK_WORK)/dependencies.mk $(addsuffix .d,$(HOST_CHECK_OBJECTS))
endif
