# sources.mk: the local native collection and its framed execution library.
NATIVE_COLLECTION_SRC = tests/native/collection.c tests/native/inventory.c \
    tests/native/attempt.c tests/native/assets.c tests/native/proof.c tests/native/cut_transport.c \
    tools/etude/posix/run_native.c tools/etude/posix/remote.c tools/etude/json.c \
    tools/etude/posix/process.c tools/etude/guest/wire.c utils/sha256.c
NATIVE_COLLECTION_HDR = tests/native/collection.h tools/etude/include/etude/native.h \
    tools/etude/include/etude/remote.h tools/etude/include/etude/frame.h \
    tools/etude/include/etude/exec.h tools/etude/posix/posix.h tools/etude/json.h \
    tools/etude/guest/wire.h utils/sha256.h
