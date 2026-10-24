# bootstrap.sh: build a version of etude on the machine it is for, from its
# source, into that machine's cache of versions, or find it built there.
#
# provision_version.sh sends this file to a POSIX sh that reads it on its
# standard input, after the assignments of ETUDE_NONCE, ETUDE_SOURCE, ETUDE_CC
# and ETUDE_CACHE and before the definitions of etude_files and etude_unpack,
# and ends the script with the call of etude_main. The file defines functions
# and runs nothing, so a script cut short anywhere runs nothing but
# assignments, and provision_version.sh reads it as well, for the functions
# both ends share.
# tools/etude/Readme.md describes the cache.
# shellcheck shell=sh

# etude_fail WHAT: report WHAT on stderr and end with status 1.
etude_fail()
{
    echo "etude provision: $*" >&2
    exit 1
}

# etude_hash [FILE]: print the SHA-256 of FILE, or of standard input, in
# hexadecimal, by the first of sha256sum, shasum and openssl that the machine
# has. Fails when the tool fails or prints anything else.
etude_hash()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$@"
    else
        openssl dgst -sha256 -r "$@"
    fi | {
        read -r sum _ || exit 1
        case $sum in
            *[!0-9a-f]*|'') exit 1 ;;
        esac
        [ "${#sum}" -eq 64 ] || exit 1
        printf '%s\n' "$sum"
    }
}

