#!/bin/sh
# Fetch signed Debian snapshot indices, resolve named package lists, and stage
# their SHA-256-pinned packages. Usage: fetch_debs.sh LAYER [STEP...].
# GNU_DEBIAN_LISTS names lists relative to vendor/gnu. A list has one package
# per line, optionally =VERSION, and optionally an amd64 or i386 selector.
# With no steps, run index, resolve, fetch, unpack and check. Each layer has
# its own stage and closure; download caches are shared within this clone.

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
GNU="$(cd "$DIR/.." && pwd)"
# shellcheck disable=SC2034 # arch.sh reads it
REPO="$(cd "$GNU/../.." && pwd)"
. "$GNU/arch.sh"
. "$DIR/fetch_asset.sh"
OUT="$GNU_OUT"
LAYER=${1:-desktop}
case $LAYER in
    *[!a-z0-9_-]*|'') echo "gnu: debian: invalid layer: $LAYER" >&2; exit 2 ;;
esac
[ $# -eq 0 ] || shift
WORK="$OUT/work/debian/$LAYER"
LOG="$OUT/log"
ROOT="$OUT/debian/$LAYER"
DEBS="$REPO/build/cache/debian/$GNU_DEB_ARCH"
SHA="$REPO/build/tools/verify/sha256"
# Each layer names its lists; later tasks add lists beside their code.
LISTS=${GNU_DEBIAN_LISTS:-"debian/mesa.list debian/desktop.list debian/browsers.list debian/sound.list debian/tools.list"}
ROOTS=
for list in $LISTS; do
    case $list in
        /*|*..*) echo "gnu: debian: invalid list: $list" >&2; exit 2 ;;
    esac
    [ -f "$GNU/$list" ] || { echo "gnu: debian: missing list: $list" >&2; exit 1; }
    selected=$(awk -v arch="$GNU_DEB_ARCH" '
        /^[ \t]*#/ || NF == 0 { next }
        NF > 2 || (NF == 2 && $2 != "amd64" && $2 != "i386") { exit 1 }
        NF == 1 || $2 == arch { print $1 }' "$GNU/$list") || {
        echo "gnu: debian: malformed package list: $list" >&2
        exit 1
    }
    ROOTS="$ROOTS $selected"
done
# Accounts and the base runtime come from the Rach2 manifest and GNU ports.
UNFOLLOWED="adduser"

SNAPSHOT=20260927T082144Z

# Each suite as archive:suite:SHA-256 of its InRelease at SNAPSHOT.
SUITES="debian:trixie:0584fba32e13e0ab8285fb16c27adea1ec03a73669c18702821094fd6ca86675
debian-security:trixie-security:07731db874e977c02ed7da0a721ec9c53ed793f1360beb1e19dab65194b2edb7"

# The keys that sign the two suites, each as its name in sources-extra.txt, a
# colon, and its primary fingerprint, which ftp-master.debian.org publishes
# and the debian-archive-keyring package has.
KEYS="debian-archive-key-12:B8B80B5B623EAB6AD8775C45B7C5D7D6350947F8
debian-archive-key-13:04B54C3CDCA79751B16BC6B5225629DF75B188BD
debian-release-key-13:41587F7DB8C774BCCF131416762F67A0B2C39DE4
debian-security-key-12:05AB90340C0C5E797F44A8C8254CF3B5AEC0A8F0
debian-security-key-13:5E04A1E3223A19A20706E20F9904613D4CCE68C6"

for tool in python3 dpkg gpg gpgv xz ar tar awk sort; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "gnu: debian: $tool not found" >&2
        exit 1
    fi
done
GNUPGHOME="$WORK/gnupg"
export GNUPGHOME

make -s -C "$REPO" build/tools/verify/sha256

sha256_of()
{
    "$SHA" "$1" | awk '{print $1}'
}

download()
{
    if command -v curl >/dev/null 2>&1; then
        curl -sSfL --retry 3 -o "$2" "$1"
    else
        wget -q -O "$2" "$1"
    fi
}

# get URL FILE SHA256: leave FILE with that digest, downloading it when it is
# missing or has another.
get()
(
    part=
    trap '[ -z "$part" ] || rm -f "$part"' EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    if [ -f "$2" ]; then
        got="$(sha256_of "$2")"
        if [ "$got" != "$3" ]; then
            rm -f "$2"
            echo "gnu: debian: refused: $1: sha256 $got, expected $3" >&2
            return 1
        fi
        return 0
    fi
    part=$(mktemp "$2.part.XXXXXX")
    download "$1" "$part" || {
        echo "gnu: debian: download failed: $1" >&2
        return 1
    }
    got="$(sha256_of "$part")"
    if [ "$got" != "$3" ]; then
        echo "gnu: debian: refused: $1: sha256 $got, expected $3" >&2
        return 1
    fi
    mv -f "$part" "$2"
    part=
)

keyring()
{
    rm -rf "$GNUPGHOME"
    mkdir -p "$GNUPGHOME"
    chmod 700 "$GNUPGHOME"
    for k in $KEYS; do
        asc="$(asset "${k%%:*}")"
        fpr="$(gpg --batch --with-colons --show-keys "$asc" | awk -F: '$1 == "fpr" { print $10; exit }')"
        if [ "$fpr" != "${k#*:}" ]; then
            echo "gnu: debian: ${k%%:*}: fingerprint $fpr, expected ${k#*:}" >&2
            return 1
        fi
        gpg --batch --dearmor < "$asc" >> "$WORK/keyring.gpg"
    done
}

step_index()
{
    rm -rf "$WORK"
    mkdir -p "$WORK" "$DEBS"
    keyring
    for s in $SUITES; do
        archive="${s%%:*}"
        rest="${s#*:}"
        suite="${rest%%:*}"
        base="https://snapshot.debian.org/archive/$archive/$SNAPSHOT/dists/$suite"
        get "$base/InRelease" "$DEBS/$suite.InRelease" "${rest#*:}"
        gpgv --keyring "$WORK/keyring.gpg" --output "$WORK/$suite.Release" \
            "$DEBS/$suite.InRelease"
        index="main/binary-$GNU_DEB_ARCH/Packages.xz"
        sha="$(awk -v want="$index" '/^SHA256:/ { s = 1; next } /^[^ ]/ { s = 0 }
            s && $3 == want { print $1 }' "$WORK/$suite.Release")"
        if [ -z "$sha" ]; then
            echo "gnu: debian: $suite: no $GNU_DEB_ARCH Packages index in the Release" >&2
            return 1
        fi
        get "$base/$index" "$DEBS/$suite.$GNU_DEB_ARCH.Packages.xz" "$sha"
        xz -dc "$DEBS/$suite.$GNU_DEB_ARCH.Packages.xz" > "$WORK/$suite.Packages"
        echo "$archive $suite: $(awk '/^Date:/ { sub(/^Date: /, ""); print; exit }' "$WORK/$suite.Release")"
    done
}

# Each index becomes one line per package, fields separated by tabs, and the
# suites are read in order, so that a later suite's entry replaces an earlier
# one's. A dependency is the first alternative that is a package, or else the
# first a package provides, with version constraints checked by dpkg. A name
# in UNFOLLOWED ends the search; the image already supplies its function.
step_resolve()
{
    for s in $SUITES; do
        rest="${s#*:}"
        awk -v archive="${s%%:*}" '
            function flush() {
                if (pkg != "")
                    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                        pkg, ver, archive, file, size, sha, dep, prov
                pkg = ver = file = size = sha = dep = prov = ""
            }
            /^Package: / { pkg = $2 }
            /^Version: / { ver = $2 }
            /^Filename: / { file = $2 }
            /^Size: / { size = $2 }
            /^SHA256: / { sha = $2 }
            /^(Pre-)?Depends: / {
                sub(/^[^:]*: /, "")
                dep = dep == "" ? $0 : dep ", " $0
            }
            /^Provides: / { sub(/^Provides: /, ""); prov = $0 }
            /^$/ { flush() }
            END { flush() }' "$WORK/${rest%%:*}.Packages"
    done > "$WORK/packages.tsv"
    # shellcheck disable=SC2086 # ROOTS contains package names, never paths
    python3 "$DIR/resolve_debs.py" "$WORK/packages.tsv" "$WORK/closure.txt" "$UNFOLLOWED" $ROOTS
    sort "$WORK/closure.txt" | awk '{ print $1, $2, $3; total += $5 }
        END { printf "%d packages, %.1f MB\n", NR, total / 1e6 }'
}

step_fetch()
{
    mkdir -p "$DEBS"
    while read -r name _ archive file _ sha; do
        get "https://snapshot.debian.org/archive/$archive/$SNAPSHOT/$file" \
            "$DEBS/${file##*/}" "$sha"
        echo "gnu: debian: checked: $name"
    done < "$WORK/closure.txt"
    echo "$(wc -l < "$WORK/closure.txt") packages checked"
}

