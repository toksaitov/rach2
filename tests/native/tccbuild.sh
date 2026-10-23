#!/bin/sh
# tccbuild.sh: a TinyCC build, run as one case by `etude run native` on a
# Rach2 machine that is already up. Its argument chooses the build: `pass`
# compiles a program, runs it and checks what it prints; `fail` compiles a
# source with an #error, so that the build fails on purpose and the case
# exits with the compiler's status; and `loop` builds the program again and
# again until it is stopped. TCC names the compiler, tcc unless it is set.
# The program is linked -static, as every program for Rach2 is and as
# Rach2's tcc requires, except on macOS, whose linker makes no static
# program; etude_native_hosttest runs the build there with the host's cc.
set -u
tcc=${TCC:-tcc}
system=$(uname -s)

# build: write the program's source and compile it; the compiler's status.
build()
{
    cat > sum.c <<'EOF'
#include <stdio.h>

int main(void)
{
    int i, sum = 0;

    for (i = 1; i <= 100; i++)
        sum += i;
    printf("tccbuild: sum %d\n", sum);
    return 0;
}
EOF
    if [ "$system" = Darwin ]; then
        "$tcc" -o sum sum.c
    else
        "$tcc" -static -o sum sum.c
    fi
}

case ${1:-} in
pass)
    build || exit 1
    out=$(./sum) || exit 1
    if [ "$out" != 'tccbuild: sum 5050' ]; then
        echo "tccbuild: the program printed: $out" >&2
        exit 1
    fi
    echo "$out"
    echo 'tccbuild: pass'
    ;;
fail)
    echo '#error this build fails on purpose' > broken.c
    exec "$tcc" -c -o broken.o broken.c
    ;;
loop)
    n=0
    while :; do
        n=$((n + 1))
        build || exit 1
        echo "tccbuild: build $n"
    done
    ;;
*)
    echo 'usage: tccbuild.sh pass|fail|loop' >&2
    exit 2
    ;;
esac
