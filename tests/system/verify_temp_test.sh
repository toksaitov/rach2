#!/bin/sh
# Execute check.sh's directory setup and ABI write against planted links.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/verify-temp-test.XXXXXX")
trap 'rm -rf "$WORK"' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
fail() { echo "verify-temp-test: $*" >&2; exit 1; }
mkdir "$WORK/rach2_verify"
chmod 0777 "$WORK/rach2_verify"
printf 'keep this file\n' > "$WORK/victim"
cp "$WORK/victim" "$WORK/expected"
ln -s "$WORK/victim" "$WORK/rach2_verify/_abi.c"
ln -s "$WORK/victim" "$WORK/rach2_verify/verify_build.log"
# Redirect the old fixed /tmp name into this test's private directory.
printf 'make() { :; }\n' > "$WORK/setup.sh"
awk '
    /^VDIR=/ { setup = 1 }
    /^LIBC=/ { setup = 0 }
    setup { gsub("/tmp/rach2_verify", "${TMPDIR}/rach2_verify"); print }
    /^if make -C rach2 / { print; print "    :; fi" }
    /printf.*int main\(void\).*VDIR/ { print }
' "$REPO/tools/verify/check.sh" >> "$WORK/setup.sh"
# shellcheck disable=SC2016
printf 'printf "%%s\\n" "$VDIR" > "$1"\n' >> "$WORK/setup.sh"
for attempt in 1 2; do
    TMPDIR="$WORK" sh "$WORK/setup.sh" "$WORK/path.$attempt"
    cmp "$WORK/expected" "$WORK/victim" || fail "temporary write followed the planted symlink"
    directory=$(cat "$WORK/path.$attempt")
    [ -d "$directory" ] || fail "logs directory was not retained"
    [ -f "$directory/_abi.c" ] || fail "abi fixture was not written"
    mode=$(find "$directory" -prune -type d -perm 0700 -print)
    [ "$mode" = "$directory" ] || fail "logs directory is not private"
done
[ "$(cat "$WORK/path.1")" != "$(cat "$WORK/path.2")" ] || fail "runs share a directory"
if TMPDIR="$WORK/missing" sh "$WORK/setup.sh" "$WORK/no-path" > "$WORK/failure" 2>&1; then
    fail "directory creation failure was ignored"
fi
# The host suite and the other verify scripts keep their files in the
# tree's build directory, not in a directory of /tmp that every tree and
# every user shares.
for script in tests/system/suites/host.sh tools/verify/analyze.sh tools/verify/coverage.sh; do
    ! grep -q '/tmp/rach2_verify' "$REPO/$script" || fail "$script writes to the shared /tmp/rach2_verify"
done
echo "verify-temp-test: passed: private, distinct, retained directories and failed creation"