# etude_select: copy from standard input to standard output the paths that
# belong to etude's source: the C sources, headers and make files under
# tools/etude but its fixtures, the native collection's C, headers and make
# files, the schemas its programs read, and the SHA-256 of utils/.
etude_select()
{
    while read -r path; do
        case $path in
            tools/etude/fixtures/*) ;;
            tools/etude/*.c|tools/etude/*.h|tools/etude/*.mk) printf '%s\n' "$path" ;;
            tools/etude/schemas/*.schema.json) printf '%s\n' "$path" ;;
            tests/native/*.c|tests/native/*.h|tests/native/*.mk) printf '%s\n' "$path" ;;
            utils/sha256.c|utils/sha256.h) printf '%s\n' "$path" ;;
        esac
    done
}

# etude_list DIR: for each path on standard input, of a file under DIR,
# print the line `SHA256  PATH`. The digest of the list is the source's.
etude_list()
(
    cd "$1" || exit 1
    paths=
    set --
    while IFS= read -r path; do
        case $path in
            *[!A-Za-z0-9._/-]*|'') exit 1 ;;
        esac
        paths="$paths$path|"
        set -- "$@" "$path"
    done
    [ "$#" -gt 0 ] || exit 0
    if command -v sha256sum >/dev/null 2>&1; then
        output=$(sha256sum "$@") || exit 1
    elif command -v shasum >/dev/null 2>&1; then
        output=$(shasum -a 256 "$@") || exit 1
    else
        # Older OpenSSL versions need one digest invocation per file.
        for path do
            sum=$(etude_hash "$path") || exit 1
            printf '%s  %s\n' "$sum" "$path"
        done
        exit
    fi
    printf '%s\n' "$output" | LC_ALL=C awk -v paths="$paths" '
        BEGIN { count = split(paths, names, "[|]") - 1 }
        {
            sum = substr($0, 1, 64)
            if (NR > count || length(sum) != 64 || sum ~ /[^0-9a-f]/ ||
                substr($0, 65, 2) != "  " || substr($0, 67) != names[NR])
                bad = 1
        }
        END { if (bad || NR != count) exit 1 }
        { print }
    '
)

# etude_text DIR END: refuse NUL bytes, missing final newlines and a line
# equal to END in each listed source. One wc records the file boundaries and
# one od reads their concatenation. Both must finish successfully.
etude_text()
(
    cd "$1" || exit 1
    marker=$2
    paths=
    set --
    while IFS= read -r path; do
        case $path in
            *[!A-Za-z0-9._/-]*|'') exit 1 ;;
        esac
        paths="$paths$path|"
        set -- "$@" "./$path"
    done
    [ "$#" -gt 0 ] || exit 0
    (
        LC_ALL=C wc -c "$@" || exit 1
        printf 'data\n'
        od -An -v -tu1 "$@" || exit 1
        printf 'complete\n'
    ) | LC_ALL=C awk -v marker="$marker" -v paths="$paths" '
        BEGIN { count = split(paths, names, "[|]") - 1 }
        function fail(reason) {
            print "etude provision: " path ": " reason
            bad = 1
            exit 1
        }
        function advance() {
            current++
            while (current <= count && sizes[current] == 0) current++
            path = names[current]
            left = sizes[current]
            width = 0
            matchline = 1
        }
        !data && /^data$/ {
            if (rows != count + (count > 1)) fail("invalid size records")
            data = 1
            advance()
            next
        }
        !data {
            rows++
            path = names[rows]
            if (NF != 2 || $1 !~ /^[0-9]+$/ || length($1) > 15)
                fail("invalid size record")
            if (rows <= count) {
                if ($2 != "./" path) fail("invalid size path")
                sizes[rows] = $1 + 0
                total += sizes[rows]
            } else if (count <= 1 || rows != count + 1 ||
                       $2 != "total" || $1 != total) fail("invalid size total")
            next
        }
        /^complete$/ {
            if (complete || current <= count) fail("incomplete byte stream")
            complete = 1
            next
        }
        {
            if (complete) fail("invalid byte stream")
            if (!NF) next
            for (i = 1; i <= NF; i++) {
                if (current > count || $i !~ /^[0-9]+$/ || $i > 255)
                    fail("invalid byte stream")
                last = $i + 0
                if (!last) fail("the file has a NUL byte")
                if (last == 10) {
                    if (matchline && width == length(marker))
                        fail("a line is " marker)
                    width = 0
                    matchline = 1
                } else {
                    width++
                    if (matchline &&
                        substr(marker, width, 1) != sprintf("%c", last))
                        matchline = 0
                }
                if (--left == 0) {
                    if (last != 10) fail("the last line has no newline")
                    advance()
                }
            }
        }
        END { if (bad || !data || !complete) exit 1 }
    ' >&2
)

# etude_pack DIR: encode the validated source list with one text reader.
# A colon distinguishes every source line from a file or completion record.
etude_pack()
{
    LC_ALL=C awk -v root="$1" '
        {
            file = root "/" $0
            print "file " $0
            while ((status = getline line < file) > 0) print ":" line
            if (status < 0 || close(file) != 0) { bad = 1; exit 1 }
        }
        END { if (bad) exit 1; print "complete" }
    '
}

# etude_unpack_text DIR: write a complete encoded source into private DIR.
# The caller verifies its source digest before invoking the compiler.
etude_unpack_text()
{
    LC_ALL=C awk -v root="$1" '
        function finish() {
            if (file && close(file) != 0) { bad = 1; exit 1 }
            file = ""
        }
        /^file / && !complete {
            finish()
            path = substr($0, 6)
            if (path == "" || path ~ /[^A-Za-z0-9._\/-]/ ||
                path ~ /^\// || path ~ /(^|\/)\.\.?($|\/)/) { bad = 1; exit 1 }
            file = root "/" path
            printf "%s", "" > file
            next
        }
        /^:/ && file && !complete { print substr($0, 2) > file; next }
        /^complete$/ && !complete { finish(); complete = 1; next }
        { bad = 1; exit 1 }
        END { finish(); if (bad || !complete) exit 1 }
    '
}

# etude_platform: print the platform and the machine, the platform named as
# tools/setup/prereq.sh names it: macos, linux, rach2 for a Linux whose
# release ends in -rach2, or windows for the shell of MSYS2.
etude_platform()
{
    case $(uname -s) in
        Darwin) p=macos ;;
        Linux)
            case $(uname -r) in
                *-rach2) p=rach2 ;;
                *) p=linux ;;
            esac ;;
        MINGW*|MSYS*) p=windows ;;
        *) return 1 ;;
    esac
    printf '%s %s\n' "$p" "$(uname -m)"
}

# etude_check ROOT RECORD: succeed when RECORD is a whole record of a version
# in the cache at ROOT, and print the version's directory. Its key must be
# the digest of its first five lines, its build directory must exist, and
# each program it lists must have the digest it gives.
etude_check()
{
    sum=$(sed -n '1,5p' "$2" | etude_hash) || return 1
    [ "$(sed -n 's/^key //p' "$2")" = "$sum" ] || return 1
    dir=$(sed -n 's/^build //p' "$2")
    case $dir in
        builds/"$sum".*) ;;
        *) return 1 ;;
    esac
    [ -d "$1/$dir/bin" ] || return 1
    n=0
    while read -r word name want; do
        [ "$word" = program ] || continue
        [ "$(etude_hash "$1/$dir/bin/$name")" = "$want" ] || return 1
        n=$((n + 1))
    done < "$2"
    [ "$n" -gt 0 ] || return 1
    printf '%s\n' "$1/$dir"
}

# etude_reuse ROOT KEY: report the published version KEY of the cache at
# ROOT as reused, or fail when its record or its programs are damaged.
etude_reuse()
{
    dir=$(etude_check "$1" "$1/versions/$2") ||
        etude_fail "version $2 in $1 is damaged; a person removes it"
    echo "@etude-version $ETUDE_NONCE reused $2 $dir"
}

# etude_abandon: remove the build directory of this run unless it was
# published, so that a build that failed or was stopped leaves nothing.
etude_abandon()
{
    [ -n "$published" ] || [ -z "$build" ] || rm -rf "$build"
}

# etude_main: build the version of the appended source for this machine, or
# find it built, and print `@etude-version NONCE built|reused KEY DIR`. The
# build's make runs without MAKEFLAGS, MFLAGS and MAKELEVEL, so that a make
# that runs this script gives it neither jobs nor variables the key omits.
etude_main()
{
    platform=$(etude_platform) || etude_fail "unknown system $(uname -s)"
    cc=${ETUDE_CC:-cc}
    exe=
    flags='-std=c99 -O2 -pthread'
    # shellcheck disable=SC2016 # make expands the variable
    src='SRC=$(ETUDE_SRC)'
    defs=-D_DEFAULT_SOURCE
    libs=
    root=${ETUDE_CACHE:-}
    case $platform in
        windows*)
            cc=${ETUDE_CC:-/ucrt64/bin/gcc}
            exe=.exe
            flags='-std=c99 -O2 -static'
            # shellcheck disable=SC2016 # make expands the variable
            src='SRC=$(ETUDE_WIN_SRC)'
            defs=-D_WIN32_WINNT=0x0A00
            libs='-lws2_32 -lbcrypt'
            if [ -z "$root" ]; then
                [ -n "${LOCALAPPDATA:-}" ] || etude_fail 'LOCALAPPDATA is not set'
                root=$(cygpath -u "$LOCALAPPDATA") || etude_fail "cygpath cannot read $LOCALAPPDATA"
                root=$root/etude
            fi ;;
        rach2*)
            flags='-std=c99 -static -D__rach2__'
            # shellcheck disable=SC2016 # make expands the variable
            src='SRC=$(ETUDE_RACH2_SRC)' ;;
    esac
    if [ -z "$root" ]; then
        [ -n "${HOME:-}" ] || etude_fail 'HOME is not set'
        root=${XDG_CACHE_HOME:-$HOME/.cache}/etude
    fi
    command -v make >/dev/null 2>&1 || etude_fail "no make on $platform"
    command -v mktemp >/dev/null 2>&1 || etude_fail "no mktemp on $platform"
    # shellcheck disable=SC2086 # CC is a command line
    ident=$($cc -v 2>&1) || etude_fail "cannot run $cc -v"
    compiler=$(printf '%s\n' "$ident" | etude_hash) || etude_fail 'no SHA-256 tool'
    identity="etude-version 1
source $ETUDE_SOURCE
platform $platform
compiler $compiler
flags $flags"
    key=$(printf '%s\n' "$identity" | etude_hash) || etude_fail 'no SHA-256 tool'
    record=$root/versions/$key
    if [ -f "$record" ]; then
        etude_reuse "$root" "$key"
        return
    fi

    published=
    build=
    trap etude_abandon EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    mkdir -p "$root/versions" "$root/builds" || etude_fail "cannot make $root"
    build=$(mktemp -d "$root/builds/$key.XXXXXX") || etude_fail "cannot make a build directory in $root"
    if ! mkdir "$build/src" "$build/bin" || ! etude_unpack "$build/src"; then
        etude_fail "cannot write the source in $build"
    fi
    etude_files | etude_list "$build/src" > "$build/sources" || etude_fail 'cannot read the source'
    [ "$(etude_hash "$build/sources")" = "$ETUDE_SOURCE" ] ||
        etude_fail "the source arrived damaged: its digest is not $ETUDE_SOURCE"
    (cd "$build/src" && unset MAKEFLAGS MFLAGS MAKELEVEL && make -f tools/etude/version.mk OUT=../bin EXE="$exe" CC="$cc" CFLAGS="$flags" "$src" FLAGS="$defs" LIBS="$libs") >&2 ||
        etude_fail "the build of $key failed"
    {
        printf '%s\n' "$identity"
        echo "key $key"
        echo "cc $cc"
        echo "build builds/${build##*/}"
        for p in "$build"/bin/*; do
            sum=$(etude_hash "$p") || etude_fail "cannot read $p"
            echo "program ${p##*/} $sum"
        done
    } > "$build/version.new" || etude_fail "cannot write the record of $key"
    mv "$build/version.new" "$build/version" || etude_fail "cannot write the record of $key"
    etude_check "$root" "$build/version" > /dev/null || etude_fail "the build of $key made no whole version"
    if ln "$build/version" "$record" 2>/dev/null; then
        published=1
        echo "@etude-version $ETUDE_NONCE built $key $build"
    elif [ -f "$record" ]; then
        etude_reuse "$root" "$key"
    else
        etude_fail "cannot publish $record"
    fi
}
