#!/usr/bin/env sh

#
# fetch_chromium.sh: Debian's chromium-headless-shell for i386, with the
# libraries it needs and the D-Bus system bus it asks for, unpacked into a
# root of its own.
#
# Usage: fetch_chromium.sh [STEP...]
#
# Takes chromium-headless-shell, dbus-daemon and dbus-system-bus-common and
# the closure of their dependencies from Debian 13 (trixie) for i386, as snapshot.debian.org has the archive at the
# time SNAPSHOT below: the stable suite and its security suite, which has
# Chromium 154. The steps are:
#
#   index    fetch each suite's InRelease and check its digest and its
#            signature by the Debian archive keys, then fetch the suite's
#            i386 Packages index and check it against the digest that the
#            signed text lists
#   resolve  follow Depends and Pre-Depends from those three packages
#            through both indices, the security suite's entry for a package
#            replacing the stable suite's, and list the closure
#   fetch    fetch each package of the closure and check it against the
#            digest its index lists
#   unpack   unpack every package into build/vendor/gnu/chromium
#   check    list, through the sysroot's loader, the libraries the browser
#            and its own shared objects need, glibc's from the sysroot and
#            every other from the root; report the newest glibc symbol
#            version the root's programs need; and run the browser once on a
#            local page through the root's own loader and libraries
#
# With no STEP all five run from nothing, and naming steps runs only those
# over what an earlier run left. Each step writes its output to a log in
# build/vendor/gnu/log. The indices and the packages are kept in
# ../downloads/debian and checked again on every use, as fetch.sh keeps its
# tarballs.
#

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
GNU="$(cd "$DIR/.." && pwd)"
REPO="$(cd "$GNU/../.." && pwd)"
OUT="$REPO/build/vendor/gnu"
WORK="$OUT/work/chromium"
LOG="$OUT/log"
CROSS="$OUT/cross"
SYSROOT="$OUT/sysroot"
ROOT="$OUT/chromium"
DEBS="$GNU/downloads/debian"
SHA="$REPO/build/tools/verify/sha256"
TARGET=i686-linux-gnu
PACKAGE=chromium-headless-shell
BROWSER=usr/lib/chromium/chromium-headless-shell

# The packages the closure starts from: the browser, and D-Bus's system bus,
# which the browser asks for at /run/dbus/system_bus_socket and init starts.
# A dependency on a package of UNFOLLOWED is taken as met and not followed:
# adduser, which dbus-system-bus-common's maintainer script runs to make the
# bus's user, an account the root image has already.
ROOTS="$PACKAGE dbus-daemon dbus-system-bus-common"
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

for tool in cc gpg gpgv xz ar tar awk sort; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "gnu: chromium: $tool not found" >&2
        exit 1
    fi
done
GNUPGHOME="$WORK/gnupg"
export GNUPGHOME

