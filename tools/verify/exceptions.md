# Exceptions to the checks

Every check in this tree is meant to be green with nothing suppressed. This file records the remainder: a category a check is run without, with the reason the check is wrong about this code, and where the individual exceptions live. An entry here is a claim to be broken: a reader who finds the reason false removes the exception and fixes what the check then finds.

## Categories left out

* GCC `-Wconversion -Wsign-conversion` and `-Wunused-macros`: rejected as warning flags, with the reasons beside the flag list in `mk/warnings.mk`.

The historical `sparse` case checks the user-pointer contract with Clang and a parsed cast audit, as [Native source judges](../setup/source-tools.md) describes. It no longer runs Sparse or uses its `-Wno-non-pointer-null` and `-Wno-decl` exceptions. Literal zero remains a valid C null pointer; the kernel's existing `-Wmissing-prototypes` still checks declarations.

## Individual exceptions

* cppcheck: `tools/verify/cppcheck.suppress`, one entry per check and file, each with its reason in the comment above it.
* The prose checks: the register rule's own text, the checker's word lists and its fixtures are the only places the banned terms may appear, as `AGENTS.md` says.
