# local.mk: build the controller locally with the installed TinyCC.
# Run from the prepared tree: make -f tests/native/local.mk CC=tcc.
include tests/native/sources.mk
include tools/etude/sources.mk
CC = tcc
NATIVE_LOCAL = build/tests/native/local/etude
NATIVE_FLAGS = -std=c99 -static -D__rach2__ -D_DEFAULT_SOURCE \
    -Itools/etude/include -Itools/etude -Itools/etude/posix -Itools/etude/guest -Iutils
.PHONY: all
all: $(NATIVE_LOCAL)
$(NATIVE_LOCAL): $(ETUDE_RACH2_SRC) $(NATIVE_COLLECTION_HDR) tools/etude/*.h tools/etude/include/etude/*.h
	mkdir -p build/tests/native/local
	$(CC) $(NATIVE_FLAGS) -o $@ $(ETUDE_RACH2_SRC)
