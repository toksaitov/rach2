#!/bin/sh
# The single-quoted texts below are the function's own source and account
# lines, not expansions.
# shellcheck disable=SC2016
#
# install_accounts_test.sh: rach2-install keeps the image's system accounts
# beside the machine's.
#
# `keep_image_accounts` is taken from utils/rach2-install whole and run on
# account files in a private directory. Each row gives it an image's files
# and a machine's: the image's `sshd`, id 74, must be appended with its group
# and its shadow line, and its `test`, id 1000, and `demo`, id 1002, which
# the machine does not use, must not; running it again
# must add nothing; an image account whose name or whose id the machine
# already has, and an image with no account files, must add nothing; an
# image with no shadow line for an account it adds must leave the machine's
# shadow file as it was. With --mutate each fault below is put into the copy
# of the function and must fail its row; the rows run again on the function
# as it is at the end.
#
# Usage: sh tests/system/install_accounts_test.sh [--mutate]
#
set -u

case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    *) echo 'install-accounts-test: usage: install_accounts_test.sh [--mutate]' >&2; exit 2 ;;
esac

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/install-accounts.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

awk '/^keep_image_accounts\(\) \{$/ { copy = 1 } copy { print } copy && /^\}$/ { exit }' \
    "$repo/utils/rach2-install" > "$work/function.sh"
grep -q '^}$' "$work/function.sh" || {
    echo 'install-accounts-test: keep_image_accounts is not in utils/rach2-install' >&2
    exit 1
}
cp "$work/function.sh" "$work/function.saved"

failures=0
fail() {
    echo "install-accounts-test: failed: $1"
    failures=$((failures + 1))
}

# files DIR PASSWD GROUP SHADOW: DIR's account files, each line of a text
# given with a literal \n between lines; an empty text leaves the file out.
files() {
    mkdir -p "$1/etc"
    rm -f "$1/etc/passwd" "$1/etc/group" "$1/etc/shadow"
    [ -z "$2" ] || printf '%b' "$2" > "$1/etc/passwd"
    [ -z "$3" ] || printf '%b' "$3" > "$1/etc/group"
    [ -z "$4" ] || printf '%b' "$4" > "$1/etc/shadow"
}

# keep IMAGE DEST: the function run in a shell of its own, as the installer
# runs it.
keep() {
    sh -c '. "$1"; keep_image_accounts "$2" "$3"' keep "$work/function.sh" "$1" "$2"
}

image_passwd='root:x:0:0:root:/root:/bin/sh\ntest:x:1000:1000:test:/home/test:/bin/sh\nsshd:x:74:74::/var/empty:/usr/bin/false\ndemo:x:1002:1002:demo:/home/demo:/bin/sh\n'
image_group='root:x:0:\ntest:x:1000:\nsshd:x:74:\n'
image_shadow='root:*:0:0:99999:7:::\ntest:*:0:0:99999:7:::\nsshd:!:0:0:99999:7:::\n'
machine_passwd='root:x:0:0:root:/root:/bin/sh\nowner:x:1000:1000:owner:/home/owner:/bin/sh\n'
machine_group='root:x:0:\nowner:x:1000:\n'
machine_shadow='root:$6$a:0:0:99999:7:::\nowner:$6$b:0:0:99999:7:::\n'

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
    cp -R "$work/dest" "$work/once"
    if ! keep "$work/image" "$work/dest" || ! diff -r "$work/once" "$work/dest" > /dev/null; then
        fail "a second run adds nothing"
    fi
    rm -rf "$work/once"

    files "$work/dest" "${machine_passwd}other:x:74:74::/:/bin/sh\n" "$machine_group" "$machine_shadow"
    cp -R "$work/dest" "$work/before"
    if ! keep "$work/image" "$work/dest" || ! diff -r "$work/before" "$work/dest" > /dev/null; then
        fail "an image account whose id the machine has is not added"
    fi
    rm -rf "$work/before"

    files "$work/dest" "${machine_passwd}sshd:x:80:80::/:/bin/sh\n" "$machine_group" "$machine_shadow"
    cp -R "$work/dest" "$work/before"
    if ! keep "$work/image" "$work/dest" || ! diff -r "$work/before" "$work/dest" > /dev/null; then
        fail "an image account whose name the machine has is not added"
    fi
    rm -rf "$work/before"

    files "$work/image" "" "" ""
    files "$work/dest" "$machine_passwd" "$machine_group" "$machine_shadow"
    cp -R "$work/dest" "$work/before"
    if ! keep "$work/image" "$work/dest" || ! diff -r "$work/before" "$work/dest" > /dev/null; then
        fail "an image with no account files adds nothing"
    fi
    rm -rf "$work/before"

    files "$work/image" "$image_passwd" "$image_group" 'root:*:0:0:99999:7:::\n'
    files "$work/dest" "$machine_passwd" "$machine_group" "$machine_shadow"
    if ! keep "$work/image" "$work/dest" ||
        [ "$(cat "$work/dest/etc/shadow")" != "$(printf '%b' "$machine_shadow")" ] ||
        ! grep -q '^sshd:' "$work/dest/etc/passwd"; then
        fail "an image with no shadow line for an account it adds leaves the shadow file as it was"
    fi
}

# fault NAME OLD NEW EXPECT: the function with OLD replaced by NEW must fail
# the row named EXPECT.
fault() {
    cp "$work/function.saved" "$work/function.sh"
    python3 - "$work/function.sh" "$2" "$3" << 'PY' || exit 1
import sys
path, old, new = sys.argv[1:4]
text = open(path).read()
assert text.count(old) == 1, old
open(path, 'w').write(text.replace(old, new))
PY
    before=$failures
    out=$(rows)
    failures=$before
    case $out in
        *"failed: $4"*) echo "install-accounts-test: result: $1 fails its row" ;;
        *) echo "install-accounts-test: fault passed: $1" >&2; failures=$((failures + 1)) ;;
    esac
}

if [ "$mutate" -eq 1 ]; then
    fault uid-bound '$3 + 0 < 1000 && !($1 in name) && !($3 in id)'"' \"\$2/etc/passwd\"" \
        '!($1 in name) && !($3 in id)'"' \"\$2/etc/passwd\"" \
        "the image's sshd is appended with its group and shadow line, and nothing else is"
    fault id-taken '$3 + 0 < 1000 && !($1 in name) && !($3 in id)'"' \"\$2/etc/passwd\"" \
        '$3 + 0 < 1000 && !($1 in name)'"' \"\$2/etc/passwd\"" \
        'an image account whose id the machine has is not added'
    fault group-left '[ -z "$groups" ] || printf' ': || printf' \
        "the image's sshd is appended with its group and shadow line, and nothing else is"
    fault shadow-left '[ -z "$lines" ] || printf' ': || printf' \
        "the image's sshd is appended with its group and shadow line, and nothing else is"
    cp "$work/function.saved" "$work/function.sh"
fi

rows
if [ "$failures" -ne 0 ]; then
    echo "install-accounts-test: result: fail: $failures"
    exit 1
fi
echo 'install-accounts-test: result: pass: the image system accounts the machine lacks are kept'
