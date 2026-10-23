#!/bin/sh
#
# etude_version_test.sh: `etude provision version`, which
# tools/etude/provision_version.sh is, builds a version of etude on a machine
# once and reuses it by its key, keeps versions side by side, publishes
# nothing from a build that failed, was stopped or got damaged source, and
# works whatever shell the machine's account is configured with.
#
# The rows run a private copy of tools/etude against stand-ins for ssh(1) and
# the compiler. The `ssh` stand-in runs its command as sshd does, by the
# account's shell with -c, in a process group of its own, with HOME a
# fixture directory whose shell startup files write to standard output;
# given the Windows command, it runs sh with `uname` and `cygpath` that
# answer as MSYS2's. `fakecc` answers -v with a version the fixture sets,
# logs each compile, and writes the digest of its sources as the program; the
# fixture can make it fail, send a signal to the machine's session, or first
# build the same version itself, as a second request would. One row builds
# the real etude with the host's compiler and runs its verbs.
#
# The rows: a build, and its record and program; reuse, which compiles
# nothing, after changes outside etude's source; versions of two sources and
# two compilers side by side, the first unchanged; the shells sh, bash, and
# dash, zsh, ksh, mksh, yash, fish, tcsh and csh where the host has them,
# each with its startup files' output, a false result line among it; sh and
# its tools from BusyBox, as on Rach2; a build stopped by SIGTERM, which
# leaves nothing, and by SIGKILL, whose directory the next build passes by; a
# failed compile; a script cut short, which changes nothing; damaged source;
# two builds of one version at once, of which one is published; a local
# build under a parallel make with a variable of its own, neither of which
# reaches the compiles; Windows, whose version is etude.exe; a commit's
# source; and the real build.
#
# The busybox row runs on the host when it has BusyBox, and otherwise in
# etude's tooling machine (tests/system/tooling.sh), whose Ubuntu has it: the
# job runs this test with --busybox against the private copy. The row fails
# when neither has BusyBox; the host suite's preparation makes the machine.
#
# --busybox runs the busybox row alone, and exits 3 when the host has no
# BusyBox.
#
# Usage: sh tests/system/etude_version_test.sh [--busybox]
#
set -u
unset MAKEFLAGS MFLAGS MAKELEVEL

case ${1:-} in
    '') mode=check ;;
    --busybox) mode=busybox ;;
    *) echo 'etude-version-test: usage: etude_version_test.sh [--busybox]' >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-version-test.XXXXXX") || exit 1
TREE=$work/tree
FIX=$work/fix
export TREE FIX
mkdir -p "$TREE/tools" "$TREE/utils" "$work/bin" "$work/windows" || exit 1
cp -R "$repo/tools/etude" "$TREE/tools/" || exit 1
cp "$repo/utils/sha256.c" "$repo/utils/sha256.h" "$TREE/utils/" || exit 1

windows_command='C:\msys64\usr\bin\env.exe PATH=/usr/bin:/ucrt64/bin sh -s'

# ssh -T -o BatchMode=yes -- HOST COMMAND: log the arguments, and run COMMAND
# by the shell FIX/shell names, sh by default, with -c, in a process group of
# its own, with HOME FIX/home and FIX/path, when it exists, before PATH.
cat > "$work/bin/ssh" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "\$FIX/ssh.log"
[ "\$#" -eq 6 ] && [ "\$1 \$2 \$3 \$4" = '-T -o BatchMode=yes --' ] ||
    { echo "ssh: unexpected arguments: \$*" >&2; exit 255; }
command=\$6
shell=sh
[ ! -e "\$FIX/shell" ] || shell=\$(cat "\$FIX/shell")
path=\$PATH
[ ! -e "\$FIX/path" ] || path=\$(cat "\$FIX/path"):\$PATH
if [ "\$command" = '$windows_command' ]; then
    command='sh -s'
    path=$work/windows:\$path
    LOCALAPPDATA='C:\\Users\\fix\\AppData\\Local'
    export LOCALAPPDATA
fi
exec env HOME="\$FIX/home" BASH_ENV="\$FIX/home/.bashrc" PATH="\$path" \\
    perl -e 'setpgrp(0, 0); exec @ARGV or die "ssh: exec: \$!\\n"' "\$shell" -c "\$command"
