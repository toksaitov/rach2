# Exceptions to the checks

Every check in this tree is meant to be green with nothing suppressed. This file records the remainder: a category a check is run without, with the reason the check is wrong about this code, and where the individual exceptions live. An entry here is a claim to be broken: a reader who finds the reason false removes the exception and fixes what the check then finds.

## Categories left out

* sparse `-Wnon-pointer-null`: the kernel writes `0` for a null pointer in several hundred places, a style older than the check. The check is run for the address-space annotations, which a literal zero does not touch, and the zeros are left as they are: each is a plain zero the reader takes as null, and a change would be churn without a fault.
* sparse `-Wdecl`: sparse asks for `static` on every function and variable without a prior declaration. The kernel's own `-Wmissing-prototypes` under `-Werror` already requires a prior declaration of every function that is not `static`, and a variable a header declares is what the linker joins. The two overlap, and GCC's is the one under the build.
* GCC `-Wconversion -Wsign-conversion` and `-Wunused-macros`: rejected as warning flags, with the reasons beside the flag list in `mk/warnings.mk`.

## Individual exceptions

* cppcheck: `tools/verify/cppcheck.suppress`, one entry per check and file, each with its reason in the comment above it.
* The prose checks: the register rule's own text, the checker's word lists and its fixtures are the only places the banned terms may appear, as `AGENTS.md` says.