sha256_of()
{
    if [ ! -x "$SHA" ]; then
        mkdir -p "$(dirname "$SHA")"
        cc -O2 -o "$SHA" "$REPO/tools/verify/sha256.c" || return 1
    fi
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
{
    if [ -f "$2" ] && [ "$(sha256_of "$2")" = "$3" ]; then
        return 0
    fi
    rm -f "$2"
    download "$1" "$2.part" || {
        rm -f "$2.part"
        echo "gnu: chromium: download failed: $1" >&2
        return 1
    }
    mv "$2.part" "$2"
    got="$(sha256_of "$2")"
    if [ "$got" != "$3" ]; then
        rm -f "$2"
        echo "gnu: chromium: refused: $1: sha256 $got, expected $3" >&2
        return 1
    fi
}

keyring()
{
    rm -rf "$GNUPGHOME"
    mkdir -p "$GNUPGHOME"
    chmod 700 "$GNUPGHOME"
    for k in $KEYS; do
        asc="$("$GNU/fetch.sh" --path "${k%%:*}")"
        fpr="$(gpg --batch --with-colons --show-keys "$asc" | awk -F: '$1 == "fpr" { print $10; exit }')"
        if [ "$fpr" != "${k#*:}" ]; then
            echo "gnu: chromium: ${k%%:*}: fingerprint $fpr, expected ${k#*:}" >&2
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
        sha="$(awk '/^SHA256:/ { s = 1; next } /^[^ ]/ { s = 0 }
            s && $3 == "main/binary-i386/Packages.xz" { print $1 }' "$WORK/$suite.Release")"
        if [ -z "$sha" ]; then
            echo "gnu: chromium: $suite: no i386 Packages index in the Release" >&2
            return 1
        fi
        get "$base/main/binary-i386/Packages.xz" "$DEBS/$suite.Packages.xz" "$sha"
        xz -dc "$DEBS/$suite.Packages.xz" > "$WORK/$suite.Packages"
        echo "$archive $suite: $(awk '/^Date:/ { sub(/^Date: /, ""); print; exit }' "$WORK/$suite.Release")"
    done
}

# Each index becomes one line per package, fields separated by tabs, and the
# suites are read in order, so that a later suite's entry replaces an earlier
# one's. A dependency is the first alternative that is a package, or else the
# first a package provides, and one of UNFOLLOWED ends the search. Versions
# are not compared: within one snapshot of a suite and its security updates,
# the archive keeps them satisfiable.
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
    awk -F '\t' -v roots="$ROOTS" -v unfollowed="$UNFOLLOWED" '
        function bare(d) {
            sub(/\(.*/, "", d)
            sub(/:[a-z0-9]*/, "", d)
            gsub(/[ \t]/, "", d)
            return d
        }
        {
            ver[$1] = $2; arc[$1] = $3; file[$1] = $4
            size[$1] = $5; sha[$1] = $6; dep[$1] = $7
            n = split($8, pv, ",")
            for (i = 1; i <= n; i++) {
                p = bare(pv[i])
                if (p != "" && !(p in provider))
                    provider[p] = $1
            }
        }
        END {
            nu = split(unfollowed, list, " ")
            for (u = 1; u <= nu; u++)
                cut[list[u]] = 1
            nq = split(roots, queue, " ")
            for (h = 1; h <= nq; h++) {
                name = queue[h]
                if (name in seen)
                    continue
                seen[name] = 1
                if (!(name in ver)) {
                    print "gnu: chromium: no package " name > "/dev/stderr"
                    bad = 1
                    continue
                }
                print name, ver[name], arc[name], file[name], size[name], sha[name]
                nc = split(dep[name], clause, ",")
                for (c = 1; c <= nc; c++) {
                    na = split(clause[c], alt, "|")
                    pick = ""
                    for (a = 1; a <= na && pick == ""; a++)
                        if (bare(alt[a]) in cut)
                            pick = "-"
                    for (a = 1; a <= na && pick == ""; a++)
                        if (bare(alt[a]) in ver)
                            pick = bare(alt[a])
                    for (a = 1; a <= na && pick == ""; a++)
                        if (bare(alt[a]) in provider)
                            pick = provider[bare(alt[a])]
                    if (pick == "") {
                        print "gnu: chromium: " name ": nothing satisfies " clause[c] > "/dev/stderr"
                        bad = 1
                    } else if (pick != "-" && !(pick in seen))
                        queue[++nq] = pick
                }
            }
            exit bad
        }' "$WORK/packages.tsv" > "$WORK/closure.txt"
    sort "$WORK/closure.txt" | awk '{ print $1, $2, $3; total += $5 }
        END { printf "%d packages, %.1f MB\n", NR, total / 1e6 }'
}

step_fetch()
{
    mkdir -p "$DEBS"
    while read -r name _ archive file _ sha; do
        get "https://snapshot.debian.org/archive/$archive/$SNAPSHOT/$file" \
            "$DEBS/${file##*/}" "$sha"
    done < "$WORK/closure.txt"
    echo "$(wc -l < "$WORK/closure.txt") packages checked"
}

# The root has the merged /usr of trixie, whose packages expect /bin, /lib
# and /sbin to be links into /usr; tar keeps the links when a package has
# files under them.
step_unpack()
{
    rm -rf "$ROOT" "$WORK/files"
    mkdir -p "$ROOT/usr/bin" "$ROOT/usr/lib" "$ROOT/usr/sbin" "$WORK/files"
    ln -s usr/bin "$ROOT/bin"
    ln -s usr/lib "$ROOT/lib"
    ln -s usr/sbin "$ROOT/sbin"
    while read -r name _ archive file _ sha; do
        deb="$DEBS/${file##*/}"
        if [ ! -f "$deb" ] || [ "$(sha256_of "$deb")" != "$sha" ]; then
            echo "gnu: chromium: $name: ${deb##*/} is missing or not the package the closure names" >&2
            return 1
        fi
        member="$(ar t "$deb" | grep '^data\.tar')"
        case "$member" in
            *.xz) unpack="xz -dc" ;;
            *.gz) unpack="gzip -dc" ;;
            *.zst) unpack="zstd -dc" ;;
            *) echo "gnu: chromium: $name: no data member" >&2; return 1 ;;
        esac
        ar p "$deb" "$member" | $unpack |
            tar -xvf - -C "$ROOT" --keep-directory-symlink > "$WORK/files/$name"
    done < "$WORK/closure.txt"
    du -sh "$ROOT"
}