EOF
cat > "$work/windows/uname" <<'EOF'
#!/bin/sh
case $1 in
    -s) echo MINGW64_NT-10.0-26100 ;;
    -m) echo x86_64 ;;
    -r) echo 3.6.1-1.x86_64 ;;
    *) exit 1 ;;
esac
EOF
cat > "$work/windows/cygpath" <<'EOF'
#!/bin/sh
[ "$1" = -u ] && [ "$2" = 'C:\Users\fix\AppData\Local' ] || exit 1
echo "$FIX/appdata"
EOF

# fakecc: -v prints the version FIX/ccversion gives, 1 by default. A compile
# logs its arguments to FIX/cc.log and its MAKEFLAGS to FIX/flags.log, and
# writes to the -o file a digest of its sources. FIX/fail makes it fail;
# FIX/interrupt, a signal's name, makes it send that signal to its process
# group, the machine's session; FIX/race makes it first build the same
# version through provision_version.sh.
cat > "$work/bin/fakecc" <<'EOF'
#!/bin/sh
if [ "$1" = -v ]; then
    echo "fakecc version $(cat "$FIX/ccversion" 2>/dev/null || echo 1)" >&2
    exit 0
fi
printf '%s\n' "$*" >> "$FIX/cc.log"
printf '%s\n' "${MAKEFLAGS:-}" >> "$FIX/flags.log"
[ ! -e "$FIX/fail" ] || { echo 'fakecc: error: planted failure' >&2; exit 1; }
if [ -e "$FIX/interrupt" ]; then
    signal=$(cat "$FIX/interrupt")
    rm -f "$FIX/interrupt"
    kill -s "$signal" 0
fi
if [ -e "$FIX/race" ]; then
    rm -f "$FIX/race"
    sh "$TREE/tools/etude/provision_version.sh" -c fakecc -C "$FIX/cache" machine > "$FIX/race.out" 2> "$FIX/race.err" || exit 1
fi
out=
sources=
while [ "$#" -gt 0 ]; do
    case $1 in
        -o) out=$2; shift ;;
        *.c) sources="$sources $1" ;;
    esac
    shift
done
[ -n "$out" ] || exit 1
# shellcheck disable=SC2086 # one path a word
{ echo fakecc; cat $sources | cksum; } > "$out"
EOF
chmod 755 "$work/bin/ssh" "$work/bin/fakecc" "$work/windows/uname" "$work/windows/cygpath" || exit 1

# The shells an account may have, of which the rows require sh and bash, and
# those whose startup files a shell started by sshd reads, with the file.
shells='sh'
for s in bash dash zsh ksh mksh yash fish tcsh csh; do
    if command -v "$s" >/dev/null 2>&1; then
        shells="$shells $s"
    elif [ "$s" = bash ]; then
        echo 'etude-version-test: no bash on this host' >&2
        exit 1
    fi
done

# BusyBox's sh and its applets, but for those the stand-ins and the build
# need, in a directory of their own, as Rach2 has them.
busybox=$(command -v busybox 2>/dev/null) || busybox=
if [ -n "$busybox" ]; then
    mkdir "$work/busybox" || exit 1
    for a in $("$busybox" --list); do
        case $a in
            make|cc|env|perl) ;;
            *) ln -s "$busybox" "$work/busybox/$a" || exit 1 ;;
        esac
    done
fi

# Where the busybox row ran, and why it cannot run.
ranbusybox=
nobusybox=
tooling=
if [ -z "$busybox" ] && [ "$mode" != busybox ]; then
    REPO=$repo
    # shellcheck source=tests/system/tooling.sh
    . "$repo/tests/system/tooling.sh"
    if tooling_made; then
        tooling=1
    else
        nobusybox='no busybox on this host, and no made tooling machine'
    fi
fi

# state: an empty fixture: a home whose startup files print, and no cache.
state()
{
    rm -rf "$FIX" && mkdir -p "$FIX/home/.config/fish" || exit 1
    : > "$FIX/cc.log"
    : > "$FIX/flags.log"
    : > "$FIX/ssh.log"
    for f in .bashrc .zshenv .cshrc .tcshrc .config/fish/config.fish; do
        { echo "echo noise from $f"
          echo 'echo @etude-version 0123456789abcdef built 00 /nowhere'
          echo 'cd /'; } > "$FIX/home/$f"
    done
}

