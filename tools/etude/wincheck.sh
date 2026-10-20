#!/bin/sh
# wincheck.sh: build etude's Windows adapter, executor and supervisor,
# etude-tooling and their fixtures with MinGW-w64, check that the programs
# import no MSYS or Cygwin runtime, and run the fixtures on Windows.
#
# Usage: sh tools/etude/wincheck.sh [FIXTURE...]
#        sh tools/etude/wincheck.sh --mutate | --texts
#
# It runs in MSYS2's shell on Windows, or in WSL, where it runs the programs
# on the Windows of the same machine and builds them in a directory under
# that Windows's TEMP, since a Windows program cannot write a WSL account's
# home. ETUDE_WIN_CC names the compiler; by default it is
# x86_64-w64-mingw32-gcc where that is on the path, then MSYS2's UCRT64 gcc.
# The sources are copied to the build directory, in the layout of the tree,
# and compiled there; the directory is removed on every exit.
# A machine with no Windows fails the check.
#
# --mutate proves the fixtures: it runs them on the unmodified sources, then
# puts each fault below into a copy of the sources and requires the fixtures
# named for it to fail, then runs them on the restored sources again.
# --texts checks that each fault's text is once in its source, and builds and
# runs nothing.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
src=$repo/tools/etude

# The faults: NAME|FILE|TEXT|REPLACEMENT|FIXTURES. FILE is under tools/etude;
# TEXT is a whole line of FILE, found as a fixed string, and REPLACEMENT the
# line put in its place.
faults() {
    cat <<'EOF'
resume-early|win/winexec.c|    refused = sp->registered != NULL && sp->registered(sp->ctx, c, &c->ident) != 0;|    ResumeThread(thread); refused = sp->registered != NULL && sp->registered(sp->ctx, c, &c->ident) != 0;|suspended-start
no-kill-on-close|win/winexec.c|        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE \| JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;|        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;|controller-death
breakaway-ok|win/winexec.c|        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE \| JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;|        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE \| JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION \| JOB_OBJECT_LIMIT_BREAKAWAY_OK;|breakaway
main-only-time|win/winexec.c|    u->user_us = (uint64_t)a.BasicInfo.TotalUserTime.QuadPart / 10u;|    { FILETIME f[4]; GetProcessTimes(c->process, \&f[0], \&f[1], \&f[2], \&f[3]); u->user_us = ((uint64_t)f[3].dwHighDateTime << 32 \| f[3].dwLowDateTime) / 10u; }|accounting
stop-first-only|win/winexec.c|    if (!TerminateJobObject(c->job, ETUDE_WIN_STOP_STATUS))|    if (!TerminateProcess(c->process, ETUDE_WIN_STOP_STATUS))|stop
inherit-all|win/winexec.c|    si.lpAttributeList = l->attrs;|    si.lpAttributeList = NULL;|handles
end-at-first-exit|win/winexec.c|    if (!c->main_ended \|\| !c->out.eof \|\| !c->err.eof)|    if (c->main_ended) { c->ended = 1; return 1; } if (!c->main_ended \|\| !c->out.eof \|\| !c->err.eof)|late-output
quote-backslash|win/winfmt.c|            slashes = slashes * 2 + (*p == '"');|            slashes = slashes + (*p == '"');|arguments
env-dup-case|win/winfmt.c|        if (namecmp(vars[i - 1], vars[i]) == 0)|        if (strcmp(vars[i - 1], vars[i]) == 0)|environment
cancel-ignored|win/exec.c|    etude_win_stop(p->child);|    (void)p;|exec-attempts
no-end-stop|win/exec.c|        etude_win_stop(c);|        (void)c;|exec-attempts
refusal-ignored|win/exec.c|    return p->reg_error != 0;|    return 0;|exec-unrun
child-unrecorded|win/exec.c|            if (p->child_reg != NULL \&\&|            if (0 \&\&|ledger
verdict-on-windows|ledger.c|    if (strcmp(c->system, "win32") != 0 \&\& !verdict_confirmed(l, c->id)) {|    if (!verdict_confirmed(l, c->id)) {|ledger
serial-per-supervisor|win/winexec.c|    s->serial = (unsigned long)InterlockedIncrement(\&started);|    s->serial++;|tooling-job
sample-ended|win/exec.c|    if (p->child->main_ended \|\| WaitForSingleObject(p->child->process, 0) == WAIT_OBJECT_0)|    if (0)|exec-sample
sample-no-time|win/exec.c|    *cpu_us = u.user_us + u.kernel_us;|    *cpu_us = 0;|exec-sample tooling-sample
wait-zero-unserved|win/exec.c|        if (polled \&\& left == 0)|        if (left == 0 \&\& polled >= 0)|exec-poll
poll-zero-unread|win/winexec.c|            slice = 0;|            return 0;|exec-poll
path-case|tooling.c|#define name_cmp  _strnicmp|#define name_cmp  strncmp|tooling-made
text-outputs|tooling.c|    if ((f = fopen(names, "wb")) == NULL)|    if ((f = fopen(names, "w")) == NULL)|tooling-job
text-stdout|etude_tooling.c|    _setmode(_fileno(stdout), _O_BINARY);|    (void)0;|tooling-job
unchecked-digest|tooling.c|    if (file_sha256(part, hex) != 0 \|\| strcmp(hex, m->image_sha256) != 0) {|    if (file_sha256(part, hex) != 0) {|tooling-digest
unprovisioned|tooling.c|    if (!b->provisioned \|\| b->failed[0] != '\0')|    if (b->failed[0] == 'x')|tooling-provision
status-unchecked|tooling.c|            if (sscanf(data, "%d", \&status) != 1 \|\| status != res->status) {|            if (sscanf(data, "%d", \&status) != 1) {|tooling-lie
no-outputs|tooling.c|        if (run_program(site->envp, out, res->why, sizeof(res->why)) != 0)|        if (0)|tooling-job
no-deadline|tooling.c|            etude_exec_cancel(b->p);|            (void)b;|tooling-late
no-bootindex|tooling.c|    disk(l, "root", root, "qcow2", NULL, ",bootindex=0");|    disk(l, "root", root, "qcow2", NULL, "");|tooling-line
status-passes|tooling.c|    if (s->done \&\& !res->making \&\& i == ETUDE_TOOLING_JOB \&\& res->status != 0) {|    if (0) {|tooling-status
EOF
}

# field LINE N: the Nth field of a fault line, with `\|` and `\&` read as `|`
# and `&`.
field() {
    printf '%s\n' "$1" | sed 's/\\|/\x01/g; s/\\&/\x02/g' | cut -d'|' -f"$2" |
        sed 's/\x01/|/g; s/\x02/\&/g'
}

# count FILE TEXT: how many lines of FILE are TEXT. The texts reach awk
# through its environment, where it reads no escape sequences.
count() {
    T=$2 awk '$0 == ENVIRON["T"] { n++ } END { print n + 0 }' "$1"
}

if [ "${1:-}" = --texts ]; then
    bad=0
    faults | while IFS= read -r f; do
        name=$(field "$f" 1)
        n=$(count "$src/$(field "$f" 2)" "$(field "$f" 3)")
        if [ "$n" -ne 1 ]; then
            echo "wincheck: $name: its text is $n times in $(field "$f" 2)" >&2
            exit 1
        fi
    done || bad=1
    [ "$bad" -eq 0 ] || exit 1
    echo "wincheck: result: pass: every fault's text is once in its source"
    exit 0
fi

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
    mkdir -p "$e/win" "$e/include/etude" "$e/schemas" "$1/utils"
    cp "$repo"/utils/sha256.h "$repo"/utils/sha256.c "$1/utils/"
    cp "$src"/capture.h "$src"/capture.c "$src"/ledger.c "$src"/tooling.c "$src"/etude_tooling.c \
        "$src"/history.c "$src"/history_json.h "$src"/history_json.c "$src"/history_collect.c \
        "$src"/watchdog.c "$src"/supervise.c "$e/"
    cp "$src"/include/etude/*.h "$e/include/etude/"
    cp "$src"/schemas/history.schema.json "$e/schemas/"
    cp "$src"/win/winexec.h "$src"/win/winexec.c "$src"/win/winsys.c "$src"/win/winfmt.h \
        "$src"/win/winfmt.c "$src"/win/exec.c "$src"/win/noqemu.c "$src"/win/wintarget.c \
        "$src"/win/winfixture.h "$src"/win/winfixture.c "$src"/win/winexecfix.c \
        "$src"/win/winledgerfix.c "$src"/win/wintoolfix.c "$src"/win/winstand.c "$e/win/"
}

# compile DIR FLAGS: build the programs in DIR/tools/etude/win. A Windows
# program started from WSL reads the shell's standard input, so it gets none.
compile() {
    (
        cd "$1/tools/etude/win"
        executor='exec.c ../capture.c winexec.c winsys.c winfmt.c'
        history='../history.c ../history_json.c ../history_collect.c ../../../utils/sha256.c'
        # shellcheck disable=SC2086
        "$cc" $2 -I ../include -I ../../../utils -o winfixture.exe winfixture.c winexecfix.c \
            winledgerfix.c wintoolfix.c ../ledger.c ../watchdog.c ../supervise.c \
            noqemu.c $executor ../../../utils/sha256.c -lws2_32
        # shellcheck disable=SC2086
        "$cc" $2 -municode -o wintarget.exe wintarget.c -lws2_32
        # shellcheck disable=SC2086
        "$cc" $2 -o winstand.exe winstand.c
        # shellcheck disable=SC2086
        "$cc" $2 -I ../include -o etude-tooling.exe ../etude_tooling.c ../tooling.c $executor \
            $history -lws2_32
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

# pristine NAME...: build the unmodified sources and require the fixtures
# named, or all of them, to pass, and the programs' imports to be clean.
pristine() {
    rm -rf "$work/ok"
    mkdir "$work/ok"
    copy "$work/ok"
    compile "$work/ok" "$flags -Wall -Wextra -Werror"
    w=$work/ok/tools/etude/win
    "$pecheck" "$w/winfixture.exe" "$w/wintarget.exe" "$w/winstand.exe" "$w/etude-tooling.exe"
    if fixtures "$work/ok" "$work/ok.log" "$@"; then
        cat "$work/ok.log"
        return 0
    fi
    cat "$work/ok.log"
    return 1
}

if [ "${1:-}" != --mutate ]; then
    pristine "$@"
    finished=1
    exit 0
fi

echo "wincheck: the unmodified sources"
pristine
bad=0
faults >"$work/faults"
while IFS= read -r f; do
    name=$(field "$f" 1)
    file=$(field "$f" 2)
    text=$(field "$f" 3)
    repl=$(field "$f" 4)
    names=$(field "$f" 5)
    rm -rf "$work/m"
    mkdir "$work/m"
    copy "$work/m"
    T=$text R=$repl awk '$0 == ENVIRON["T"] { print ENVIRON["R"]; next } { print }' \
        "$src/$file" >"$work/m/tools/etude/$file"
    if [ "$(count "$work/m/tools/etude/$file" "$text")" -ne 0 ]; then
        echo "wincheck: $name: the fault was not applied" >&2
        bad=1
        continue
    fi
    if ! compile "$work/m" "$flags" >"$work/m.clog" 2>&1; then
        echo "wincheck: $name: the faulty sources do not build:" >&2
        cat "$work/m.clog" >&2
        bad=1
        continue
    fi
    # shellcheck disable=SC2086
    if fixtures "$work/m" "$work/m.log" $names; then
        echo "wincheck: $name: the fixtures passed with the fault in" >&2
        cat "$work/m.log" >&2
        bad=1
        continue
    fi
    for n in $names; do
        if ! grep -q "^winfixture: result: fail: $n: " "$work/m.log"; then
            echo "wincheck: $name: $n did not fail:" >&2
            cat "$work/m.log" >&2
            bad=1
        fi
    done
    printf 'wincheck: %s: caught by %s: %s\n' "$name" "$names" \
        "$(grep "^winfixture: result: fail: " "$work/m.log" | head -n 1 | cut -d: -f4-)"
done <"$work/faults"
echo "wincheck: the restored sources"
pristine
if [ "$bad" -ne 0 ]; then
    echo "wincheck: result: fail: a fault was not caught" >&2
    exit 1
fi
echo "wincheck: result: pass: every fault caught, and the restored sources pass"
finished=1
