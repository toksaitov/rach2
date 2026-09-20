#!/bin/sh
# Compile fixed, faulty and restored TCP sources in a private tree.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/tcp-proof.XXXXXX")
child=
mkdir -p "$work/rach2" "$work/tests/host/compat"
cp -R rach2/sys "$work/rach2/sys"
cp tests/host/tcp_hosttest.c "$work/tests/host/"
cp tests/host/compat/kheap_host.c tests/host/compat/pressure_host.c "$work/tests/host/compat/"
source="$work/rach2/sys/network/tcp/tcp.c"
cp "$source" "$work/tcp.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/tcp.saved" "$source"
    touch "$source"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -g -O1 -Wno-empty-body -ffreestanding \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I "$work/tests/host/compat" \
        "$work/tests/host/tcp_hosttest.c" "$work/rach2/sys/lib/kstring.c" \
        "$work/rach2/sys/lib/kctypes.c" "$work/rach2/sys/network/tcp/rcvwnd.c" \
        "$work/tests/host/compat/kheap_host.c" "$work/tests/host/compat/pressure_host.c" \
        -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$work/tests/host/tcp_hosttest.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "tcp-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$status" -ne "$expected" ] || ! grep -Fq "$message" "$binary.log"; then
        echo "tcp-mutation: unexpected result: $phase status $status" >&2
        exit 1
    fi
    echo "tcp-mutation: result: $phase status $status"
}

echo "tcp-mutation: logs: $work"
run_phase fixed 0 'tcp_hosttest: PASS'
python3 - "$source" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
source = path.read_text()
start = source.index("int tcp_send(")
end = source.index("\n}\n", start) + 3
fixed = source[start:end]
if fixed.count("    flags = net_lock();\n") != 1:
    raise SystemExit("tcp-mutation: lock pattern is not unique")
if fixed.count("        net_unlock(flags);\n") != 2:
    raise SystemExit("tcp-mutation: error-unlock pattern count differs")
faulty = fixed.replace("    flags = net_lock();\n", "", 1)
faulty = faulty.replace("        net_unlock(flags);\n", "")
old = "    free_space = TCP_RING_SIZE"
if faulty.count(old) != 1:
    raise SystemExit("tcp-mutation: ring pattern is not unique")
faulty = faulty.replace(old, "    flags = net_lock();\n\n" + old, 1)
path.write_text(source[:start] + faulty + source[end:])
PY
run_phase mutated 3 'send rejects a reset before masking'
cleanup
run_phase restored 0 'tcp_hosttest: PASS'
cmp "$work/tcp.saved" "$source"