# run ARGS...: run the copy of provision_version.sh with the stand-ins first
# on the path, its output in work/out, its errors in work/err, its status in
# work/status.
run()
{
    PATH="$work/bin:$PATH" sh "$TREE/tools/etude/provision_version.sh" "$@" > "$work/out" 2> "$work/err"
    echo "$?" > "$work/status"
}

# why TEXT: record why the row failed, and fail.
why()
{
    echo "$*" >> "$work/why"
    { echo '  stdout:'; sed 's/^/    /' "$work/out"; echo '  stderr:'; sed 's/^/    /' "$work/err"; } >> "$work/why"
    return 1
}

# result HOW [ROOT]: require the last run to have printed `etude provision:
# result: HOW KEY DIR` alone and exited 0, DIR a build of the cache at ROOT,
# FIX/cache by default. Sets key and dir.
result()
{
    root=${2:-$FIX/cache}
    [ "$(cat "$work/status")" = 0 ] || { why "status $(cat "$work/status"), wanted 0"; return 1; }
    [ "$(wc -l < "$work/out")" -eq 1 ] || { why 'not one line of output'; return 1; }
    # shellcheck disable=SC2046 # the result is words
    set -- $(cat "$work/out") "$1"
    [ "$#" -eq 7 ] && [ "$1 $2 $3" = 'etude provision: result:' ] && [ "$4" = "$7" ] ||
        { why "the result is not $7"; return 1; }
    key=$5
    dir=$6
    case $dir in
        "$root/builds/$key".??????) ;;
        *) why "$dir is not a build of $root"; return 1 ;;
    esac
    if [ ! -d "$dir/bin" ] || ! cmp -s "$root/versions/$key" "$dir/version"; then
        why "$root/versions/$key does not publish $dir"
        return 1
    fi
}

# refused: require the last run to have failed with no result.
refused()
{
    [ "$(cat "$work/status")" != 0 ] || { why 'status 0, wanted a failure'; return 1; }
    ! grep -q 'result:' "$work/out" || { why 'a result after a failure'; return 1; }
}

# count DIR: print the number of entries in DIR, 0 when it is missing.
count()
{
    # shellcheck disable=SC2012 # the names are the cache's own
    if [ -d "$1" ]; then ls -A "$1" | wc -l | tr -d ' '; else echo 0; fi
}

# compiles: print the number of compiles fakecc logged.
compiles()
{
    wc -l < "$FIX/cc.log" | tr -d ' '
}

# digest FILE...: print a digest of the files' contents.
digest()
{
    cat "$@" | cksum
}

# shell_row SHELL [DIR]: build and reuse a version with SHELL the account's
# shell, and DIR before the machine's PATH.
shell_row()
{
    state
    echo "$1" > "$FIX/shell"
    [ -z "${2:-}" ] || echo "$2" > "$FIX/path"
    run -c fakecc -C "$FIX/cache" machine
    result built || { why "under $1"; return 1; }
    run -c fakecc -C "$FIX/cache" machine
    result reused || { why "under $1"; return 1; }
    [ "$(compiles)" = 1 ] || { why "under $1: $(compiles) compiles, wanted 1"; return 1; }
    case $1 in
        bash|zsh|tcsh|csh|fish)
            grep -q '^noise from ' "$work/err" || { why "under $1: the startup file's output was lost"; return 1; } ;;
    esac
}

