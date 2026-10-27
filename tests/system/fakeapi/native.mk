# native.mk: portable C services and their preparation-only TLS dependency.
FAKEAPI_NATIVE = tests/system/fakeapi/native
FAKEAPI_SRC = $(FAKEAPI_NATIVE)/json.c $(FAKEAPI_NATIVE)/platform.c \
              $(FAKEAPI_NATIVE)/tls.c $(FAKEAPI_NATIVE)/server.c \
              $(FAKEAPI_NATIVE)/protocols.c $(FAKEAPI_NATIVE)/responses.c \
              $(FAKEAPI_NATIVE)/assets.c $(FAKEAPI_NATIVE)/archive.c \
              $(FAKEAPI_NATIVE)/worker.c tools/etude/volume/volume.c
FAKEAPI_HDR = $(wildcard $(FAKEAPI_NATIVE)/*.h) tools/etude/include/etude/args.h
SNTP_SRC = tests/system/sntpserver.c $(FAKEAPI_NATIVE)/platform.c
FAKEAPI_SSL = build/vendor/host-openssl
FAKEAPI_CC = $(HOSTCC)
FAKEAPI_LIBS = -lpthread
FAKEAPI_FLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L
FAKEAPI_LINK = $(FAKEAPI_CC) $(HOSTCFLAGS) $(FAKEAPI_FLAGS) -o $@
ifeq ($(shell uname -s),Linux)
FAKEAPI_LIBS += -ldl
endif
ifeq ($(OS),Windows_NT)
FAKEAPI_CC = $(ETUDECC)
FAKEAPI_FLAGS += -D_WIN32_WINNT=0x0A00 -static -Itools/etude/include -Itools/etude -Iutils
FAKEAPI_SRC += $(ETUDE_WIN_FILE_SRC)
SNTP_SRC += $(ETUDE_WIN_FILE_SRC)
FAKEAPI_HDR += $(wildcard tools/etude/win/*.h tools/etude/include/etude/*.h)
FAKEAPI_LIBS = -lws2_32 -lcrypt32 -ladvapi32 -lbcrypt
FAKEAPI_LINK = sh tools/etude/compile.sh $(FAKEAPI_CC) $@ $(HOSTCFLAGS) $(FAKEAPI_FLAGS)
endif
$(FAKEAPI_SSL)/ready: tests/system/fakeapi/build_openssl.sh vendor/gnu/sources.txt \
                     vendor/gnu/sources-extra.txt vendor/gnu/fetch.sh
	FAKEAPI_CC='$(FAKEAPI_CC)' sh tests/system/fakeapi/build_openssl.sh

build/tests/cases/machine/agent_server: $(FAKEAPI_SRC) $(FAKEAPI_HDR) $(FAKEAPI_SSL)/ready
	@mkdir -p $(@D)
	$(FAKEAPI_LINK) -I$(FAKEAPI_SSL)/include \
	    $(FAKEAPI_SRC) $(FAKEAPI_SSL)/lib/libssl.a $(FAKEAPI_SSL)/lib/libcrypto.a $(FAKEAPI_LIBS)
build/tests/cases/machine/agent_relay: build/tests/cases/machine/agent_server
	@mkdir -p $(@D)
	cp $< $@
build/tests/cases/gnu/sntpserver: $(SNTP_SRC) $(FAKEAPI_NATIVE)/platform.h
	@mkdir -p $(@D)
	$(FAKEAPI_LINK) $(SNTP_SRC) $(FAKEAPI_LIBS)

# The native protocol regression is a prepared program, with no interpreter.
build/tests/system/fakeapi_test: $(FAKEAPI_NATIVE)/selftest.c $(filter-out $(FAKEAPI_NATIVE)/server.c $(FAKEAPI_NATIVE)/tls.c,$(FAKEAPI_SRC)) $(FAKEAPI_HDR) $(FAKEAPI_NATIVE)/server.c $(FAKEAPI_NATIVE)/tls.c $(FAKEAPI_SSL)/ready \
    tests/host/compat/fixdir_host.c tests/host/compat/fixdir_host.h tests/host/compat/tree_host.h
	@mkdir -p $(@D)
	$(FAKEAPI_LINK) -I$(FAKEAPI_SSL)/include \
	    $(FAKEAPI_NATIVE)/selftest.c $(filter-out $(FAKEAPI_NATIVE)/server.c $(FAKEAPI_NATIVE)/tls.c,$(FAKEAPI_SRC)) \
	    $(FAKEAPI_SSL)/lib/libssl.a $(FAKEAPI_SSL)/lib/libcrypto.a $(FAKEAPI_LIBS)

# Immutable scripts and fixtures. Host C reads metadata; the guest receives
# the same archive for workloads that exercise the tree's tools.
AGENT_ASSET_PATHS = tests/system/agents_rows.sh tests/system/agents \
    tests/system/accept/agents_guest.sh tests/system/accept/agents \
    'tests/system/fakeapi/*.json' tests/system/fakeapi/repo \
    tools/flow tools/git-hooks/commit-msg vendor/gnu/agents/packages
AGENT_ASSET_FILES := $(shell git ls-files -- $(AGENT_ASSET_PATHS))
build/tests/cases/machine/agents-assets.tar: $(AGENT_ASSET_FILES) tests/system/fakeapi/native.mk
	@mkdir -p $(@D)
	tar --format=ustar -cf $@ $(AGENT_ASSET_FILES)
