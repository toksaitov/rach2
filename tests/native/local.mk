# local.mk: build the controller locally with the installed TinyCC.
# Run from the prepared tree: make -f tests/native/local.mk CC=tcc.
include tests/native/sources.mk
CC = tcc
NATIVE_LOCAL = build/tests/native/native-collection-local
NATIVE_FLAGS = -std=c99 -static -D__rach2__ -D_DEFAULT_SOURCE \
    -Itools/etude/include -Itools/etude -Itools/etude/posix -Itools/etude/guest
.PHONY: all
all: $(NATIVE_LOCAL)
$(NATIVE_LOCAL): $(NATIVE_COLLECTION_SRC) $(NATIVE_COLLECTION_HDR)
	mkdir -p build/tests/native
	$(CC) $(NATIVE_FLAGS) -o $@ $(NATIVE_COLLECTION_SRC)
