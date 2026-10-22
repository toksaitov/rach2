# version.mk: build etude's programs for a cached version, as bootstrap.sh
# runs it in the root of the version's source on the machine the version is
# for. A POSIX make reads it: GNU make on Linux, macOS and Windows, pdpmake
# on Rach2. OUT is the directory the programs go in, EXE the suffix of an
# executable, and CC and CFLAGS the compiler and its flags, which bootstrap.sh
# chooses for the platform. `all` builds the programs of every platform,
# `remote` etude-remote and `bridge` etude-bridge, which bootstrap.sh asks for
# on Linux and macOS, `hold` etude-hold, which it asks for on every platform
# but Rach2, and `ports` etude-ports, which it asks for on Linux, macOS and
# Windows.

include tools/etude/sources.mk

all: $(OUT)/casecheck$(EXE) $(OUT)/history_check$(EXE)

$(OUT)/casecheck$(EXE): $(ETUDE_CASECHECK_SRC)
	$(CC) $(CFLAGS) -Itools/etude -Itools/etude/include -Iutils -o $@ $(ETUDE_CASECHECK_SRC)

$(OUT)/history_check$(EXE): $(ETUDE_HISTORY_CHECK_SRC)
	$(CC) $(CFLAGS) -o $@ $(ETUDE_HISTORY_CHECK_SRC)

remote: $(OUT)/etude-remote$(EXE)

$(OUT)/etude-remote$(EXE): $(ETUDE_REMOTE_SRC)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -Itools/etude/include -o $@ $(ETUDE_REMOTE_SRC)

# The admission wrapper that runs an old checkout's commands from outside it.
bridge: $(OUT)/etude-bridge$(EXE)

$(OUT)/etude-bridge$(EXE): $(ETUDE_BRIDGE_SRC)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -Itools/etude/include -Iutils -o $@ $(ETUDE_BRIDGE_SRC)

# The holder of a tree's run lock. HOLD_SRC and HOLD_FLAGS give the sources
# and flags of Linux and macOS, and bootstrap.sh gives those of Windows.
HOLD_SRC = $(ETUDE_HOLD_SRC)
HOLD_FLAGS = -D_DEFAULT_SOURCE

hold: $(OUT)/etude-hold$(EXE)

$(OUT)/etude-hold$(EXE): $(HOLD_SRC)
	$(CC) $(CFLAGS) $(HOLD_FLAGS) -o $@ $(HOLD_SRC)

# The holder of the host lock and a block of host ports for one command.
ports: $(OUT)/etude-ports$(EXE)

$(OUT)/etude-ports$(EXE): $(ETUDE_PORTS_SRC)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -Itools/etude/include -Itools/etude -o $@ $(ETUDE_PORTS_SRC)
