# shellcheck shell=sh
# tooling.sh: what a suite sources to run a command its host lacks in etude's
# tooling machine (tools/etude/Readme.md, The tooling machine). REPO must be
# the tree's root. The machine is the one tools/etude/tooling describes, in
# the cache ETUDE_TOOLING_CACHE names or build/tooling, and runs under the
# accelerator RACH2_QEMU_ACCEL names, which tests/run sets.

TOOLING_BIN=$REPO/build/tools/etude/etude-tooling

# tooling ARG...: run etude-tooling with the tree's recipe and cache.
tooling() {
    "$TOOLING_BIN" -r "$REPO/tools/etude/tooling" -C "${ETUDE_TOOLING_CACHE:-$REPO/build/tooling}" "$@"
}

# tooling_made: succeed when the tooling machine is made, so that a job can run.
tooling_made() {
    [ -x "$TOOLING_BIN" ] && tooling check > /dev/null 2>&1
}
