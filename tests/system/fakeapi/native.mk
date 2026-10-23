# native.mk: portable C services and their preparation-only TLS dependency.
FAKEAPI_NATIVE = tests/system/fakeapi/native
FAKEAPI_SRC = $(FAKEAPI_NATIVE)/json.c $(FAKEAPI_NATIVE)/platform.c \
              $(FAKEAPI_NATIVE)/tls.c $(FAKEAPI_NATIVE)/server.c \
              $(FAKEAPI_NATIVE)/protocols.c $(FAKEAPI_NATIVE)/responses.c $(FAKEAPI_NATIVE)/assets.c $(FAKEAPI_NATIVE)/archive.c $(FAKEAPI_NATIVE)/worker.c tools/etude/volume/volume.c
FAKEAPI_HDR = $(wildcard $(FAKEAPI_NATIVE)/*.h)
FAKEAPI_SSL = build/vendor/host-openssl
FAKEAPI_CC = $(HOSTCC)
FAKEAPI_LIBS = -lpthread
FAKEAPI_FLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L
ifeq ($(shell uname -s),Linux)
FAKEAPI_LIBS += -ldl
endif
ifeq ($(OS),Windows_NT)
FAKEAPI_CC = $(ETUDECC)
FAKEAPI_FLAGS += -D_WIN32_WINNT=0x0A00 -static
FAKEAPI_LIBS = -lws2_32 -lcrypt32 -ladvapi32 -lbcrypt
endif
$(FAKEAPI_SSL)/ready: tests/system/fakeapi/build_openssl.sh vendor/gnu/sources.txt \
                     vendor/gnu/sources-extra.txt vendor/gnu/fetch.sh
	FAKEAPI_CC='$(FAKEAPI_CC)' sh tests/system/fakeapi/build_openssl.sh

build/tests/cases/machine/agent_server: $(FAKEAPI_SRC) $(FAKEAPI_HDR) $(FAKEAPI_SSL)/ready
	@mkdir -p $(@D)
	$(FAKEAPI_CC) $(HOSTCFLAGS) $(FAKEAPI_FLAGS) -I$(FAKEAPI_SSL)/include \
	    -o $@ $(FAKEAPI_SRC) $(FAKEAPI_SSL)/lib/libssl.a $(FAKEAPI_SSL)/lib/libcrypto.a $(FAKEAPI_LIBS)
build/tests/cases/machine/agent_relay: build/tests/cases/machine/agent_server
	@mkdir -p $(@D)
	cp $< $@
build/tests/cases/gnu/sntpserver: tests/system/sntpserver.c $(FAKEAPI_NATIVE)/platform.c $(FAKEAPI_NATIVE)/platform.h
	@mkdir -p $(@D)
	$(FAKEAPI_CC) $(HOSTCFLAGS) $(FAKEAPI_FLAGS) -o $@ $(filter %.c,$^) $(FAKEAPI_LIBS)

# The native protocol regression is a prepared program, with no interpreter.
build/tests/system/fakeapi_test: $(FAKEAPI_NATIVE)/selftest.c $(filter-out $(FAKEAPI_NATIVE)/server.c,$(FAKEAPI_SRC)) $(FAKEAPI_HDR) $(FAKEAPI_NATIVE)/server.c $(FAKEAPI_SSL)/ready
	@mkdir -p $(@D)
	$(FAKEAPI_CC) $(HOSTCFLAGS) $(FAKEAPI_FLAGS) -I$(FAKEAPI_SSL)/include \
	    -o $@ $(FAKEAPI_NATIVE)/selftest.c $(filter-out $(FAKEAPI_NATIVE)/server.c,$(FAKEAPI_SRC)) \
	    $(FAKEAPI_SSL)/lib/libssl.a $(FAKEAPI_SSL)/lib/libcrypto.a $(FAKEAPI_LIBS)

# Immutable scripts and fixtures. Host C reads metadata; the guest receives
# the same archive for workloads that exercise the tree's tools.
AGENT_ASSET_FILES = $(wildcard $(shell git ls-files tests tools vendor/gnu/agents/packages))
build/tests/cases/machine/agents-assets.tar: $(AGENT_ASSET_FILES)
	@mkdir -p $(@D)
	git ls-files tests tools vendor/gnu/agents/packages | tar --format=ustar -cf $@ -T -