# The root has the merged /usr of trixie, whose packages expect /bin, /lib
# and /sbin to be links into /usr, and on amd64 /lib64 as well, where libc6
# puts the loader the browser asks for; tar keeps the links when a package
# has files under them.
step_unpack()
{
    rm -rf "$ROOT" "$WORK/files"
    mkdir -p "$ROOT/usr/bin" "$ROOT/usr/lib" "$ROOT/usr/sbin" "$WORK/files"
    ln -s usr/bin "$ROOT/bin"
    ln -s usr/lib "$ROOT/lib"
    ln -s usr/sbin "$ROOT/sbin"
    if [ "$GNU_DEB_ARCH" = amd64 ]; then
        mkdir -p "$ROOT/usr/lib64"
        ln -s usr/lib64 "$ROOT/lib64"
    fi
    while read -r name _ archive file _ sha; do
        deb="$DEBS/${file##*/}"
        if [ ! -f "$deb" ] || [ "$(sha256_of "$deb")" != "$sha" ]; then
            echo "gnu: debian: $name: ${deb##*/} is missing or not the package the closure names" >&2
            return 1
        fi
        member="$(ar t "$deb" | grep '^data\.tar')"
        case "$member" in
            *.xz) unpack="xz -dc" ;;
            *.gz) unpack="gzip -dc" ;;
            *.zst) unpack="zstd -dc" ;;
            *) echo "gnu: debian: $name: no data member" >&2; return 1 ;;
        esac
        ar p "$deb" "$member" > "$WORK/data.tar.compressed"
        $unpack < "$WORK/data.tar.compressed" > "$WORK/data.tar"
        tar -xvf "$WORK/data.tar" -C "$ROOT" --keep-directory-symlink > "$WORK/files/$name"
        rm -f "$WORK/data.tar.compressed" "$WORK/data.tar"
    done < "$WORK/closure.txt"
    du -sh "$ROOT"
}