# row NAME: run row NAME and succeed when its result is the expected one.
row()
{
    case $1 in
        build)
            state
            run -c fakecc -C "$FIX/cache" machine
            result built || return 1
            [ "$(cat "$FIX/ssh.log")" = '-T -o BatchMode=yes -- machine sh -s' ] ||
                { why "ssh was run as: $(cat "$FIX/ssh.log")"; return 1; }
            [ "$(count "$FIX/cache/builds")" = 1 ] && [ "$(count "$FIX/cache/versions")" = 1 ] ||
                { why 'not one build and one version'; return 1; }
            [ "$(cd "$dir/bin" && echo *)" = etude ] ||
                { why "the programs are $(cd "$dir/bin" && echo *)"; return 1; }
            [ "$(compiles)" = 1 ] || { why "$(compiles) compiles, wanted 1"; return 1; }
            [ "$(sed -n 's/^cc //p;s/^flags //p' "$dir/version" | tr '\n' ' ')" = '-std=c99 -O2 fakecc ' ] ||
                { why 'the record does not give the compiler and its flags'; return 1; }
            [ "$(grep '^program ' "$dir/version")" = "program etude $(sha "$dir/bin/etude")" ] ||
                { why "the record does not give etude's digest alone"; return 1; }
            [ "$(sed -n 1,5p "$dir/version" | sha)" = "$key" ] ||
                { why 'the key is not the digest of the record'\''s first five lines'; return 1; } ;;
        reuse)
            state
            run -c fakecc -C "$FIX/cache" machine
            result built || return 1
            first="$key $dir"
            echo 'A change of the Readme.' >> "$TREE/tools/etude/Readme.md"
            echo 'int fixture;' > "$TREE/tools/etude/fixtures/new.c"
            run -c fakecc -C "$FIX/cache" machine
            cp "$repo/tools/etude/Readme.md" "$TREE/tools/etude/Readme.md"
            rm -f "$TREE/tools/etude/fixtures/new.c"
            result reused || return 1
            [ "$key $dir" = "$first" ] || { why "reused $key $dir, not $first"; return 1; }
            [ "$(compiles)" = 1 ] || { why "$(compiles) compiles, wanted 1"; return 1; } ;;
        versions)
            state
            run -c fakecc -C "$FIX/cache" machine
            result built || return 1
            first="$key $dir"
            before=$(digest "$dir/version" "$dir/bin/etude")
            echo '/* A change. */' >> "$TREE/tools/etude/json.c"
            run -c fakecc -C "$FIX/cache" machine
            cp "$repo/tools/etude/json.c" "$TREE/tools/etude/json.c"
            result built || { why 'for changed source'; return 1; }
            second=$key
            echo 2 > "$FIX/ccversion"
            run -c fakecc -C "$FIX/cache" machine
            rm -f "$FIX/ccversion"
            result built || { why 'for another compiler'; return 1; }
            [ "$key" != "$second" ] && [ "$key $dir" != "$first" ] && [ "$second" != "${first%% *}" ] ||
                { why 'two versions have one key'; return 1; }
            run -c fakecc -C "$FIX/cache" machine
            result reused || { why 'for the first version again'; return 1; }
            [ "$key $dir" = "$first" ] || { why "reused $key $dir, not $first"; return 1; }
            [ "$(compiles)" = 3 ] || { why "$(compiles) compiles, wanted 3"; return 1; }
            [ "$(digest "$dir/version" "$dir/bin/etude")" = "$before" ] ||
                { why 'the first version changed'; return 1; }
            [ "$(count "$FIX/cache/versions")" = 3 ] && [ "$(count "$FIX/cache/builds")" = 3 ] ||
                { why 'not three versions side by side'; return 1; } ;;
        shells)
            for s in $shells; do
                shell_row "$s" || return 1
            done ;;
        busybox)
            if [ -n "$busybox" ]; then
                shell_row "$work/busybox/sh" "$work/busybox" || return 1
                ranbusybox='on this host'
            elif [ -n "$tooling" ]; then
                busybox_elsewhere || return 1
            else
                why "$nobusybox"
                return 1
            fi ;;
        term)
            state
            echo TERM > "$FIX/interrupt"
            run -c fakecc -C "$FIX/cache" machine
            refused || return 1
            [ "$(count "$FIX/cache/builds")" = 0 ] && [ "$(count "$FIX/cache/versions")" = 0 ] ||
                { why 'a stopped build left something'; return 1; }
            run -c fakecc -C "$FIX/cache" machine
            result built || return 1 ;;
        kill)
            state
            echo KILL > "$FIX/interrupt"
            run -c fakecc -C "$FIX/cache" machine
            refused || return 1
            [ "$(count "$FIX/cache/versions")" = 0 ] || { why 'a killed build was published'; return 1; }
            [ "$(count "$FIX/cache/builds")" = 1 ] || { why 'the killed build did not stay'; return 1; }
            left=$(ls -d "$FIX/cache/builds"/*)
            run -c fakecc -C "$FIX/cache" machine
            result built || return 1
            [ "$dir" != "$left" ] && [ -d "$left" ] && [ ! -e "$left/version" ] ||
                { why 'the next build did not pass the killed one by'; return 1; } ;;
        failed)
            state
            : > "$FIX/fail"
            run -c fakecc -C "$FIX/cache" machine
            refused || return 1
            grep -q '^etude provision: the build of [0-9a-f]* failed$' "$work/err" || { why 'no report of the failure'; return 1; }
            [ "$(count "$FIX/cache/builds")" = 0 ] && [ "$(count "$FIX/cache/versions")" = 0 ] ||
                { why 'a failed build left something'; return 1; } ;;
        truncated)
            state
            run -c fakecc -C "$FIX/cache" -s
            [ "$(cat "$work/status")" = 0 ] || { why 'no script'; return 1; }
            cp "$work/out" "$work/script"
            size=$(wc -c < "$work/script")
            for cut in 'head -c 64' "head -c $((size / 2))" "head -c $((size - 40))" "sed \$d"; do
                $cut < "$work/script" | PATH="$work/bin:$PATH" HOME="$FIX/home" sh -s > "$work/out" 2> "$work/err"
                [ ! -e "$FIX/cache" ] && [ ! -s "$work/out" ] && [ "$(compiles)" = 0 ] ||
                    { why "a script cut by $cut ran"; return 1; }
            done
            PATH="$work/bin:$PATH" HOME="$FIX/home" sh -s < "$work/script" > "$work/out" 2> "$work/err"
            grep -q '^@etude-version [0-9a-f]* built ' "$work/out" || { why 'the whole script built nothing'; return 1; } ;;
        damaged)
            state
            run -c fakecc -C "$FIX/cache" -s
            awk '{ print } /^    cat > "\$1\/tools\/etude\/json.c" <</ { print "/* damage */" }' "$work/out" > "$work/script"
            cmp -s "$work/out" "$work/script" && { why 'no damage made'; return 1; }
            PATH="$work/bin:$PATH" HOME="$FIX/home" sh -s < "$work/script" > "$work/out" 2> "$work/err"
            echo "$?" > "$work/status"
            refused || return 1
            grep -q '^etude provision: the source arrived damaged' "$work/err" || { why 'no report of the damage'; return 1; }
            [ "$(count "$FIX/cache/builds")" = 0 ] && [ "$(count "$FIX/cache/versions")" = 0 ] && [ "$(compiles)" = 0 ] ||
                { why 'damaged source was built'; return 1; } ;;
        race)
            state
            : > "$FIX/race"
            run -c fakecc -C "$FIX/cache" machine
            result reused || return 1
            [ "$(sed 's/^etude provision: result: built //' "$FIX/race.out")" = "$key $dir" ] ||
                { why "the other request gave $(cat "$FIX/race.out")"; return 1; }
            [ "$(count "$FIX/cache/builds")" = 1 ] || { why 'the second build stayed'; return 1; } ;;
        makeflags)
            state
            MAKEFLAGS='-j4 -- ETUDE_PLANTED=1' run -l -c fakecc -C "$FIX/cache"
            result built || return 1
            pattern='-j([^1]|1[0-9]|$)|jobserver|ETUDE_PLANTED'
            ! grep -qE -e "$pattern" "$FIX/flags.log" ||
                { why "a compile saw MAKEFLAGS $(grep -m 1 -E -e "$pattern" "$FIX/flags.log")"; return 1; } ;;
        windows)
            state
            run -w -c fakecc machine
            result built "$FIX/appdata/etude" || return 1
            [ "$(cat "$FIX/ssh.log")" = "-T -o BatchMode=yes -- machine $windows_command" ] ||
                { why "ssh was run as: $(cat "$FIX/ssh.log")"; return 1; }
            [ "$(sed -n 's/^platform //p' "$dir/version")" = 'windows x86_64' ] &&
                [ "$(sed -n 's/^program \([^ ]*\) .*/\1/p' "$dir/version" | tr '\n' ' ')" = 'etude.exe ' ] ||
                { why 'not a version for Windows'; return 1; } ;;
        revision)
            state
            git -C "$TREE" init -q && git -C "$TREE" add -A &&
                git -C "$TREE" -c user.name=test -c user.email=test@example.com commit -qm source || exit 1
            echo '/* A change. */' >> "$TREE/tools/etude/json.c"
            run -r HEAD -c fakecc -C "$FIX/cache" machine
            result built || { rm -rf "$TREE/.git"; why 'for HEAD'; return 1; }
            first="$key $dir"
            run -c fakecc -C "$FIX/cache" machine
            cp "$repo/tools/etude/json.c" "$TREE/tools/etude/json.c"
            result built || { rm -rf "$TREE/.git"; why 'for the working tree'; return 1; }
            [ "$key $dir" != "$first" ] || { rm -rf "$TREE/.git"; why 'the working tree gave the commit'\''s version'; return 1; }
            run -c fakecc -C "$FIX/cache" machine
            rm -rf "$TREE/.git"
            result reused || { why 'for the working tree as committed'; return 1; }
            [ "$key $dir" = "$first" ] || { why 'the commit and its working tree gave two versions'; return 1; }
            [ "$(compiles)" = 2 ] || { why "$(compiles) compiles, wanted 2"; return 1; } ;;
        real)
            state
            run -l -C "$FIX/cache"
            result built || return 1
            "$dir/bin/etude" check -s "$dir/src/tools/etude/schemas" schema case \
                "$repo/tools/etude/schemas/case.host.example.json" > "$work/out" 2> "$work/err" ||
                { why 'the built etude check refused an example'; return 1; }
            "$dir/bin/etude" check history "$dir/src/tools/etude/schemas/history.schema.json" \
                "$repo/tools/etude/schemas/history.example.jsonl" > "$work/out" 2> "$work/err" ||
                { why 'the built etude check history refused an example'; return 1; }
            for verb in serve admit 'lock tree' 'lock ports' build; do
                rc=0
                # shellcheck disable=SC2086 # a verb and its form are words
                "$dir/bin/etude" $verb > "$work/out" 2> "$work/err" || rc=$?
                [ "$rc" = 2 ] || { why "the built etude $verb exited $rc without arguments"; return 1; }
                grep -q "usage: etude $verb " "$work/err" ||
                    { why "the built etude $verb did not give its usage"; return 1; }
            done ;;
    esac
}

