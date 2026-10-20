# version.mk: build etude's programs for a cached version, as bootstrap.sh
# runs it in the root of the version's source on the machine the version is
# for. A POSIX make reads it: GNU make on Linux, macOS and Windows, pdpmake
# on Rach2. OUT is the directory the programs go in, EXE the suffix of an
# executable, and CC and CFLAGS the compiler and its flags, which bootstrap.sh
# chooses for the platform. `all` builds the programs of every platform, and
# `remote` etude-remote, which bootstrap.sh asks for on Linux and macOS.

include tools/etude/sources.mk

all: $(OUT)/casecheck$(EXE) $(OUT)/history_check$(EXE)

$(OUT)/casecheck$(EXE): $(ETUDE_CASECHECK_SRC)
	$(CC) $(CFLAGS) -Itools/etude -Itools/etude/include -Iutils -o $@ $(ETUDE_CASECHECK_SRC)

$(OUT)/history_check$(EXE): $(ETUDE_HISTORY_CHECK_SRC)
	$(CC) $(CFLAGS) -o $@ $(ETUDE_HISTORY_CHECK_SRC)

remote: $(OUT)/etude-remote$(EXE)

$(OUT)/etude-remote$(EXE): $(ETUDE_REMOTE_SRC)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -Itools/etude/include -o $@ $(ETUDE_REMOTE_SRC)

# The POSIX wrapper used outside an old checkout for bisection.
bridge: $(OUT)/etude-bridge$(EXE)

$(OUT)/etude-bridge$(EXE): $(ETUDE_BRIDGE_SRC)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -Itools/etude/include -Iutils -o $@ $(ETUDE_BRIDGE_SRC)
