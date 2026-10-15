#!/usr/bin/env sh
#
# tests/fuzz/build.sh MODE [NAME...]: build the fuzz harnesses under
# tests/fuzz, each a NAME_fuzz.c that defines the entry fuzz.h declares and
# names the sources it links in `fuzz-deps:` comment lines, as the host tests
# do with `host-deps:`.
#
#   replay   a program that replays the files on its command line under
#            ASan and UBSan, with fuzz.h's main; the host suite runs each
#            over its corpus under tests/fuzz/corpus/NAME
#   fuzz     the libFuzzer program, built with clang, for the long runs
#
# The compiler writes the programs to build/tests/fuzz as NAME and NAME-fuzz.
# With HOST_PRINT_COMMANDS set, the replay build prints its compile commands and
# compiles nothing, for the compilation database.
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO" || exit 1
mode=${1:-replay}
shift 2>/dev/null
OUT=build/tests/fuzz
mkdir -p "$OUT"
CC="${CC:-cc}"
INC="-I rach2/sys -I rach2/sys/lib -I rach2/sys/proc -I utils -I krffs -I tests/host/compat"
COMMON="-std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST -DFIXTURE_NO_MAIN -g -O1 -Wno-empty-body -ffreestanding $INC"
fail=0
if [ $# -eq 0 ]; then
    for src in tests/fuzz/*_fuzz.c; do
        set -- "$@" "$(basename "$src" _fuzz.c)"
    done
fi
for name in "$@"; do
    src="tests/fuzz/${name}_fuzz.c"
    deps=$(grep -o 'fuzz-deps:[^*]*' "$src" | sed 's/fuzz-deps://' | tr '\n' ' ')
    case $mode in
    replay)
        cmd="$CC $COMMON -DFUZZ_REPLAY -fsanitize=address,undefined -fno-sanitize-recover=all -o $OUT/$name $src $deps"
        ;;
    fuzz)
        cmd="clang $COMMON -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -o $OUT/$name-fuzz $src $deps"
        ;;
    *)
        echo "fuzz: usage: build.sh replay|fuzz [NAME...]" >&2; exit 2 ;;
    esac
    if [ -n "${HOST_PRINT_COMMANDS:-}" ]; then
        echo "$cmd"
        continue
    fi
    if ! $cmd; then
        echo "fuzz: $name: did not build" >&2; fail=1
    fi
done
exit $fail
