#!/bin/sh
# Prove the recovery key rows of the installer suite with faults in a private
# tree.
#
# Usage: sh tests/system/recovery_disk_key_mutate.sh
#
# Rows 6b and 6c of tests/system/suites/installer.sh install a disk with a key
# for its recovery system and one without, boot each disk's own recovery
# system with an image that has no key of its own, and require the first to
# let root in with the key the install placed and the second to answer
# nothing. The tree is copied, the test stick built there, and the rows run
# alone with `recoverykey`: as the tree is, where they must pass; with the
# recovery init's disk key read off (it asks the boot line for a slot no boot
# names), where row 6b must fail; restored, where they must pass again with
# the init the first phase had; with the installer's copy of the key taken
# out, where row 6b must fail on the key file; and restored again. The trap
# stops the row's process group and restores both sources.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ ! -d build/.run.lock ] || {
    echo 'recovery-disk-key-mutation: another suite owns this tree' >&2; exit 1;
}
mkdir -p build
work=$(mktemp -d "$repo/build/recovery-disk-key-proof.XXXXXX")
echo "recovery-disk-key-mutation: logs: $work"
rsync -a --exclude=/.git --exclude=/build/.run.lock --exclude=/build/images \
    --exclude='/build/machine-*' --exclude='/build/*-proof.*' "$repo/" "$work/tree/"
cd "$work/tree"
init_source="$work/tree/utils/init.c"
installer_source="$work/tree/utils/rach2-install"
cp "$init_source" "$work/init.saved"
cp "$installer_source" "$work/installer.saved"
child=

stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

restore()
{
    cp "$work/init.saved" "$init_source"
    cp "$work/installer.saved" "$installer_source"
    touch "$init_source" "$installer_source"
}

cleanup()
{
    stop_child
    restore
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Change one private source where the old text occurs exactly once, and touch
# it, since a copy back within the second of the last build reads as current.
mutate()
{
    python3 - "$1" "$2" "$3" <<'PY'
import pathlib
import sys

path, old, new = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit("recovery-disk-key-mutation: pattern is not unique: " + old)
path.write_text(text.replace(old, new))
PY
    touch "$1"
}

# Each phase rebuilds the recovery init and every image that stages it or the
# installer, from nothing: an image whose staged program changed in the same
# second as its last build reads as current to make. The rows stage their own
# recovery image with no key from the init built here.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    rm -f build/images/rach2-recovery*.krffs build/images/rach2-recovery*.iso \
        build/images/installer-test.disk build/images/install-test-stick.img \
        build/utils/sbin/init-recovery
    make -s utils >"$work/$phase.build.log" 2>&1
    make -s -j8 build/images/install-test-stick.img build/images/installer-test.disk \
        build/tests/system/harness build/tests/system/runutil \
        >>"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    shasum -a 256 "$init_source" "$installer_source" build/utils/sbin/init-recovery \
        build/images/install-test-stick.img >"$work/$phase.artifacts"
    cp build/utils/sbin/init-recovery "$work/$phase.init"
    cp "$installer_source" "$work/$phase.installer"
    build/tests/system/runutil group sh tests/system/suites/installer.sh \
        build/images/install-test-stick.img build/images/installer-test.disk \
        build/tests/system/harness "build/images/recovery-key-$phase" \
        "$(cat krffs/rach2_demo_password)" build/images/install-test-stick.img \
        build/images/installer-test.disk "${RACH2_RECOVERY_KEY_PORT:-2331}" \
        unused krffs/rach2_demo_ed25519 recoverykey >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.log"
    if [ "$status" -ne "$expected" ] || ! grep -Fq "$message" "$work/$phase.log"; then
        echo "recovery-disk-key-mutation: wrong result: $phase status $status" >&2; exit 1
    fi
    echo "recovery-disk-key-mutation: result: $phase status $status"
}

passed='row 6b: root logs in to the recovery system with the key the install placed: OK'
make -s -j8 rach2 utils krffs >"$work/components.build.log" 2>&1
cat "$work/components.build.log"
run_phase fixed 0 "$passed"
grep -Fq 'row 6c: and nothing answers SSH: OK' "$work/fixed.log"

# The recovery init no longer reads the key off its partition.
mutate "$init_source" 'boot_line_has("rach2.slot=recovery")' 'boot_line_has("rach2.slot=never")'
run_phase init-mutated 1 'FAIL: row 6b: the placed key'
if cmp -s "$work/fixed.init" "$work/init-mutated.init"; then
    echo 'recovery-disk-key-mutation: the init fault did not change the init' >&2; exit 1
fi
restore
run_phase init-restored 0 "$passed"
cmp "$work/fixed.init" "$work/init-restored.init"

# The installer no longer copies the key it was given. The dollars are the
# installer's own text.
# shellcheck disable=SC2016
mutate "$installer_source" "        printf '%s\\n' \"\$recovery_key_text\" > \"\$RECOVERY_MNT/\$RECOVERY_KEYS_FILE\" ||" \
    '        : "$recovery_key_text" ||'
run_phase installer-mutated 1 "FAIL: row 6b: the recovery partition's key file holds"
restore
run_phase installer-restored 0 "$passed"
cmp "$work/fixed.installer" "$work/installer-restored.installer"
cmp "$work/fixed.init" "$work/installer-restored.init"
echo 'recovery-disk-key-mutation: result: pass: the init and the installer faults each failed row 6b; both restorations passed'
