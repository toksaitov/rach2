# mk/rules.mk -- the canonical compile rule + auto-dependency include for a
# component that compiles to .o and then links a final binary the plain way. A
# component sets CC, CFLAGS (with $(DEPFLAGS) from mk/deps.mk), and OBJECTS, then
# `include ../mk/rules.mk`. ish/ uses it directly.
#
# Two components keep their OWN compile rules instead of including this file, by
# design: ports/doom (it compiles from two source dirs -- the local dir and
# src/) and the krffs host tools (the FUSE objects need extra cflags the standalone
# tools do not). The single-source test/util programs build with a `%: %.c` rule
# (compile+link in one step) and add their own `-include $(PROGS:=.d)`. All three
# shapes still share mk/toolchain.mk + mk/deps.mk -- this file is just the common
# compile-then-link case.

%.o : %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Pull in the GCC-written header prerequisites (-MMD) for this component's objects,
# so editing a header rebuilds exactly the objects that include it. Silent on a
# first build (no .d yet); -MP keeps a deleted header from wedging make.
-include $(OBJECTS:.o=.d)
