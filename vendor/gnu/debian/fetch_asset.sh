# shellcheck shell=sh
# Fetch a source-extra.txt asset into this clone's build cache and check it.
# The caller sets GNU and REPO; asset NAME prints only the checked path.

asset()
{
    line=$(awk -v n="$1" '$1 == n { print; exit }' "$GNU/sources-extra.txt")
    [ -n "$line" ] || { echo "gnu: asset: no pin for $1" >&2; return 1; }
    # shellcheck disable=SC2086 # the manifest has four fields
    set -- $line
    cache="$REPO/build/cache/gnu"
    mkdir -p "$cache"
    file="$cache/${3##*/}"
    if [ ! -f "$file" ]; then
        curl -fL --retry 3 -o "$file.part" "$3" || {
            rm -f "$file.part"
            return 1
        }
        mv "$file.part" "$file"
    fi
    make -s -C "$REPO" build/tools/verify/sha256 || return 1
    got=$("$REPO/build/tools/verify/sha256" "$file" | awk '{print $1}')
    if [ "$got" != "$4" ]; then
        rm -f "$file"
        echo "gnu: asset: refused: $1: sha256 $got, expected $4" >&2
        return 1
    fi
    printf '%s\n' "$file"
}
