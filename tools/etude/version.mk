# version.mk: build etude for a cached version, as bootstrap.sh runs it in
# the root of the version's source on the machine the version is for. A POSIX
# make reads it: GNU make on Linux, macOS and Windows, pdpmake on Rach2. OUT
# is the directory the program goes in, EXE the suffix of an executable, and
# CC and CFLAGS the compiler and its flags, which bootstrap.sh chooses for the
# platform. SRC, FLAGS and LIBS give the sources, the definitions and the
# libraries of the platform's etude: those of Linux and macOS unless
# bootstrap.sh gives those of Windows or Rach2.

include tools/etude/sources.mk

SRC = $(ETUDE_SRC)
FLAGS = -D_DEFAULT_SOURCE
LIBS =

all: $(OUT)/etude$(EXE)

$(OUT)/etude$(EXE): $(SRC)
	$(CC) $(CFLAGS) $(FLAGS) -Itools/etude/include -Itools/etude -Itools/etude/posix -Itools/etude/guest -Iutils \
	    -o $@ $(SRC) $(LIBS)
