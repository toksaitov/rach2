# shellcheck shell=sh
# Fetch a pinned asset into this clone's build cache and check it. The caller
# sets GNU and REPO; asset NAME [LIST] prints only the checked path. LIST is
# the manifest that pins NAME, sources-extra.txt by default.

asset()
(
    set -e
    part=
    trap '[ -z "$part" ] || rm -f "$part"' EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    line=$(awk -v n="$1" '$1 == n { print; exit }' "${2:-$GNU/sources-extra.txt}")
    [ -n "$line" ] || { echo "gnu: asset: no pin for $1" >&2; return 1; }
    # shellcheck disable=SC2086 # the manifest has four fields
    set -- $line
    cache="$REPO/build/cache/gnu"
    mkdir -p "$cache"
    file="$cache/${3##*/}"
    source=$file
    if [ ! -f "$file" ]; then
        part=$(mktemp "$file.part.XXXXXX")
        curl -sSfL --retry 3 -o "$part" "$3"
        source=$part
    fi
    make -s -C "$REPO" build/tools/verify/sha256 || return 1
    got=$("$REPO/build/tools/verify/sha256" "$source" | awk '{print $1}')
    if [ "$got" != "$4" ]; then
        [ "$source" != "$file" ] || rm -f "$file"
        echo "gnu: asset: refused: $1: sha256 $got, expected $4" >&2
        return 1
    fi
    if [ -n "$part" ]; then
        mv -f "$part" "$file"
        part=
    fi
    printf '%s\n' "$file"
)