# glibc_needs FILE: the glibc symbol versions FILE needs, one to a line.
glibc_needs()
{
    "$CROSS/bin/$TARGET-readelf" -V -W "$1" 2>/dev/null | awk '
        /^Version needs section/ { n = 1; next }
        /^Version / { n = 0 }
        n { for (i = 1; i <= NF; i++) if ($i ~ /^GLIBC_[0-9]/) print $i }'
}

# is_elf FILE: whether FILE begins with the ELF magic number.
is_elf()
{
    [ "$(od -An -tx1 -N4 "$1" 2>/dev/null | tr -d ' \n')" = 7f454c46 ]
}

# root_libpath: the root's library directories as a search path. Some
# libraries find others through a RUNPATH, an absolute directory such as
# libpulse's /usr/lib/i386-linux-gnu/pulseaudio, which on this host must be
# taken inside the root.
root_libpath()
{
    p="$ROOT/usr/lib/i386-linux-gnu:$ROOT/usr/lib/chromium"
    for d in $(find "$ROOT/usr/lib" -type f -name '*.so*' | while read -r f; do
            "$CROSS/bin/$TARGET-readelf" -d "$f" 2>/dev/null | sed -n \
                -e 's/.*(RUNPATH).*\[\(.*\)\]$/\1/p' -e 's/.*(RPATH).*\[\(.*\)\]$/\1/p'
        done | tr ':' '\n' | grep '^/' | sort -u); do
        p="$p:$ROOT$d"
    done
    echo "$p"
}

# Whether the file name $1 is a shared object's: it ends in .so, or in .so and
# a version of digits and dots, as libfoo.so.1.2 does, and not in the .so.TOC
# of a table of contents the package ships beside some of them.
is_so_name()
{
    case ${1##*.so} in
        '') return 0 ;;
        *[!0-9.]*) return 1 ;;
        .[0-9]*) return 0 ;;
    esac
    return 1
}

step_check()
{
    if [ ! -x "$SYSROOT/lib/ld-linux.so.2" ]; then
        echo "gnu: chromium: no sysroot; run make gnu-toolchain first" >&2
        return 1
    fi
    if [ ! -x "$ROOT/$BROWSER" ] || ! is_elf "$ROOT/$BROWSER"; then
        echo "gnu: chromium: $BROWSER is missing or not an executable ELF file" >&2
        return 1
    fi
    rootlib="$(root_libpath)"
    : > "$WORK/list.txt"
    for f in "$ROOT/$BROWSER" "$ROOT"/usr/lib/chromium/*.so*; do
        [ -e "$f" ] || continue
        [ "$f" = "$ROOT/$BROWSER" ] || is_so_name "${f##*/}" || continue
        if ! is_elf "$f"; then
            echo "gnu: chromium: ${f#"$ROOT"/} is not an ELF file" >&2
            return 1
        fi
        echo "${f#"$ROOT"/}:" >> "$WORK/list.txt"
        "$SYSROOT/lib/ld-linux.so.2" --list --library-path "$SYSROOT/lib:$rootlib" "$f" \
            >> "$WORK/list.txt" 2>&1 || echo "    the loader exited with $?" >> "$WORK/list.txt"
    done
    cat "$WORK/list.txt"
    awk -v root="$ROOT/" -v sys="$SYSROOT/lib/" '
        /=>/ && index($3, root) != 1 && index($3, sys) != 1 { print; bad = 1 }
        /not found|error while loading|exited with/ { print; bad = 1 }
        END { exit bad }' "$WORK/list.txt" > "$WORK/missing.txt" || {
        echo "gnu: chromium: libraries not found in the root or the sysroot:" >&2
        cat "$WORK/missing.txt" >&2
        return 1
    }
    echo "every library was found: glibc's in the sysroot, the rest in the root"

    awk '$1 != "libc6" { print $1 }' "$WORK/closure.txt" | while read -r name; do
        sed -e 's|^\./||' "$WORK/files/$name" | while read -r f; do
            [ -f "$ROOT/$f" ] && [ ! -h "$ROOT/$f" ] || continue
            glibc_needs "$ROOT/$f" | sed -e "s|^|$name |"
        done
    done | sort -u > "$WORK/glibc.txt"
    newest="$(awk '{ print $2 }' "$WORK/glibc.txt" | sort -u -t. -k2,2n -k3,3n | tail -n 1)"
    echo "the newest glibc symbol version the root needs is $newest; these packages"
    awk -v v="$newest" '$2 == v { print "  " $1 }' "$WORK/glibc.txt"
    echo "need it; the sysroot's glibc is $(awk '$2 == "__GLIBC__" { ma = $3 }
        $2 == "__GLIBC_MINOR__" { mi = $3 } END { print ma "." mi }' "$SYSROOT/usr/include/features.h")"

    run_browser
}

