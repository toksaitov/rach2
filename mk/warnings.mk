# mk/warnings.mk: the kernel's curated warning flags, all under -Werror.
#
# The set was taken from `i686-elf-gcc -Q --help=warning` and measured with
# the testing host's GCC 15.2. A flag is on the list when its sites are fixed
# by a type or a declaration; a flag whose sites would be fixed by casts is
# rejected and listed below with the reason.
#
# -Wpedantic is the mechanical form of the dialect rule: the kernel stays
# inside the C89 that GCC and TinyCC share, and a construct outside it is
# found at compile time, not by `tests/run selfbuild` minutes later.
# -Wno-long-long stays because the 64-bit integer types are that dialect's.
#
# -Wframe-larger-than=4096 is the frame bound against the 16 KiB kernel
# stack each process has: a quarter of it for one frame, so an interrupt
# taken on the deepest chain still has room. The bound is the owner's to
# move.
KWARN = -Wpedantic -Wno-long-long \
        -Wshadow -Wstrict-prototypes -Wpointer-arith -Wundef -Wlogical-op \
        -Wduplicated-cond -Wjump-misses-init -Wformat-security \
        -Wformat-signedness -Wnested-externs -Winit-self \
        -Wmissing-prototypes \
        -Wwrite-strings \
        -Wredundant-decls \
        -Wduplicated-branches \
        -Wswitch-default \
        -Wswitch-enum \
        -Wbad-function-cast \
        -Wcast-qual \
        -Wcast-align=strict \
        -Wframe-larger-than=4096

# Rejected, with the reason:
#
# -Wconversion -Wsign-conversion: each site is an arithmetic narrowing whose
#   only fix is a cast that says nothing the type does not; a cast at every
#   one hides the few that matter.
# -Wunused-macros: the sites are register maps and ABI constants defined
#   beside the ones a driver or a system call uses, so the reader has the
#   datasheet's or the interface's whole set; the flag would have them
#   removed or moved into headers, where it no longer warns, and nothing is
#   gained.
