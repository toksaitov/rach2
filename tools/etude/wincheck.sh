#!/bin/sh
# wincheck.sh: build etude.exe, the Windows build of etude, with
# etude-volume.exe and the fixtures of etude's Windows adapter, executor and
# supervisor and of its verbs that have a Windows implementation, with
# MinGW-w64, check that the programs import no MSYS or Cygwin runtime, and run
# the fixtures on Windows.
#
# Usage: sh tools/etude/wincheck.sh [FIXTURE...]
#
# It runs in MSYS2's shell on Windows, or in WSL, where it runs the programs
# on the Windows of the same machine and builds them in a directory under
# that Windows's TEMP, since a Windows program cannot write a WSL account's
# home. ETUDE_WIN_CC names the compiler; by default it is
# x86_64-w64-mingw32-gcc where that is on the path, then MSYS2's UCRT64 gcc.
# The sources are copied to the build directory, in the layout of the tree,
# and compiled there; the directory is removed on every exit.
# A machine with no Windows fails the check.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
src=$repo/tools/etude

work=
pe=
# bash 3.2, the Mac's sh, gives an EXIT trap a status of 0 when `set -u`
# stops a script that also has `set -e`, so a run that reached no success
# exit fails here.
finished=
status=0
trap 'status=$?; rm -rf ${work:+"$work"} ${pe:+"$pe"}; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; exit "$status"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

case $(uname -s) in
MINGW* | MSYS* | UCRT* | CLANG*)
    work=$(mktemp -d "${TMPDIR:-/tmp}/etude-wincheck.XXXXXX")
    ;;
*)
    cmd=${ETUDE_WIN_CMD:-/mnt/c/Windows/System32/cmd.exe}
    if [ ! -x "$cmd" ] || ! command -v wslpath >/dev/null 2>&1; then
        echo "wincheck: no Windows to run the fixtures on" >&2
        exit 1
    fi
    wintemp=$(cd / && "$cmd" /c 'echo %TEMP%' 2>/dev/null | tr -d '\r')
    work=$(mktemp -d "$(wslpath -u "$wintemp")/etude-wincheck.XXXXXX")
    ;;
esac

cc=${ETUDE_WIN_CC:-}
if [ -z "$cc" ]; then
    if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
        cc=x86_64-w64-mingw32-gcc
    elif [ -x /mnt/c/msys64/ucrt64/bin/gcc.exe ]; then
        cc=/mnt/c/msys64/ucrt64/bin/gcc.exe
    elif command -v gcc >/dev/null 2>&1; then
        cc=gcc
    else
        echo "wincheck: no MinGW-w64 compiler; set ETUDE_WIN_CC" >&2
        exit 1
    fi
