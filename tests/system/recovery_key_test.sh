#!/bin/sh
# recovery_key_test.sh: check `make recovery-key`, the root Makefile's
# recipe that composes the recovery system's authorized keys, and the KRFFS
# image that stages them, with no guest and no real keys.
#
# The recipe must keep the default key and a key file a person put in its
# place, add the keys of RECOVERY_EXTRA_KEYS so that the image trusts both
# the builder's key and the operator's, leave an unchanged file alone on a
# second run, and refuse a private key, a missing, an empty, a malformed or
# a mixed extra file without changing the keys.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO/tools/etude/hold.sh"
etude_hold "$REPO" recovery-key-test "$REPO/tests/system/recovery_key_test.sh" "$@"
[ "$#" = 0 ] || { echo "recovery-key-test: usage: recovery_key_test.sh" >&2; exit 2; }
mkdir -p "$REPO/build"
WORK=
keep=

# Remove the work directory, unless the test failed after printing its path,
# which sets `keep`; a signal clears `keep`.
cleanup() {
    exit_status=$?
    [ -n "$WORK" ] || return 0
    if [ "$exit_status" -eq 0 ] || [ -z "$keep" ]; then
        rm -rf "$WORK"
    fi
}
trap cleanup 0
trap 'keep=; exit 130' INT
trap 'keep=; exit 143' TERM HUP
WORK=$(mktemp -d "$REPO/build/recovery-key-test.XXXXXX")
echo "recovery-key-test: logs: $WORK"
keep=1
mkdir "$WORK/krffs" "$WORK/bin"
cp "$REPO/krffs/gen_recovery_key.sh" "$WORK/krffs/gen_recovery_key.sh"

# Report $* with the logs' directory and exit 1.
fail() {
    echo "recovery-key-test: failed: $*; logs: $WORK" >&2
    exit 1
}

cd "$WORK"
awk '
    /^\.PHONY: recovery-key$/ { copy = 1 }
    /^# `make no-recovery-key`/ { copy = 0 }
    copy
' "$REPO/Makefile" > Makefile
cp "$REPO/krffs/gen_demo_key.sh" krffs/
sh krffs/gen_demo_key.sh > credentials.log 2>&1
ssh-keygen -q -t ed25519 -N '' -C operator -f operator
ssh-keygen -y -f krffs/rach2_demo_ed25519 > builder.pub
ssh-keygen -y -f operator > operator.expected
awk '{ print $1, $2 }' builder.pub operator.expected > expected.keys
awk '$1 == "optional-file" && $3 == "/root/.ssh/authorized_keys"' \
    "$REPO/krffs/manifests/rach2-recovery.manifest" > keys.manifest
[ "$(wc -l < keys.manifest | tr -d ' ')" = 1 ] || fail 'recovery key directive missing'

for tool in mkfs readfile; do
    case "$tool" in
        mkfs) extra= ;;
        readfile) extra="$REPO/krffs/krffs_reader.c" ;;
    esac
    # shellcheck disable=SC2086
    "${CC:-cc}" -O2 -Wall -Wextra -Werror -std=gnu99 -D_FILE_OFFSET_BITS=64 \
        -I "$REPO/krffs" -o "bin/$tool.krffs" "$REPO/krffs/$tool.krffs.c" \
        "$REPO/krffs/krffs_file_system.c" "$REPO/krffs/krffs_node.c" \
        "$REPO/krffs/krffs_platform.c" "$REPO/krffs/krffs_utilities.c" \
        $extra > "$tool.build.log" 2>&1
done

"${CC:-cc}" -O2 -Wall -Wextra -Werror -std=gnu99 -D_FILE_OFFSET_BITS=64 \
    -I "$REPO/krffs" -o bin/stageimage.krffs "$REPO/krffs/stageimage.krffs.c" \
    "$REPO/krffs/krffs_file_system.c" "$REPO/krffs/krffs_node.c" \
    "$REPO/krffs/krffs_platform.c" "$REPO/krffs/krffs_utilities.c" \
    "$REPO/krffs/krffs_allocator.c" > stager.build.log 2>&1

# check_pair PHASE: compose the keys with the operator's through the recipe,
# stage them into PHASE.krffs through the manifest's directive, and require
# the image to trust both keys.
check_pair() {
    phase=$1
    make recovery-key "RECOVERY_EXTRA_KEYS=$WORK/operator.pub" || return 1
    dd if=/dev/zero of="$phase.krffs" bs=1048576 count=1 2>/dev/null || return 1
    bin/mkfs.krffs "$phase.krffs" || return 1
    bin/stageimage.krffs keys.manifest "$phase.krffs" || return 1
    bin/readfile.krffs "$phase.krffs" /root/.ssh/authorized_keys "$phase.keys" || return 1
    awk 'NF { print $1, $2 }' "$phase.keys" > "$phase.identities" || return 1
    if ! cmp -s expected.keys "$phase.identities"; then
        echo "recovery-key-test: failed: image does not trust both keys" >&2
        return 1
    fi
}

make recovery-key > default.log 2>&1
cmp -s krffs/authorized_keys krffs/recovery_authorized_keys || fail 'default key changed'
cp operator.pub krffs/recovery_authorized_keys
make recovery-key >> default.log 2>&1
cmp -s operator.pub krffs/recovery_authorized_keys || fail 'custom key overwritten'
check_pair pair > pair.log 2>&1 || { cat pair.log >&2; fail 'the builder and operator keys'; }
echo 'recovery-key-test: passed: image trusts builder and operator'
touch -t 200101010000 krffs/recovery_authorized_keys
cp -p krffs/recovery_authorized_keys unchanged
make recovery-key "RECOVERY_EXTRA_KEYS=$WORK/operator.pub" > repeat.log 2>&1
cmp -s unchanged krffs/recovery_authorized_keys || fail 'repeated composition changed keys'
[ ! krffs/recovery_authorized_keys -nt unchanged ] || fail 'unchanged keys republished'
for invalid in operator missing empty malformed mixed; do
    : > empty
    printf 'ssh-ed25519 invalid\n' > malformed
    cat operator.pub malformed > mixed
    if make recovery-key "RECOVERY_EXTRA_KEYS=$WORK/$invalid" > "$invalid.log" 2>&1; then
        fail "accepted $invalid extra file"
    fi
    cmp -s unchanged krffs/recovery_authorized_keys || fail 'failed update changed keys'
done
echo 'recovery-key-test: passed: defaults, preservation, repetition and invalid input'

