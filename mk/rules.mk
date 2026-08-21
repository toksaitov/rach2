# mk/rules.mk: the canonical OUT-OF-TREE compile rule + auto-dependency include
# for a component that compiles to .o and then links a final binary the plain
# way. A component sets CC, CFLAGS (with $(DEPFLAGS) from mk/deps.mk) and
# OBJECTS as $(SRC:%.c=$(BUILD)/%.o), where $(BUILD) (its output dir under
# build/) comes from mk/build.mk, then `include ../mk/rules.mk`. ish/ uses it
#directly. 
# Two components keep their OWN compile rules instead of including this file, by
# design: ports/doom (it compiles from two source dirs, the local dir and src/)
# and the krffs host tools (the FUSE objects need extra cflags the standalone
# tools do not). The single-source test/util programs build with a `%: %.c` rule
# (compile+link in one step) and redirect their -MMD .d into $(BUILD) directly.
# All shapes still share mk/toolchain.mk + mk/deps.mk + mk/build.mk and write
# only under build/; this file is just the common compile-then-link case.

# Objects, the GCC-written .d beside them, and the final binary the component
# links all land under $(BUILD)/ (build/<component>/), so the source tree never
# has a build product.
$(BUILD)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Pull in the GCC-written header prerequisites (-MMD) for this component's
# objects, so editing a header rebuilds exactly the objects that include it.
# Silent on a first build (no .d yet); -MP keeps a deleted header from wedging
# make.
-include $(OBJECTS:.o=.d)
