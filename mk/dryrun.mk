# mk/dryrun.mk: whether this make is a dry run, for the recipes marked `+`.
#
# A recipe with a leading `+` runs even under `-n`, which is how the vendor
# stamps print what they would build instead of building it. Such a recipe has
# to ask whether this is a dry run, and the only thing it can ask is
# `MAKEFLAGS`.
#
# Make puts the short flags of this invocation in the first word of MAKEFLAGS,
# with no leading dash, and every long option in a word of its own: `make -n
# --no-print-directory` gives `n --no-print-directory`, and
# `make --no-print-directory` alone gives `--no-print-directory` as the first
# word. Searching that first word for the letter `n` therefore reads
# `--no-print-directory` as a dry run, which `tests/run` passes on every named
# suite, and the vendor builds were skipped where their outputs were absent.
# The word is taken only when it is not an option, which is what says it holds
# the short flags.
#
# That word is not the whole answer. GNU Make 3.81, the make macOS ships, puts
# the short flag after the long option when both are given: `make -n
# --no-print-directory` gives ` --no-print-directory -n`, where the first word
# is the long option and `-n` is a word of its own with its dash. Reading only
# the first word there would call a real dry run a build and run the vendor
# script it was asked to describe. So the long forms and that detached `-n`
# are matched as whole words as well.
MAKE_SHORT_FLAGS := $(filter-out -%,$(firstword $(MAKEFLAGS)))
DRY_RUN := $(strip $(findstring n,$(MAKE_SHORT_FLAGS)) \
                   $(filter -n --dry-run --just-print --recon,$(MAKEFLAGS)))
