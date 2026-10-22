#!/bin/sh
# nativecheck_test.sh: check tools/verify/nativecheck.sh on a small tree in
# fourteen cases: complete cases pass, and a program with no case and no
# reason, a program with a case and a reason, a reason for a program that is
# not built, a case named for another program, a case named twice, a fixture
# that fits no form, an id, a port or an image that maps lacks, a volume of
# another type than its image, a staged file that is missing, a port mapped
# twice and more volumes than loop devices fail.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=
trap 'rm -rf ${WORK:+"$WORK"}' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rach2-nativecheck-test.XXXXXX")
mkdir -p "$WORK/tests/musl" "$WORK/tests/native"
printf 'PROGS = alpha beta gamma\n' > "$WORK/tests/Makefile"
: > "$WORK/tests/musl/delta.c"
: > "$WORK/tests/musl/helper.c"
printf 'hello\n' > "$WORK/tests/native/hello.txt"
cat > "$WORK/tests/native/maps" <<'EOF'
idmap 1000:61000
portmap 7000:47000
image vol ext2 16
EOF
printf 'gamma two disks of the suite\n' > "$WORK/tests/native/machine"
: > "$WORK/tests/native/probes.cases"
: > "$WORK/tests/native/musl.cases"

# cases TEXT: make TEXT the probes' cases.
cases() {
    printf '%s\n' "$1" > "$WORK/tests/native/probes.cases"
}

# check STATUS NAME: run nativecheck.sh over the tree and require STATUS.
check() {
    result=0
    NATIVECHECK_ROOT="$WORK" sh "$REPO/tools/verify/nativecheck.sh" > "$WORK/log" 2>&1 ||
        result=$?
    if [ "$result" -ne "$1" ]; then
        cat "$WORK/log" >&2
        echo "nativecheck-test: $2: expected $1, got $result" >&2
        exit 1
    fi
}
good='# The cases.
case alpha ids:1000 ports:7000 stage:native/hello.txt -- alpha a
case alpha.vol volume:ext2:vol mountns \
    log -- alpha machine=native
case beta root:ext2:32 pty:hello.txt end:signal:2 env:HOME -- beta'
printf 'case musl/delta stage:musl/helper -- musl/delta\n' > "$WORK/tests/native/musl.cases"
cases "$good"
check 0 'complete cases'
cases "$(printf '%s\n' "$good" | sed '$d')"
check 1 'a program with no case and no reason'
printf 'gamma two disks\nbeta a reason\n' > "$WORK/tests/native/machine"
cases "$good"
check 1 'a program with a case and a reason'
printf 'gamma two disks\nepsilon gone\n' > "$WORK/tests/native/machine"
check 1 'a reason for a program that is not built'
printf 'gamma two disks of the suite\n' > "$WORK/tests/native/machine"
cases "$good
case alpha.x -- beta"
check 1 'a case named for another program'
cases "$good
case beta -- beta"
check 1 'a case named twice'
cases "$good
case alpha.y netns -- alpha"
check 1 'a fixture that fits no form'
cases "$good
case alpha.y ids:1001 -- alpha"
check 1 'an id that maps lacks'
cases "$good
case alpha.y ports:7001 -- alpha"
check 1 'a port that maps lacks'
cases "$good
case alpha.y volume:ext2:other -- alpha"
check 1 'an image that maps lacks'
cases "$good
case alpha.y volume:vfat:vol -- alpha"
check 1 'a volume of another type than its image'
cases "$good
case alpha.y stage:native/absent.txt -- alpha"
check 1 'a staged file that is missing'
cases "$good"
printf 'portmap 7001:47000\n' >> "$WORK/tests/native/maps"
check 1 'a port mapped twice'
sed '$d' "$WORK/tests/native/maps" > "$WORK/maps" && mv "$WORK/maps" "$WORK/tests/native/maps"
cases "$good
case alpha.y root:krffs volume:ext2:vol volume:ext2:vol volume:ext2:vol volume:ext2:vol -- alpha"
check 1 'more volumes than loop devices'
echo "nativecheck-test: passed: 14 cases"
