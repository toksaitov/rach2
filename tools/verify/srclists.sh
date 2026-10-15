#!/usr/bin/env sh
#
# srclists.sh: the cppcheck source lists, defined once for the style suite
# (tools/verify/check.sh, at the normal level) and the slow analysers
# (tools/verify/analyze.sh, at the exhaustive level), so that the two runs
# check the same files. The file is sourced, not run.

# srclists REPO OUTDIR PREFIX: write three lists of the sources under REPO.
# OUTDIR/PREFIXsrcs.txt has the freestanding kernel sources of the i686 build
# and the guest programs that share its headers; OUTDIR/PREFIXsrcs64.txt has
# the kernel sources of the x86-64 build, which are checked again as 64-bit
# code; OUTDIR/PREFIXhostsrcs.txt has the programs built for the host: the
# tools, the runner, the checkers and fixtures under tests/system, and the
# KRFFS tools without their vendored terminal library. A guest file that is
# absent is left out without an error.
srclists() {
    _sl_repo="$1"
    _sl_out="$2"
    _sl_pre="$3"

    find "$_sl_repo/rach2/sys" -name '*.c' ! -path '*/arch/x86_64/*' >"$_sl_out/${_sl_pre}srcs.txt"
    echo "$_sl_repo/rach2/kernel.c" >>"$_sl_out/${_sl_pre}srcs.txt"
    find "$_sl_repo/rach2/sys" -name '*.c' ! -path '*/arch/i686/*' >"$_sl_out/${_sl_pre}srcs64.txt"
    echo "$_sl_repo/rach2/kernel.c" >>"$_sl_out/${_sl_pre}srcs64.txt"
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

    {
        find "$_sl_repo/tools" -name '*.c' -o -name '*.h'
        find "$_sl_repo/tests/system" "$_sl_repo/krffs" -path '*/vendor' -prune \
             -o -path '*/vs-dokany' -prune -o \( -name '*.c' -o -name '*.h' \) -print
    } 2>/dev/null | sort >"$_sl_out/${_sl_pre}hostsrcs.txt"
}
