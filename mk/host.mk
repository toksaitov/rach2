# mk/host.mk: how a host program is compiled.
#
# The programs that decide verdicts, the checkers under tools/, the test
# runners and fixture makers under tests/system/ and tests/host/, and the
# image tools, are compiled with the host compiler from one rule text, so
# that every one of them is built with the same warnings under -Werror and
# a flag added here reaches all of them. A rule names its program, its
# sources and, after $(HOST_LINK), whatever else it needs: a library, an
# include directory. The recipe is two lines, the directory and the link.
HOSTCC ?= cc
HOSTCFLAGS = -O2 -Wall -Wextra -Werror
HOST_LINK = $(HOSTCC) $(HOSTCFLAGS) -o $@ $(filter %.c,$^)
