#!/usr/bin/env sh

#
# fetch_agents.sh: fetch the packages of the agents layer, check each against
# the digest its vendor publishes, and stage it as the x86-64 GNU root has it.
#
# Usage: fetch_agents.sh NAME...
#
# A package is a recipe, packages/NAME.sh, a shell fragment that sets
#
#   VERSION  the version the vendor published
#   LICENSE  the license the vendor gives it, as a short name
#   FILES    one line for each file the package downloads: the name it is
#            kept under, its URL, and its digest as the vendor publishes it,
#            `sha256:HEX` or `sha512:HEX`
#   NOTICES  the package's license and notice files, as paths in the stage
#
# and defines package_stage DIR, which lays the package out under DIR as the
# root has it and takes each download through `fetched NAME`. A recipe may
# also define package_verify, which checks the vendor's signatures on the
# downloads, and package_check DIR, which runs the staged program on the
# build host. Both may write in SCRATCH, an empty directory of the package's
# own that is removed when it is staged.
#
# A download is kept in ../downloads/agents/NAME/ once and checked against
# its digest each time `fetched` names it; a file with another digest is
# removed and refused. The stage is agents/NAME of the layer's build tree for
# x86-64 (vendor/gnu/arch.sh), made again from nothing, and fails when one of
# NOTICES is not in it.
#

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
GNU="$(cd "$DIR/.." && pwd)"
REPO="$(cd "$GNU/../.." && pwd)"
. "$GNU/arch.sh"
SHA="$REPO/build/tools/verify/sha256"

if [ "$RACH2_ARCH" != x86_64 ]; then
    echo "gnu: agents: the packages are built for x86-64 only; run with RACH2_ARCH=x86_64" >&2
    exit 1
fi
if [ $# -eq 0 ]; then
    echo "usage: fetch_agents.sh NAME..." >&2
    exit 2
fi

# digest_of ALGORITHM FILE: print FILE's digest in hex.
digest_of()
{
    case $1 in
        sha256)
            make -s -C "$REPO" build/tools/verify/sha256 || return 1
            "$SHA" "$2" | awk '{print $1}'
            ;;
        sha512)
            if command -v sha512sum > /dev/null 2>&1; then
                sha512sum "$2"
            else
                shasum -a 512 "$2"
            fi | awk '{print $1}'
            ;;
        *)
            echo "gnu: agents: $NAME: no digest $1; the recipe may give sha256 or sha512" >&2
            return 1
            ;;
    esac
}

download()
{
    if command -v curl > /dev/null 2>&1; then
        curl -sSfL --retry 3 -o "$2" "$1"
    else
        wget -q -O "$2" "$1"
    fi
}

# fetched FILE: print the checked path of the download FILE of the recipe's
# FILES, downloading it when it is missing.
fetched()
{
    line=$(printf '%s\n' "$FILES" | awk -v f="$1" '$1 == f { print; exit }')
    if [ -z "$line" ]; then
        echo "gnu: agents: $NAME: no $1 in the recipe's FILES" >&2
        return 1
    fi
    set -f
    # shellcheck disable=SC2086 # three words; set -f keeps a URL's ? as it is
    set -- $line
    set +f
    file="$DL/$1"
    algorithm=${3%%:*}
    want=${3#*:}
    mkdir -p "$DL"
    if [ ! -f "$file" ]; then
        echo "gnu: agents: $NAME $VERSION: downloading $1" >&2
        download "$2" "$file.part" || {
            rm -f "$file.part"
            echo "gnu: agents: $NAME: download failed: $2" >&2
            return 1
        }
        mv "$file.part" "$file"
    fi
    got=$(digest_of "$algorithm" "$file") || return 1
    if [ "$got" != "$want" ]; then
        rm -f "$file"
        echo "gnu: agents: $NAME: refused: $1: $algorithm $got, pinned $want" >&2
        return 1
    fi
    echo "$file"
}

# stage NAME: fetch, check and stage the package NAME, in a shell of its own
# so that one recipe's variables and functions do not reach the next.
stage()
(
    NAME=$1
    recipe="$DIR/packages/$NAME.sh"
    if [ ! -f "$recipe" ]; then
        echo "gnu: agents: no package $NAME: there is no $recipe" >&2
        exit 1
    fi
    VERSION=
    # shellcheck disable=SC2034 # the check below reads it through eval
    LICENSE=
    FILES=
    NOTICES=
    # shellcheck disable=SC1090 # the recipe is chosen at run time
    . "$recipe"
    for var in VERSION LICENSE FILES NOTICES; do
        if eval "[ -z \"\$$var\" ]"; then
            echo "gnu: agents: $NAME: the recipe sets no $var" >&2
            exit 1
        fi
    done
    DL="$GNU/downloads/agents/$NAME"
    for f in $(printf '%s\n' "$FILES" | awk 'NF { print $1 }'); do
        fetched "$f" > /dev/null
    done
    SCRATCH=
    trap '[ -z "$SCRATCH" ] || rm -rf "$SCRATCH"' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/rach2-agents.XXXXXX")
    if command -v package_verify > /dev/null 2>&1; then
        package_verify
    fi

    STAGE="$GNU_OUT/agents/$NAME"
    rm -rf "$STAGE"
    mkdir -p "$STAGE"
    package_stage "$STAGE"
    for n in $NOTICES; do
        if [ ! -f "$STAGE/$n" ]; then
            echo "gnu: agents: $NAME: the stage has no $n" >&2
            exit 1
        fi
    done
    if command -v package_check > /dev/null 2>&1; then
        package_check "$STAGE"
    fi
    echo "gnu: agents: $NAME $VERSION: $(cd "$STAGE" && find . ! -type d | wc -l | tr -d ' ') files staged"
)

for name in "$@"; do
    stage "$name"
done
