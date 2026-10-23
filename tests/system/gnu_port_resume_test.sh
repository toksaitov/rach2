#!/bin/sh
#
# gnu_port_resume_test.sh: a stopped `build_port.sh --all` keeps the ports it
# finished, and the next --all of the same key goes on from the next port and
# ends with the root a build that never stopped makes.
#
# The script runs in a scratch tree, with vendor/gnu/build_port.sh, its
# arch.sh and mk/arch.sh copied from this tree, over five stand-in recipes:
# runtime, pkgconf, a, b, which depends on a, and c, which replaces a file of
# a's. Each recipe installs a file or two and adds its name to a log as it
# builds; b fails while the file STOP exists, as a build stopped in it. The
# cross compiler and the host's cc are stand-ins. The rows: an --all that
# never stops gives the root the others are compared with; an --all stopped
# in b, with b cut short in its copy into the root, and run again with the
# same ETUDE_STEP_KEY builds only b and c, gives that root, and leaves no
# list of finished ports; and an --all after a stop starts again from an
# empty root when the key differs, when there is none, when a port was built
# alone since the stop, and when the stage of a finished port is gone.
#
# Usage: sh tests/system/gnu_port_resume_test.sh
#
set -u
unset MAKEFLAGS MFLAGS MAKELEVEL MAKE ETUDE_STEP_KEY ETUDE_STEP_SHIMS RACH2_GNU_OUT

[ "$#" -eq 0 ] || { echo 'gnu-port-resume-test: usage: gnu_port_resume_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-gnu-port-resume-test.XXXXXX") || exit 1

gnu=$work/repo/vendor/gnu
out=$work/out
mkdir -p "$gnu/ports" "$work/repo/mk" "$work/bin" || exit 1
cp "$repo/vendor/gnu/build_port.sh" "$repo/vendor/gnu/arch.sh" "$gnu/" || exit 1
cp "$repo/mk/arch.sh" "$work/repo/mk/" || exit 1
printf '#!/bin/sh\necho x86_64-linux-gnu\n' > "$work/bin/cc"
chmod +x "$work/bin/cc" || exit 1

# recipe NAME DEPENDS INSTALL: write the recipe of NAME, which logs its build
# and runs the shell text INSTALL in its stage.
recipe()
{
    cat > "$gnu/ports/$1.sh" << EOF
VERSION=1
SOURCE=
DEPENDS="$2"
port_build()
{
    echo $1 >> "\$LOG"
    [ ! -e "\$ROOT/usr/bin/$1" ] || echo "$1 saw itself" >> "\$LOG"
    [ "$1" != b ] || [ ! -f "\$STOP" ]
}
port_install()
{
    cd "\$DEST" && $3
}
EOF
}
recipe runtime '' 'mkdir -p lib && echo libc > lib/libc.so'
recipe pkgconf '' 'mkdir -p usr/bin && echo pkgconf > usr/bin/pkgconf'
recipe a '' 'mkdir -p usr/bin usr/share && echo a > usr/bin/a && echo a > usr/share/common'
recipe b a 'mkdir -p usr/bin && echo b > usr/bin/b'
recipe c '' 'mkdir -p usr/bin usr/share && echo c > usr/bin/c && echo c > usr/share/common'

PATH=$work/bin:$PATH
RACH2_GNU_OUT=$out
LOG=$work/log
STOP=$work/stop
export PATH RACH2_GNU_OUT LOG STOP

failed=0

fail()
{
    echo "gnu-port-resume-test: $1" >&2
    failed=1
}

# fresh: an empty build tree with the stand-in cross compiler.
fresh()
{
    rm -rf "$out" "$LOG" "$STOP"
    mkdir -p "$out/cross/bin" || exit 1
    for t in gcc g++ c++ cpp strip; do
        printf '#!/bin/sh\nexit 0\n' > "$out/cross/bin/i686-linux-gnu-$t"
        chmod +x "$out/cross/bin/i686-linux-gnu-$t" || exit 1
    done
}

# ports KEY ARG...: run build_port.sh with the key KEY, or none when it is
# empty, and a new log; its status.
ports()
{
    key=$1
    shift
    : > "$LOG"
    if [ -n "$key" ]; then
        ETUDE_STEP_KEY=$key sh "$gnu/build_port.sh" "$@" > "$work/out.txt" 2>&1
    else
        sh "$gnu/build_port.sh" "$@" > "$work/out.txt" 2>&1
    fi
}

# built: the ports the last run built, on one line.
built()
{
    tr '\n' ' ' < "$LOG" | sed 's/ $//'
}

# stopped: a fresh tree whose --all of key k1 stopped in b, with b cut short
# as it copied its stage into the root.
stopped()
{
    fresh
    : > "$STOP"
    if ports k1 --all; then
        fail "the --all that b stops succeeded"
        return
    fi
    rm -f "$STOP"
    mkdir -p "$out/stage/b/usr/bin" "$out/root/usr/bin"
    echo b > "$out/stage/b/usr/bin/b"
    echo ./usr/bin/b > "$out/stage/b.files"
    echo b > "$out/root/usr/bin/b"
}

# again ROW WANT KEY ARG...: after `stopped`, run build_port.sh, and require
# it to succeed, to have built WANT and to leave the root of a build that
# never stopped and no list.
again()
{
    row=$1
    want=$2
    shift 2
    if ! ports "$@"; then
        fail "$row: the run failed:"
        sed 's/^/    /' "$work/out.txt" >&2
        return
    fi
    if [ "$(built)" != "$want" ]; then
        fail "$row: it built \"$(built)\", not \"$want\""
    elif ! diff -r "$work/ref" "$out/root" > "$work/diff.txt" 2>&1; then
        fail "$row: its root differs from that of a build that never stopped:"
        sed 's/^/    /' "$work/diff.txt" >&2
    elif [ -e "$out/work/ports.done" ]; then
        fail "$row: it left the list of finished ports"
    else
        echo "gnu-port-resume-test: $row: ok: built $want"
    fi
}

fresh
if ! ports k1 --all || [ "$(built)" != 'runtime pkgconf a b c' ]; then
    fail "whole: the --all that never stops failed or built \"$(built)\":"
    sed 's/^/    /' "$work/out.txt" >&2
    exit 1
fi
cp -a "$out/root" "$work/ref" || exit 1
[ ! -e "$out/work/ports.done" ] || fail "whole: it left the list of finished ports"

stopped
if [ "$(printf 'key k1\nruntime\npkgconf\na\n')" != "$(cat "$out/work/ports.done" 2> /dev/null)" ]; then
    fail "stopped: the list of finished ports is not the key, runtime, pkgconf and a"
fi
again resume 'b c' k1 --all
grep -q 'resume: keeping the 3 ports' "$work/out.txt" || fail "resume: it did not say that it kept 3 ports"

stopped
again 'another key' 'runtime pkgconf a b c' k2 --all

stopped
again 'no key' 'runtime pkgconf a b c' '' --all

stopped
ports k1 c || fail "lone port: build_port.sh c failed"
again 'lone port' 'runtime pkgconf a b c' k1 --all

stopped
rm -rf "$out/stage/a"
again 'stage gone' 'runtime pkgconf a b c' k1 --all
exit "$failed"
