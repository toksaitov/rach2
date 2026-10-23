#!/bin/sh
# win-run-smoke.sh: check the shell entry points with native Windows helpers.
# Run in an admitted MSYS2 shell with UCRT64 GCC available.
set -eu
case $(uname -s) in MINGW*|MSYS*|UCRT*|CLANG*) ;; *) exit 2 ;; esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
finished=
status=1
trap 'status=$?; [ -z "$work" ] || rm -rf "$work"; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; exit "$status"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$repo/build/tests"
work=$(mktemp -d "$repo/build/tests/rach2-win-run.XXXXXX")
sh "$repo/tests/run" inventory > "$work/inventory"
cmp "$repo/tests/system/suites.md" "$work/inventory"
echo 'win-run-smoke: inventory passed'

mkdir -p "$work/tree's path/build" "$work/tree's path/tools/etude/win"
for file in hold.h etude_hold.c win/hold.c win/winfmt.c win/winfmt.h; do
    cp "$repo/tools/etude/$file" "$work/tree's path/tools/etude/$file"
done
cat > "$work/tree's path/check.sh" <<'SCRIPT'
set -eu
repo=$1
shift
. "$repo/tools/etude/hold.sh"
tree=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
etude_hold "$tree" win-run-smoke "$0" "$repo" "$@"
etude_held "$tree"
[ "$(sed -n 's/^pid //p' "$tree/build/.run.lock/owner")" = "$RACH2_TREE_HELD" ]
[ -z "${MSYS2_ARG_CONV_EXCL:-}" ]
printf '%s\000' "$@" > "$tree/actual"
exit 23
SCRIPT
# shellcheck disable=SC2016 # shell syntax must reach the script literally
set -- 'two words' "single'quote" 'double"quote' 'back\slash' "end\\" '' \
    /tmp/literal 'C:/native path' '$(echo wrong)' '*'
printf '%s\000' "$@" > "$work/expected"
status=0
sh "$work/tree's path/check.sh" "$repo" "$@" || status=$?
[ "$status" -eq 23 ]
if ! cmp "$work/expected" "$work/tree's path/actual"; then
    od -An -tc "$work/tree's path/actual" >&2
    exit 1
fi
[ ! -d "$work/tree's path/build/.run.lock" ]

tree="$work/run's path"
mkdir -p "$tree/tests/cases" "$tree/tools/setup" "$tree/tools/etude/win" \
    "$tree/tools/etude/include/etude" "$work/bin"
cp "$repo/tests/run" "$tree/tests/"
for file in hold.h hold.sh etude_hold.c etude_ports.c portlock.c include/etude/portlock.h \
    win/hold.c win/winfmt.c win/winfmt.h collection-windows.sh collection_tools.py; do
    cp "$repo/tools/etude/$file" "$tree/tools/etude/$file"
done
for file in hostcheck.c e2fsprogs_path.sh ovmf_path.sh space.sh; do
    cp "$repo/tools/setup/$file" "$tree/tools/setup/$file"
done
printf 'default|qemu|core||128\nstyle|-|full||128\n' > "$tree/tests/registry"
printf '{}\n' > "$tree/tests/cases/guest.default.json"
printf '{}\n' > "$tree/tests/cases/host.style.json"
cat > "$work/bin/make" <<'SCRIPT'
#!/bin/sh
set -eu
. "$SMOKE_TREE/tools/etude/hold.sh"
cp "$SMOKE_TREE/build/.run.lock/owner" "$SMOKE_TREE/owner.before"
etude_held "$SMOKE_TREE"
etude_hold "$SMOKE_TREE" win-run-smoke "$0" "$@"
[ -n "${RACH2_TREE_HELD:-}" ]
[ "$(sed -n 's/^pid //p' "$SMOKE_TREE/build/.run.lock/owner")" = "$RACH2_TREE_HELD" ]
cmp "$SMOKE_TREE/owner.before" "$SMOKE_TREE/build/.run.lock/owner"
printf 'held\n' > "$SMOKE_TREE/made"
exit 1
SCRIPT
cat > "$work/socket-refused.h" <<'SOURCE'
/* Replace one socket operation with a permission failure. */
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
/** Refuse the socket type selected by the fixture's environment. */
static SOCKET smoke_socket(int domain, int type, int protocol)
{
    const char *which = getenv("SMOKE_REFUSE");
    if (which && ((strcmp(which, "tcp") == 0 && type == SOCK_STREAM) ||
                  (strcmp(which, "udp") == 0 && type == SOCK_DGRAM))) {
        WSASetLastError(WSAEACCES);
        return INVALID_SOCKET;
    }
    return socket(domain, type, protocol);
}
#define socket smoke_socket
SOURCE
cat > "$work/bin/compiler" <<'SCRIPT'
#!/bin/sh
set -eu
for arg do
    case $arg in
        */tools/setup/hostcheck.c) set -- -include "$SMOKE_HEADER" "$@"; break ;;
    esac
done
exec "$SMOKE_CC" "$@"
SCRIPT
chmod 755 "$work/bin/make" "$work/bin/compiler"
SMOKE_TREE=$tree
SMOKE_CC=${ETUDE_WIN_CC:-/ucrt64/bin/gcc}
SMOKE_HEADER=$(cygpath -m "$work/socket-refused.h")
ETUDE_WIN_CC=$work/bin/compiler
PATH=$work/bin:$PATH
RACH2_QEMU_ACCEL=tcg RACH2_WORKERS=1 RACH2_MEMORY_MIB=1024
export SMOKE_TREE SMOKE_CC SMOKE_HEADER ETUDE_WIN_CC PATH
export RACH2_QEMU_ACCEL RACH2_WORKERS RACH2_MEMORY_MIB
unset RACH2_CASES RACH2_AGENTS_ROWS RACH2_ACCEPT_AGENTS RACH2_RUN_WORK
unset RACH2_RUN_STATUS RACH2_TREE_HELD RACH2_HOLD_HELPER
for row in pass tcp udp no-sockets; do
    rm -f "$tree/made"
    suite=default
    SMOKE_REFUSE=$row
    [ "$row" != no-sockets ] || { suite=style; SMOKE_REFUSE=tcp; }
    export SMOKE_REFUSE
    status=0
    sh "$tree/tests/run" "$suite" > "$work/$row.out" 2>&1 || status=$?
    cat "$work/$row.out"
    [ "$status" -eq 1 ]
    case $row in
        pass|no-sockets)
            grep -qx 'hostcheck: ok' "$work/$row.out"
            [ "$(cat "$tree/made")" = held ] ;;
        *)
            grep -q 'hostcheck: refused: a .* socket bound on the loopback address' "$work/$row.out"
            grep -q 'nothing was built' "$work/$row.out"
            [ ! -e "$tree/made" ]
            [ ! -e "$tree/build/.run.guard" ] ;;
    esac
    [ ! -d "$tree/build/.run.lock" ]
    rm -f "$tree/build/.run.guard"
    echo "win-run-smoke: host operations: $row passed"
done
finished=1
echo 'win-run-smoke: result: passed: native bootstrap, shell arguments, host operations and tree hold'
