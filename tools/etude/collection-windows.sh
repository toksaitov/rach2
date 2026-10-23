# shellcheck shell=sh
# collection-windows.sh: prepare the native collection's image command paths.

collection_root=$(cygpath -u "$REPO") || return 1
COLLECTION_IMAGE_TOOLS=$(python3 "$collection_root/tools/etude/collection_tools.py" list) || return 1

# Accept a declared image command before its native entry point is built.
collection_windows_tool()
{
    case ${2:-}:$1 in
        style:cppcheck|style:shellcheck|style:clang|style:clang-format)
            tooling_host
            return $? ;;
    esac
    case " $COLLECTION_IMAGE_TOOLS " in
        *" $1 "*) tooling_host ;;
        *) return 1 ;;
    esac
}

collection_windows()
{
    case $(uname -s) in MINGW*|MSYS*|UCRT*|CLANG*) ;; *) return 0 ;; esac
    make -C "$collection_root" --no-print-directory build/tools/etude/image-command \
        build/tools/etude/etude-tooling || return 1
    commands=$collection_root/build/tools/etude/commands
    mkdir -p "$commands" || return 1
    for tool in $COLLECTION_IMAGE_TOOLS; do
        real=$(command -v "$tool") || real=
        case $real in
            "$commands/"*|'') ;;
            *) continue ;;
        esac
        cp "$collection_root/build/tools/etude/image-command" "$commands/$tool.exe" || return 1
    done
    PATH=$commands:$PATH
    export PATH
}
