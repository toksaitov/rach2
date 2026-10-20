# version.mk: build etude's programs for a cached version, as bootstrap.sh
# runs it in the root of the version's source on the machine the version is
# for. A POSIX make reads it: GNU make on Linux, macOS and Windows, pdpmake
# on Rach2. OUT is the directory the programs go in, EXE the suffix of an
# executable, and CC and CFLAGS the compiler and its flags, which bootstrap.sh
# chooses for the platform.

include tools/etude/sources.mk

all: $(OUT)/casecheck$(EXE) $(OUT)/history_check$(EXE)

$(OUT)/casecheck$(EXE): $(ETUDE_CASECHECK_SRC)
	$(CC) $(CFLAGS) -Itools/etude -Iutils -o $@ $(ETUDE_CASECHECK_SRC)

$(OUT)/history_check$(EXE): $(ETUDE_HISTORY_CHECK_SRC)
	$(CC) $(CFLAGS) -o $@ $(ETUDE_HISTORY_CHECK_SRC)
