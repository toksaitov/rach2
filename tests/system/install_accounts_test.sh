#!/bin/sh
# The single-quoted texts below are the functions' own source and account
# lines, not expansions.
# shellcheck disable=SC2016
#
# install_accounts_test.sh: rach2-install keeps the image's system accounts
# beside the machine's.
#
# `add_lines` and `keep_image_accounts` are taken from utils/rach2-install
# whole and run on account files in a private directory. Each row gives them
# an image's files and a machine's: the image's `sshd`, id 74, must be
# appended with its group and its shadow line, and its `test`, id 1000, and
# `demo`, id 1002, which the machine does not use, must not; running them
# again must add nothing; an image account whose id the machine has, written
# as 074, or whose name it has, and an image with no account files, must add
# nothing; an image that has `sshd` twice, and another account with its id,
# must add it once; a machine file whose last line has no newline must keep
# that line whole; an empty machine group file must get the image's system
# groups, root's and `sshd`'s; an image with no shadow line for an
# account it adds must leave the machine's shadow file as it was; an
# image's system group that no new account comes with, as the GNU root's
# `crontab` comes, must be added alone; and a system account the machine has
# by the image's name and id, with no shadow line, `nobody`'s of id 65534
# among them, must get the image's. With --mutate each fault below is put
# into the copy of the functions and must fail its row; the rows run again
# on the functions as they are at the end.
#
# --texts puts each fault into the copy and runs no row.
#
# Usage: sh tests/system/install_accounts_test.sh [--mutate | --texts]
#
set -u

case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    --texts) mutate=2 ;;
    *) echo 'install-accounts-test: usage: install_accounts_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "${1:-}" != --texts ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" install-accounts-test "$repo/tests/system/install_accounts_test.sh" "$@"
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/install-accounts.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

for function in add_lines keep_image_accounts; do
    awk -v f="$function" '$0 == f "() {" { copy = 1 } copy { print } copy && /^\}$/ { exit }' \
        "$repo/utils/rach2-install" > "$work/$function.sh"
    grep -q '^}$' "$work/$function.sh" || {
        echo "install-accounts-test: $function is not in utils/rach2-install" >&2
        exit 1
    }
done
cat "$work/add_lines.sh" "$work/keep_image_accounts.sh" > "$work/function.saved"
cp "$work/function.saved" "$work/function.sh"

failures=0
# fail ROW: report that ROW failed and count it.
fail() {
    echo "install-accounts-test: failed: $1"
    failures=$((failures + 1))
}

# files DIR PASSWD GROUP SHADOW: DIR's account files, each a text with a
# literal \n between lines; an empty text leaves the file out.
files() {
    mkdir -p "$1/etc"
    rm -f "$1/etc/passwd" "$1/etc/group" "$1/etc/shadow"
    [ -z "$2" ] || printf '%b' "$2" > "$1/etc/passwd"
    [ -z "$3" ] || printf '%b' "$3" > "$1/etc/group"
    [ -z "$4" ] || printf '%b' "$4" > "$1/etc/shadow"
}

# keep IMAGE DEST: the functions run in a shell of their own, as the
# installer runs them.
keep() {
    sh -c '. "$1"; keep_image_accounts "$2" "$3"' keep "$work/function.sh" "$1" "$2"
}

# unchanged IMAGE DEST: whether a run leaves DEST as it was.
unchanged() {
    rm -rf "$work/before"
    cp -R "$2" "$work/before"
    keep "$1" "$2" && diff -r "$work/before" "$2" > /dev/null
}

image_passwd='root:x:0:0:root:/root:/bin/sh\ntest:x:1000:1000:test:/home/test:/bin/sh\nsshd:x:74:74::/var/empty:/usr/bin/false\ndemo:x:1002:1002:demo:/home/demo:/bin/sh\n'
image_group='root:x:0:\ntest:x:1000:\nsshd:x:74:\n'
image_shadow='root:*:0:0:99999:7:::\ntest:*:0:0:99999:7:::\nsshd:!:0:0:99999:7:::\n'
machine_passwd='root:x:0:0:root:/root:/bin/sh\nowner:x:1001:1001:owner:/home/owner:/bin/sh\n'
machine_group='root:x:0:\nowner:x:1001:\n'
machine_shadow='root:$6$a:0:0:99999:7:::\nowner:$6$b:0:0:99999:7:::\n'

