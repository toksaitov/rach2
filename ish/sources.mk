# ish/sources.mk: the one ISH source list, read by the host Makefile and by
# guest.mk under pdpmake on Rach2. Both derive their object lists from it,
# so the two builds cannot drift apart. crt0.c stays first: ish links
# -nostdlib with its own entry code, and the linker keeps input order.
ISH_SRC = crt0.c ish.c ish_shell_utilities.c ish_cstring_utilities.c \
          ish_syscalls.c
