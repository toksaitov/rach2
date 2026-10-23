# sources.mk: the local native collection and its framed execution library.
NATIVE_COLLECTION_SRC = $(filter-out tools/etude/etude.c,$(ETUDE_RACH2_SRC))
NATIVE_COLLECTION_HDR = tests/native/collection.h tests/native/history.h tools/etude/include/etude/native.h \
    tools/etude/include/etude/remote.h tools/etude/include/etude/frame.h \
    tools/etude/include/etude/exec.h tools/etude/posix/posix.h tools/etude/json.h \
    tools/etude/guest/wire.h utils/sha256.h

# The owner-side deployment helper invokes only prepared peers and SSH.
NATIVE_DEPLOY_SRC = tests/native/deploy.c tests/native/deploy_options.c \
    tests/native/deploy_process.c tests/native/deploy_remote.c tests/native/deploy_peers.c \
    tools/etude/posix/exec.c tools/etude/posix/keeper.c tools/etude/posix/process.c \
    tools/etude/capture.c tools/etude/json.c utils/sha256.c
