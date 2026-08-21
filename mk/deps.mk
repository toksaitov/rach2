# mk/deps.mk: automatic header-dependency tracking (the fix for the old
# "edit a .h, then `make clean && make`" ritual).
#
# Add $(DEPFLAGS) to every C compile and `-include $(OBJECTS:.o=.d)` near the
# end of the Makefile. GCC then writes a `.d` next to each `.o` listing the
# headers that object included, and make rebuilds exactly the objects affected
# by a header edit (no full rebuild), and `make -j` stays correct.
#
#   -MMD : alongside the .o, emit a .d with the object's header prerequisites
#          (user headers only; system headers are skipped, like our krffs tools)
#   -MP  : also emit an empty phony target for each header, so deleting or renaming
#          a header does not wedge make with a "no rule to make <header>" error
DEPFLAGS := -MMD -MP