# The browser finds its data files and starts its helper processes through
# /proc/self/exe, a link to the loader when the loader is run by hand, so
# the run is in a copy of the root instead: unshare(1) makes the user root of
# new user, mount and process namespaces, the host's /dev, /proc and /sys are
# mounted in the copy, and chroot(1) starts the browser there, where the
# kernel loads the root's own /lib/ld-linux.so.2 for it. The run reports what
# happened and does not decide the step.
run_browser()
{
    run="$WORK/run"
    rm -rf "$run"
    mkdir -p "$run"
    cp -a "$ROOT" "$run/root"
    mkdir -p "$run/root/dev" "$run/root/proc" "$run/root/sys" "$run/root/tmp"
    cat > "$run/root/tmp/page.html" <<'EOF'
<!DOCTYPE html>
<html><body><p id="out">not run</p>
<script>document.getElementById("out").textContent = "script ran: " + 6 * 7;</script>
</body></html>
EOF
    set +e
    # shellcheck disable=SC2016
    HOME=/tmp unshare --map-root-user --mount --pid --fork sh -c '
        mount --rbind /dev "$1/dev" && mount -t proc proc "$1/proc" &&
        mount --rbind /sys "$1/sys" &&
        chroot "$1" "/$2" --no-sandbox --headless --dump-dom \
            --user-data-dir=/tmp/profile file:///tmp/page.html' \
        sh "$run/root" "$BROWSER" > "$run/dom.txt" 2> "$run/stderr.txt"
    status=$?
    set -e
    rm -rf "$run/root"
    echo "run: exit status $status"
    if grep -q 'script ran: 42' "$run/dom.txt"; then
        echo "run: the dumped DOM has the text the page's script wrote:"
        cat "$run/dom.txt"
    else
        echo "run: no DOM with the script's text; the end of its standard error:"
        tail -n 20 "$run/stderr.txt"
    fi
}

if [ $# -eq 0 ]; then
    rm -rf "$WORK" "$ROOT"
    set -- index resolve fetch unpack check
fi
mkdir -p "$LOG"
for step in "$@"; do
    case "$step" in
        index|resolve|fetch|unpack|check) ;;
        *) echo "gnu: chromium: no step named $step" >&2; exit 2 ;;
    esac
    echo "gnu: chromium: $step"
    log="$LOG/chromium-$step.log"
    t0=$(date +%s)
    set +e
    ( set -e; "step_$step" ) > "$log" 2>&1
    status=$?
    set -e
    if [ $status -ne 0 ]; then
        tail -n 40 "$log" >&2
        echo "gnu: chromium: $step: failed; the log is $log" >&2
        exit 1
    fi
    case "$step" in
        resolve|check) cat "$log" ;;
    esac
    echo "gnu: chromium: $step: done in $(($(date +%s) - t0)) s"
done
