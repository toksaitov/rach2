# mk/deps.mk: the compiler flags that record each object's headers, so that a
# header edit rebuilds the objects that include it.
#
# A component adds $(DEPFLAGS) to every C compile and puts
# `-include $(OBJECTS:.o=.d)` near the end of its Makefile. GCC then writes a
# `.d` beside each `.o` listing the headers that object included, and make
# rebuilds only the objects a header edit affects, under `make -j` as well.
#
# -MMD writes the .d with the object's own headers; system headers are left
# out. -MP adds an empty phony target for each header, so that deleting or
# renaming a header does not stop make with "no rule to make target".
DEPFLAGS := -MMD -MP