fi
case $cc in
*/*) PATH=$(dirname "$cc"):$PATH ;;
esac

# The import check runs where the shell runs, built by the host's compiler,
# or by MinGW's in MSYS2, where the host has none.
pe=$(mktemp -d "${TMPDIR:-/tmp}/etude-pecheck.XXXXXX")
if command -v cc >/dev/null 2>&1; then
    cc -std=c99 -O2 -o "$pe/pecheck" "$src/win/pecheck.c"
    pecheck=$pe/pecheck
else
    cp "$src/win/pecheck.c" "$work/"
    (cd "$work" && "$cc" -std=c99 -O2 -o pecheck.exe pecheck.c)
    pecheck=$work/pecheck.exe
fi

# copy DIR: copy the sources to DIR/tools/etude and DIR/utils.
copy() {
    e=$1/tools/etude
    mkdir -p "$e/win" "$e/posix" "$e/guest" "$e/volume" "$e/include/etude" "$e/schemas" "$1/utils" "$1/tests/host/compat"
    cp "$repo"/utils/sha256.h "$repo"/utils/sha256.c "$1/utils/"
    cp "$src"/*.c "$src"/*.h "$src"/sources.mk "$src"/version.mk "$e/"
    cp "$src"/posix/*.c "$src"/posix/*.h "$e/posix/"
    cp "$src"/guest/wire.c "$src"/guest/wire.h "$e/guest/"
    cp "$src"/win/*.c "$src"/win/*.h "$e/win/"
    cp "$src"/volume/volume.h "$src"/volume/volume.c "$src"/volume/etude_volume.c "$e/volume/"
    cp "$src"/include/etude/*.h "$e/include/etude/"
    cp "$src"/schemas/history.schema.json "$e/schemas/"
    cp "$src"/schemas/history.schema.json "$e/win/"
    cp "$repo"/tests/host/compat/output_sinks.[ch] "$1/tests/host/compat/"
}

# compile DIR FLAGS: build the programs in DIR/tools/etude/win, etude.exe
# through version.mk with the sources, definitions and libraries that
# bootstrap.sh gives a Windows machine. A Windows program started from WSL
# reads the shell's standard input, so it gets none.
compile() {
    (
        cd "$1/tools/etude/win"
        files='winsys.c winfmt.c files.c'
        executor='exec.c ../capture.c winexec.c winsys.c winfmt.c files.c'
        # shellcheck disable=SC2086
        "$cc" $2 -I ../include -I ../../../utils -I ../../../tests/host/compat -o winfixture.exe winfixture.c winexecfix.c \
            winledgerfix.c winrunfix.c wintoolfix.c winholdfix.c winportsfix.c ../ledger.c ../watchdog.c \
            ../supervise.c ../history_json.c ../posix/qemu.c ../posix/store.c ../qemu_profile.c ../processor.c wincollectionfix.c $executor ../../../utils/sha256.c ../../../tests/host/compat/output_sinks.c -lws2_32 -lbcrypt
        # shellcheck disable=SC2086
        "$cc" $2 -municode -o wintarget.exe wintarget.c -lws2_32 -lbcrypt
        # shellcheck disable=SC2086
        "$cc" $2 -I ../include -I ../../../utils -I ../volume -o winstand.exe winstand.c ../volume/volume.c $files -lws2_32 -lbcrypt
        # shellcheck disable=SC2086
        "$cc" $2 -o image-command.exe image_command.c hold.c winfmt.c
        # shellcheck disable=SC2086
        "$cc" $2 -I ../include -I ../../../utils -o etude-volume.exe ../volume/etude_volume.c ../volume/volume.c $files -lws2_32 -lbcrypt
    ) </dev/null
    (
        cd "$1"
        # shellcheck disable=SC2016 # make expands the source-list variable
        make -f tools/etude/version.mk OUT=tools/etude/win EXE=.exe CC="$cc" CFLAGS="$2" \
            'SRC=$(ETUDE_WIN_SRC)' FLAGS=-D_WIN32_WINNT=0x0A00 LIBS='-lws2_32 -lbcrypt' all
    ) </dev/null
}

flags='-std=c99 -O2 -D_WIN32_WINNT=0x0A00 -static'

# fixtures DIR LOG [NAME...]: run the fixtures built in DIR, writing LOG.
fixtures() {
    dir=$1
    log=$2
    shift 2
    (cd "$dir/tools/etude/win" && ./winfixture.exe "$@") </dev/null >"$log" 2>&1
}

# Build the sources, require the programs' imports to be clean, and run the
# fixtures named, or all of them.
mkdir "$work/ok"
copy "$work/ok"
compile "$work/ok" "$flags -Wall -Wextra -Werror"
w=$work/ok/tools/etude/win
"$pecheck" "$w/winfixture.exe" "$w/wintarget.exe" "$w/winstand.exe" "$w/etude.exe" \
    "$w/etude-volume.exe" "$w/image-command.exe"
if fixtures "$work/ok" "$work/ok.log" "$@"; then
    cat "$work/ok.log"
    finished=1
    exit 0
fi
cat "$work/ok.log"
exit 1