# Run every row on the copy of the functions.
rows() {
    files "$work/image" "$image_passwd" "$image_group" "$image_shadow"
    files "$work/dest" "$machine_passwd" "$machine_group" "$machine_shadow"
    if keep "$work/image" "$work/dest" &&
        [ "$(cat "$work/dest/etc/passwd")" = "$(printf '%b' "${machine_passwd}sshd:x:74:74::/var/empty:/usr/bin/false")" ] &&
        [ "$(cat "$work/dest/etc/group")" = "$(printf '%b' "${machine_group}sshd:x:74:")" ] &&
        [ "$(cat "$work/dest/etc/shadow")" = "$(printf '%b' "${machine_shadow}sshd:!:0:0:99999:7:::")" ]; then
        :
    else
        fail "the image's sshd is appended with its group and shadow line, and nothing else is"
    fi
    unchanged "$work/image" "$work/dest" || fail "a second run adds nothing"

    files "$work/dest" "${machine_passwd}other:x:074:74::/:/bin/sh\n" "$machine_group" "$machine_shadow"
    unchanged "$work/image" "$work/dest" ||
        fail "an image account whose id the machine has is not added"

    files "$work/dest" "${machine_passwd}sshd:x:80:80::/:/bin/sh\n" "$machine_group" "$machine_shadow"
    unchanged "$work/image" "$work/dest" ||
        fail "an image account whose name the machine has is not added"

    files "$work/image" "" "" ""
    files "$work/dest" "$machine_passwd" "$machine_group" "$machine_shadow"
    unchanged "$work/image" "$work/dest" || fail "an image with no account files adds nothing"

    files "$work/image" "${image_passwd}sshd:x:74:74::/var/empty:/usr/bin/false\nsshd2:x:74:74::/:/bin/sh\n" \
        "${image_group}sshd:x:74:\n" "${image_shadow}sshd:!:0:0:99999:7:::\n"
    files "$work/dest" "$machine_passwd" "$machine_group" "$machine_shadow"
    if ! keep "$work/image" "$work/dest" || [ "$(grep -c ':74:' "$work/dest/etc/passwd")" != 1 ] ||
        [ "$(grep -c '^sshd:' "$work/dest/etc/group")" != 1 ] ||
        [ "$(grep -c '^sshd:' "$work/dest/etc/shadow")" != 1 ]; then
        fail "an account the image has twice, or with another's id, is added once"
    fi

    files "$work/image" "$image_passwd" "$image_group" "$image_shadow"
    files "$work/dest" 'root:x:0:0:root:/root:/bin/sh\nowner:x:1001:1001:owner:/home/owner:/bin/sh' \
        "$machine_group" "$machine_shadow"
    if ! keep "$work/image" "$work/dest" ||
        ! grep -qx 'owner:x:1001:1001:owner:/home/owner:/bin/sh' "$work/dest/etc/passwd" ||
        ! grep -qx 'sshd:x:74:74::/var/empty:/usr/bin/false' "$work/dest/etc/passwd"; then
        fail "a machine file whose last line has no newline keeps that line whole"
    fi

    files "$work/dest" "$machine_passwd" "" "$machine_shadow"
    : > "$work/dest/etc/group"
    if ! keep "$work/image" "$work/dest" ||
        [ "$(cat "$work/dest/etc/group")" != "$(printf 'root:x:0:\nsshd:x:74:')" ]; then
        fail "an empty machine group file gets the image's system groups"
    fi

    files "$work/image" "$image_passwd" "$image_group" 'root:*:0:0:99999:7:::\n'
    files "$work/dest" "$machine_passwd" "$machine_group" "$machine_shadow"
    if ! keep "$work/image" "$work/dest" ||
        [ "$(cat "$work/dest/etc/shadow")" != "$(printf '%b' "$machine_shadow")" ] ||
        ! grep -q '^sshd:' "$work/dest/etc/passwd"; then
        fail "an image with no shadow line for an account it adds leaves the shadow file as it was"
    fi

    files "$work/image" "$image_passwd" "${image_group}crontab:x:63:\n" "$image_shadow"
    files "$work/dest" "${machine_passwd}sshd:x:74:74::/var/empty:/usr/bin/false\n" \
        "${machine_group}sshd:x:74:\n" "${machine_shadow}sshd:!:0:0:99999:7:::\n"
    if ! keep "$work/image" "$work/dest" ||
        [ "$(cat "$work/dest/etc/group")" != "$(printf '%b' "${machine_group}sshd:x:74:\ncrontab:x:63:")" ] ||
        [ "$(cat "$work/dest/etc/shadow")" != "$(printf '%b' "${machine_shadow}sshd:!:0:0:99999:7:::")" ]; then
        fail "an image's system group that no new account comes with is added alone"
    fi

    files "$work/image" "${image_passwd}nobody:x:65534:65534::/:/bin/false\n" "$image_group" \
        "${image_shadow}nobody:!:::::::\n"
    files "$work/dest" \
        "${machine_passwd}sshd:x:74:74::/var/empty:/usr/bin/false\nnobody:x:65534:65534::/:/bin/false\n" \
        "${machine_group}sshd:x:74:\n" "$machine_shadow"
    if ! keep "$work/image" "$work/dest" ||
        [ "$(cat "$work/dest/etc/shadow")" != "$(printf '%b' "${machine_shadow}sshd:!:0:0:99999:7:::\nnobody:!:::::::")" ] ||
        [ "$(grep -c '^sshd:' "$work/dest/etc/passwd")" != 1 ]; then
        fail "a system account the machine has without a shadow line gets the image's"
    fi
}

