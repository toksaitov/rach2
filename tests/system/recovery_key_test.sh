#!/bin/sh
# Check recovery credentials in a KRFFS image without a guest or real keys.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
case "${1:-}" in
    ''|--mutate) ;;
    *) echo "recovery-key-test: usage: recovery_key_test.sh [--mutate]" >&2; exit 2 ;;
esac
mkdir -p "$REPO/build"
WORK=$(mktemp -d "$REPO/build/recovery-key-test.XXXXXX")
echo "recovery-key-test: logs: $WORK"
mkdir "$WORK/krffs" "$WORK/bin"
cp "$REPO/krffs/gen_recovery_key.sh" "$WORK/krffs/gen_recovery_key.sh"
cp "$REPO/krffs/stageimage.krffs.c" "$WORK/stageimage.krffs.c"
cp "$WORK/krffs/gen_recovery_key.sh" "$WORK/key.source"
cp "$WORK/stageimage.krffs.c" "$WORK/stager.source"

# /** Restore the private sources after each synchronous command has exited. */
restore() {
    cp "$WORK/key.source" "$WORK/krffs/gen_recovery_key.sh"
    cp "$WORK/stager.source" "$WORK/stageimage.krffs.c"
    touch "$WORK/krffs/gen_recovery_key.sh" "$WORK/stageimage.krffs.c"
}
trap restore 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP

# /** Report the failed assertion and retain its artifacts. */
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

# /**
#  * Compile the stager from the private source without an incremental cache.
#  */
build_stager() {
    "${CC:-cc}" -O2 -Wall -Wextra -Werror -std=gnu99 -D_FILE_OFFSET_BITS=64 \
        -I "$REPO/krffs" -o bin/stageimage.krffs stageimage.krffs.c \
        "$REPO/krffs/krffs_file_system.c" "$REPO/krffs/krffs_node.c" \
        "$REPO/krffs/krffs_platform.c" "$REPO/krffs/krffs_utilities.c" \
        "$REPO/krffs/krffs_allocator.c"
}

# /**
#  * Build an image through the production recipe and compare both identities.
#  */
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

build_stager > stager.build.log 2>&1
cp bin/stageimage.krffs fixed.stager
make recovery-key > default.log 2>&1
cmp -s krffs/authorized_keys krffs/recovery_authorized_keys || fail 'default key changed'
cp operator.pub krffs/recovery_authorized_keys
make recovery-key >> default.log 2>&1
cmp -s operator.pub krffs/recovery_authorized_keys || fail 'custom key overwritten'
check_pair fixed > fixed.log 2>&1
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

if [ "${1:-}" = --mutate ]; then
    # shellcheck disable=SC2016
    sed 's/cat "\$TMP.line" >> "\$TMP"/: "\$TMP.line"/' key.source \
        > krffs/gen_recovery_key.sh
    if check_pair omitted > omitted.log 2>&1; then
        fail 'omitted operator key passed'
    fi
    grep -q 'image does not trust both keys' omitted.log || fail 'unexpected omission failure'
    restore
    build_stager > restored.build.log 2>&1
    check_pair restored > restored.log 2>&1
    cmp -s fixed.krffs restored.krffs || fail 'restored image differs'
    echo 'recovery-key-test: passed: omitted operator fails; restored image passes'

    awk '
        /ok = stage_file\(file_system, path, mode, uid, gid, data, len\);/ {
            print "        if (strcmp(path, \"/root/.ssh/authorized_keys\") == 0) {"
            print "            char *end = memchr(data, '\''\\n'\'', len);"
            print "            if (end != NULL) len = (size_t)(end - data) + 1;"
            print "        }"
        }
        { print }
    ' stager.source > stageimage.krffs.c
    build_stager > truncated.build.log 2>&1
    cp bin/stageimage.krffs truncated.stager
    if cmp -s fixed.stager truncated.stager; then
        fail 'stager mutation did not change the executable'
    fi
    if check_pair truncated > truncated.log 2>&1; then
        fail 'compiled key truncation passed'
    fi
    grep -q 'image does not trust both keys' truncated.log || fail 'unexpected truncation failure'
    restore
    build_stager > rebuilt.build.log 2>&1
    cp bin/stageimage.krffs rebuilt.stager
    cmp -s fixed.stager rebuilt.stager || fail 'rebuilt stager differs'
    check_pair rebuilt > rebuilt.log 2>&1
    cmp -s fixed.krffs rebuilt.krffs || fail 'rebuilt image differs'
    echo 'recovery-key-test: passed: compiled truncation fails; restored image passes'
fi
