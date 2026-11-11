#!/bin/sh
# Rebuild GE-Proton's DirectInput DLLs with the process-exit correction.

set -e
GNU=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck disable=SC2034 # arch.sh and fetch_asset.sh read REPO
REPO=$(cd "$GNU/../.." && pwd)
. "$GNU/arch.sh"
. "$GNU/debian/fetch_asset.sh"
WORK="$GNU_OUT/work/proton-dinput"
ROOT="$GNU_OUT/proton/opt/proton"
for tool in clang ld.lld llvm-dlltool autoreconf flex bison; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "gnu: proton: $tool not found; install the GNU host tools" >&2
        exit 1
    }
done
wine=$(asset wine-source "$GNU/proton/sources.txt")
ge=$(asset ge-source "$GNU/proton/sources.txt")
staging=$(asset staging-source "$GNU/proton/sources.txt")
rm -rf "${WORK:?}"
mkdir -p "$WORK/src" "$WORK/obj" "$WORK/tmp" "$WORK/cache"
tar -xf "$wine" -C "$WORK/src" --strip-components=1
python3 "$GNU/proton/prepare_dinput.py" "$ge" "$staging" "$WORK/src"
patch --batch --fuzz=0 -d "$WORK/src" -p1 < "$GNU/proton/dinput-shutdown.patch"
(
    cd "$WORK/src"
    XDG_CACHE_HOME="$WORK/cache" python3 dlls/winevulkan/make_vulkan -x vk.xml -X video.xml
    ./tools/make_specfiles
    ./tools/make_requests
    autoreconf -f
)
(
    cd "$WORK/obj"
    ../src/configure --enable-win64 --enable-archs=x86_64,i386 \
        --with-mingw=clang --disable-tests --without-x --without-freetype
    case " $MAKEFLAGS " in
        *jobserver*) jobs= ;;
        *) jobs="-j${GNU_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}" ;;
    esac
    # shellcheck disable=SC2086 # jobs is empty or one make option
    make $jobs dlls/dinput/x86_64-windows/dinput.dll \
        dlls/dinput8/x86_64-windows/dinput8.dll \
        dlls/dinput/i386-windows/dinput.dll dlls/dinput8/i386-windows/dinput8.dll
)
for arch in x86_64 i386; do
    for dll in dinput dinput8; do
        chmod u+w "$ROOT/files/lib/wine/$arch-windows/$dll.dll"
        cp "$WORK/obj/dlls/$dll/$arch-windows/$dll.dll" "$ROOT/files/lib/wine/$arch-windows/$dll.dll"
    done
done
chmod u+w "$ROOT"
mkdir -p "$ROOT/share/rach2-dinput"
cp "$GNU/proton/dinput-shutdown.patch" "$GNU/proton/sources.txt" "$ROOT/share/rach2-dinput/"
echo "gnu: proton: result: DirectInput rebuilt for process shutdown"
