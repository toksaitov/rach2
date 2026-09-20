#!/usr/bin/env sh
#
# srclists.sh: the two cppcheck source lists, written once and read by both
# the style suite (tools/verify/check.sh, at the normal level) and the slow
# analysers (tools/verify/analyze.sh, at the exhaustive one). The lists live
# here so the two runs cannot come to check different files.
#
# Sourced, not run: srclists REPO OUTDIR PREFIX writes OUTDIR/PREFIXsrcs.txt,
# the freestanding kernel sources and the guest programs that share its
# headers, and OUTDIR/PREFIXhostsrcs.txt, the programs built for the host.
srclists() {
    _sl_repo="$1"
    _sl_out="$2"
    _sl_pre="$3"

    find "$_sl_repo/rach2/sys" -name '*.c' >"$_sl_out/${_sl_pre}srcs.txt"
    echo "$_sl_repo/rach2/kernel.c" >>"$_sl_out/${_sl_pre}srcs.txt"
    ls "$_sl_repo"/tests/*probe.c >>"$_sl_out/${_sl_pre}srcs.txt" 2>/dev/null || true
    ls "$_sl_repo"/tests/fdstale.c "$_sl_repo"/tests/idreport.c \
        >>"$_sl_out/${_sl_pre}srcs.txt" 2>/dev/null || true
    ls "$_sl_repo"/utils/pwauth.c "$_sl_repo"/utils/pwauth.h "$_sl_repo"/utils/passwd.c \
       "$_sl_repo"/utils/adduser.c "$_sl_repo"/utils/login.c \
        >>"$_sl_out/${_sl_pre}srcs.txt" 2>/dev/null || true
    ls "$_sl_repo"/tests/host/pwauth_hosttest.c "$_sl_repo"/tests/musl/*.c \
        >>"$_sl_out/${_sl_pre}srcs.txt" 2>/dev/null || true
    ls "$_sl_repo"/krffs/krffs_reader.c "$_sl_repo"/krffs/krffs_reader.h \
       "$_sl_repo"/krffs/readfile.krffs.c >>"$_sl_out/${_sl_pre}srcs.txt" 2>/dev/null || true

    # The host programs, in their own dialect and on a 64-bit host: the tools,
    # the runner, the checkers and the fixtures under tests/system, and the
    # KRFFS tools without their vendored terminal library.
    {
        find "$_sl_repo/tools" -name '*.c' -o -name '*.h'
        find "$_sl_repo/tests/system" "$_sl_repo/krffs" -path '*/vendor' -prune \
             -o -path '*/vs-dokany' -prune -o \( -name '*.c' -o -name '*.h' \) -print
    } 2>/dev/null | sort >"$_sl_out/${_sl_pre}hostsrcs.txt"
}