# busybox_elsewhere: run the busybox row against the copy in the tooling
# machine, with this test and the copy in one tar file, since the job's volume
# keeps no file's mode.
busybox_elsewhere()
{
    rm -rf "$work/job" && mkdir -p "$work/job/tests/system" || exit 1
    cp "$repo/tests/system/etude_version_test.sh" "$work/job/tests/system/" &&
        cp -R "$TREE/tools" "$TREE/utils" "$work/job/" &&
        COPYFILE_DISABLE=1 tar -c --format ustar -f "$work/job.tar" -C "$work/job" tests tools utils || exit 1
    echo 'tar -x -f job.tar && sh tests/system/etude_version_test.sh --busybox' > "$work/job.sh"
    rc=0
    tooling run -m 1024 -i "$work/job.tar" "$work/job.sh" > "$work/out" 2> "$work/err" || rc=$?
    case $rc in
        0) ranbusybox='in the tooling machine' ;;
        3) why 'no busybox on this host or in the tooling machine'; return 1 ;;
        126) why 'no busybox on this host, and no made tooling machine'; return 1 ;;
        *) why "the tooling machine's job ended with status $rc"; return 1 ;;
    esac
}

# sha [FILE]: print the SHA-256 of FILE or of standard input.
sha()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@"
    else
        shasum -a 256 "$@"
    fi | sed 's/ .*//'
}

rows='build reuse versions shells busybox term kill failed truncated damaged race makeflags windows revision real'

# rows: run every row against the copy, print `etude-version-test: failed:
# NAME` for each that fails, and fail when any did.
rows()
{
    failed=0
    for r in $rows; do
        : > "$work/why"
        if ! row "$r"; then
            echo "etude-version-test: failed: $r"
            sed 's/^/    /' "$work/why"
            failed=1
        fi
    done
    return "$failed"
}

if [ "$mode" = busybox ]; then
    [ -n "$busybox" ] || { echo 'etude-version-test: not run: busybox: no busybox on this host'; exit 3; }
    rows=busybox
    rows >&2 || exit 1
    echo 'etude-version-test: result: pass: busybox'
    exit 0
fi
rows >&2 || exit 1
echo "etude-version-test: shells: $shells${ranbusybox:+ busybox}"
[ -z "$ranbusybox" ] || echo "etude-version-test: busybox: $ranbusybox"
echo 'etude-version-test: result: pass: 15 rows'