# Validate the unpacked stage without executing target programs on the host.
step_check()
{
    while read -r name _ archive file _ sha; do
        [ -s "$WORK/files/$name" ] || {
            echo "gnu: debian: $name: no unpacked file list" >&2
            return 1
        }
        [ "$(sha256_of "$DEBS/${file##*/}")" = "$sha" ] || return 1
    done < "$WORK/closure.txt"
    echo "gnu: debian: result: $(wc -l < "$WORK/closure.txt") packages pinned and unpacked"
}

if [ $# -eq 0 ]; then
    rm -rf "$WORK" "$ROOT"
    set -- index resolve fetch unpack check
fi
mkdir -p "$LOG"
for step in "$@"; do
    case "$step" in
        index|resolve|fetch|unpack|check) ;;
        *) echo "gnu: debian: no step named $step" >&2; exit 2 ;;
    esac
    echo "gnu: debian: $step"
    log="$LOG/debian-$LAYER-$step.log"
    t0=$(date +%s)
    set +e
    ( set -e; "step_$step" ) > "$log" 2>&1
    status=$?
    set -e
    if [ $status -ne 0 ]; then
        tail -n 40 "$log" >&2
        echo "gnu: debian: $step: failed; the log is $log" >&2
        exit 1
    fi
    case "$step" in
        resolve|check) cat "$log" ;;
    esac
    echo "gnu: debian: $step: done in $(($(date +%s) - t0)) s"
done