# fault NAME OLD NEW EXPECT: the functions with the first OLD replaced by NEW
# must fail the row named EXPECT.
fault() {
    cp "$work/function.saved" "$work/function.sh"
    python3 - "$work/function.sh" "$2" "$3" << 'PY' || exit 1
import sys
path, old, new = sys.argv[1:4]
text = open(path).read()
assert old in text, old
open(path, 'w').write(text.replace(old, new, 1))
PY
    [ "$mutate" -ne 2 ] || return 0
    before=$failures
    out=$(rows)
    failures=$before
    case $out in
        *"failed: $4"*) echo "install-accounts-test: result: $1 fails its row" ;;
        *) echo "install-accounts-test: fault passed: $1" >&2; failures=$((failures + 1)) ;;
    esac
}

if [ "$mutate" -ge 1 ]; then
    sshd_row="the image's sshd is appended with its group and shadow line, and nothing else is"
    fault uid-bound '$3 + 0 < 1000 && !($1 in name)' '!($1 in name)' "$sshd_row"
    fault uid-inclusive '$3 + 0 < 1000 && !($1 in name)' '$3 + 0 <= 1000 && !($1 in name)' "$sshd_row"
    fault id-taken '!($1 in name) && !(($3 + 0) in id)' '!($1 in name)' \
        'an image account whose id the machine has is not added'
    fault id-as-text 'name[$1] = 1; id[$3 + 0] = 1; next' 'name[$1] = 1; id[$3] = 1; next' \
        'an image account whose id the machine has is not added'
    fault twice 'print; name[$1] = 1; id[$3 + 0] = 1' 'print' \
        "an account the image has twice, or with another's id, is added once"
    fault no-newline 'if [ -s "$1" ] && [ -n "$(tail -c 1 "$1")" ]; then' 'if false; then' \
        'a machine file whose last line has no newline keeps that line whole'
    fault empty-file 'FILENAME == ARGV[1] { name[$1] = 1; id[$3 + 0] = 1; next }
            FILENAME == ARGV[2] { uid' 'NR == FNR { name[$1] = 1; id[$3 + 0] = 1; next }
            FILENAME == ARGV[2] { uid' "an empty machine group file gets the image's system groups"
    fault group-left 'add_lines "$2/etc/group" "$groups" || return 1' ':' "$sshd_row"
    fault shadow-left 'add_lines "$2/etc/shadow" "$lines"' ':' "$sshd_row"
    fault group-needs-account '!(($3 + 0) in refused) {' '!(($3 + 0) in refused) && ($1 in uid) {' \
        "an image's system group that no new account comes with is added alone"
    fault group-of-refused '!(($3 + 0) in refused) {' '1 {' \
        'an image account whose id the machine has is not added'
    fault shadow-only-added \
        'FILENAME == ARGV[1] { if ($3 + 0 < 1000 || $3 + 0 == 65534) uid[$1] = $3 + 0; next }' \
        'FILENAME == ARGV[1] { next }' \
        'a system account the machine has without a shadow line gets the image'"'"'s'
    fault shadow-any-name 'if (($1 in uid) && uid[$1] == $3 + 0) same[$1] = 1' 'same[$1] = 1' \
        'an image account whose name the machine has is not added'
    fault shadow-no-nobody 'if ($3 + 0 < 1000 || $3 + 0 == 65534) uid' 'if ($3 + 0 < 1000) uid' \
        'a system account the machine has without a shadow line gets the image'"'"'s'
    cp "$work/function.saved" "$work/function.sh"
fi
if [ "$mutate" -eq 2 ]; then
    echo 'install-accounts-test: texts: every fault is once in its source'
    exit 0
fi

rows
if [ "$failures" -ne 0 ]; then
    echo "install-accounts-test: result: fail: $failures"
    exit 1
fi
echo 'install-accounts-test: result: pass: the image system accounts the machine lacks are kept'
