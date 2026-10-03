#!/bin/sh
#
# archcheck.sh: hold the kernel's shared code to what every architecture can
# build.
#
# Usage: archcheck.sh SHARED_C_FILES... -- PLATFORM_C_FILES...
#        run from rach2/, as `make -C rach2 check-arch` runs it.
#
# The shared code, C_SRC_SHARED in rach2/sources.mk, is compiled by every
# architecture; a platform's files, the processor's and the drivers of the
# devices only that platform has, are listed by the architecture. Three
# checks, each naming what it found:
#
#   1. clang's front end compiles the shared code for 32-bit and 64-bit ARM,
#      the two architectures with headers alone, with the pointer and
#      declaration errors on: a shared file that uses a register, a port or a
#      function only an x86 has fails to compile there, and so does one that
#      holds a pointer in a 32-bit integer.
#   2. No shared file includes, through any chain, a header under an
#      architecture's directory but its cpu.h through arch.h, or the header of
#      a platform's own file: what it calls there would not exist on another
#      platform.
#   3. No shared file has inline assembly. The one exception is the compiler
#      barrier `k_barrier` in lib/kdef.h, which names no instruction.

status=0
shared=
platform=
seen_sep=0
for f in "$@"; do
    if [ "$f" = "--" ]; then
        seen_sep=1
    elif [ "$seen_sep" = 0 ]; then
        shared="$shared $f"
    else
        platform="$platform $f"
    fi
done
if [ -z "$shared" ]; then
    echo "archcheck: no shared files given" >&2
    exit 2
fi

FLAGS="-ffreestanding -fsyntax-only -ansi -ferror-limit=0 -Wno-everything \
    -Werror=implicit-function-declaration -Werror=int-conversion \
    -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast \
    -Werror=incompatible-pointer-types"

for target in armv7a-none-eabi aarch64-none-elf; do
    # shellcheck disable=SC2086
    if ! clang --target=$target $FLAGS $shared; then
        echo "archcheck: the shared code does not compile for $target" >&2
        status=1
    fi
done

# The headers of the platform's own files: a .c listed by the platform with
# a .h beside it.
platform_headers=
for f in $platform; do
    h="${f%.c}.h"
    [ -f "$h" ] && platform_headers="$platform_headers $h"
done

for f in $shared; do
    deps=$(clang --target=aarch64-none-elf -ffreestanding -ansi -MM "$f" 2>/dev/null |
        tr ' \\' '\n\n' | sed -n 's|^\(\./\)*||p' | sort -u)
    for d in $deps; do
        clean=$(echo "$d" | awk -F/ '{
            n = 0
            for (i = 1; i <= NF; i++) {
                if ($i == "..") { if (n > 0) n-- } else if ($i != ".") a[++n] = $i
            }
            s = a[1]
            for (i = 2; i <= n; i++) s = s "/" a[i]
            print s
        }')
        case "$clean" in
        sys/arch/arch.h | sys/arch/*/cpu.h | sys/arch/*/sysnr.h) ;;
        sys/arch/*)
            echo "archcheck: $f: includes $clean, an architecture's own header" >&2
            status=1
            ;;
        esac
        for h in $platform_headers; do
            if [ "$clean" = "$h" ]; then
                echo "archcheck: $f: includes $clean, a platform's own header" >&2
                status=1
            fi
        done
    done
done

shared_headers=$(find sys -name '*.h' ! -path 'sys/arch/*' | sort)
for f in $shared $shared_headers; do
    hits=$(grep -n '__asm__\|\basm[[:space:]]*(' "$f" | grep -v '#define k_barrier()')
    if [ -n "$hits" ]; then
        echo "$hits" | sed "s|^|archcheck: $f:|; s|\$|: inline assembly in shared code|" >&2
        status=1
    fi
done

if [ "$status" = 0 ]; then
    n=$(echo $shared | wc -w | tr -d ' ')
    echo "archcheck: ok: $n shared files compile for arm and arm64, reach no architecture's or platform's header, and have no assembly"
fi
exit $status
