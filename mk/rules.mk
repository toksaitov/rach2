# mk/rules.mk: the compile rule and the dependency include of a component that
# compiles each source to an object under build/ and then links one binary.
#
# A component sets CC, CFLAGS (with $(DEPFLAGS) from mk/deps.mk) and OBJECTS
# as $(SRC:%.c=$(BUILD)/%.o), where $(BUILD) comes from mk/build.mk, then
# includes this file. ish/ is the component that does. The kernel, ports/doom
# (whose sources are in two directories) and the krffs host tools (whose FUSE
# objects take flags the other tools do not) have compile rules of their own,
# and the single-source programs of utils/ and tests/ compile and link in one
# `$(BUILD)/%: %.c` rule that writes the .d into $(BUILD) with -MF. All of
# them write only under build/.

# The object, and the .d GCC writes beside it, go under $(BUILD).
$(BUILD)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# The header prerequisites GCC wrote (-MMD), so that a header edit rebuilds
# the objects that include it. A first build has no .d files, and the include
# is silent without them.
-include $(OBJECTS:.o=.d)
