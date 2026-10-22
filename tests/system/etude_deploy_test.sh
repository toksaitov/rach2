#!/bin/sh
#
# etude_deploy_test.sh: tools/etude/deploy.sh makes a machine ready to run
# etude, and a repeated deployment, or one cut short and run again, finishes
# without changing a clone, a key, a running test's ledger or its version.
#
# The rows run a private copy of tools/etude and tools/setup, whose recipe of
# prerequisites is a fixture that looks under FIX/have, against stand-ins for
# ssh(1), the compiler, sudo, id, QEMU and the Windows machine's PowerShell
# and MSYS2. The `ssh` stand-in runs its command by sh in a process group of
# its own, with HOME a fixture directory that has a clone and SSH keys, and
# loses the connection of the call FIX/drop numbers. `fakecc` and `mv` send
# SIGTERM to that group when the fixture asks, during the build and while
# the machine's record is written.
#
# The rows: a deployment and its record, ledger and scratch; a second one,
# which changes nothing; a prerequisite that needs an administrator, left as
# commands and then installed by one; one installed through sudo; -n, which
# installs nothing; a new version deployed beside a running test, whose
# ledger and version stay as they were; a ledger of another format, which
# keeps the machine's version; a ledger this account cannot write, which is
# reported with the commands that fix it; a connection lost at each of the
# three steps, and a build and a record stopped by SIGTERM, each followed by
# a deployment that finishes; a deployment on this machine from a clone,
# which the probes leave as it was; a commit's source; Windows; the discovery
# of QEMU's OVMF code image and an absent override. --ovmf runs only the two
# firmware rows and Windows. --kvm runs only the three KVM device rows.
#
# With --mutate each fault below is put into the copy and must fail exactly
# its expected rows; the copy is restored and the rows run again at the end.
# --texts puts each fault into the copy, requires only that it changes the
# file, and runs no row.
#
# Usage: sh tests/system/etude_deploy_test.sh
#            [--mutate | --texts | --ovmf | --kvm]
#
set -u

case ${1:-} in
    '') mode=check ;;
    --mutate) mode=mutate ;;
    --texts) mode=texts ;;
    --ovmf) mode=ovmf ;;
    --kvm) mode=kvm ;;
    *) echo 'etude-deploy-test: usage: etude_deploy_test.sh [--mutate | --texts | --ovmf | --kvm]' >&2; exit 2 ;;
esac
deploy_source=${ETUDE_DEPLOY_SOURCE:-}
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
testpid=
trap '[ -z "$testpid" ] || kill "$testpid" 2>/dev/null; [ -z "$work" ] || { chmod -R u+rwx "$work" 2>/dev/null; rm -rf "$work"; }' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-deploy-test.XXXXXX") || exit 1
TREE=$work/tree
FIX=$work/fix
export TREE FIX
mkdir -p "$TREE/tools/setup" "$TREE/utils" "$work/bin" "$work/mbin" "$work/windows" "$work/linux" "$work/ctl" || exit 1
cp -R "$repo/tools/etude" "$TREE/tools/" || exit 1
for f in prereq.sh e2fsprogs_path.sh ovmf_path.sh elfwrap.sh; do
    cp "$repo/tools/setup/$f" "$TREE/tools/setup/" || exit 1
done
cp "$repo/utils/sha256.c" "$repo/utils/sha256.h" "$TREE/utils/" || exit 1
realuname=$(command -v uname) || exit 1
realmv=$(command -v mv) || { echo 'etude-deploy-test: no mv on this host' >&2; exit 1; }
[ "$(id -u)" != 0 ] && root=0 || root=1

windows_command='C:\msys64\usr\bin\env.exe PATH=/usr/bin:/ucrt64/bin sh -s'

# The recipe: the same stanzas on every POSIX platform, of which `scratch`
# writes a file in the directory its probe starts in, the tree's root.
{
    echo '# A recipe whose probes look under FIX/have.'
    for p in linux macos rach2; do
        cat <<EOF
platform $p

need user-tool
probe [ -e "\$FIX/have/user-tool" ]
user touch "\$FIX/have/user-tool"

need admin-tool
probe [ -e "\$FIX/have/admin-tool" ]
admin touch "\$FIX/have/admin-tool"

need scratch
probe [ -f tools/setup/prereq.sh ] && : > probed
user true

EOF
    done
    cat <<'EOF'
platform windows

need user-tool
probe Have user-tool
user Get user-tool

need whpx
probe Have whpx
manual # enable the Windows Hypervisor Platform
EOF
} > "$TREE/tools/setup/prereqs" || exit 1

# ssh -T -o BatchMode=yes -- HOST COMMAND...: log the arguments, and run the
# command by sh with -c in a process group of its own, with HOME FIX/home,
# TMPDIR FIX/tmp and the machine's stand-ins first on the path. The call
# FIX/drop numbers loses its connection. The Windows command runs sh with
# MSYS2's stand-ins, and `powershell ... -EncodedCommand SCRIPT` answers
# prereq.sh's words: `Have NAME` is true when FIX/have/NAME exists, `Get
# NAME` makes it, and the account is elevated when FIX/elevated exists.
cat > "$work/bin/ssh" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "\$FIX/ssh.log"
[ "\$#" -ge 6 ] && [ "\$1 \$2 \$3 \$4" = '-T -o BatchMode=yes --' ] ||
    { echo "ssh: unexpected arguments: \$*" >&2; exit 255; }
shift 5
command=\$*
n=\$(wc -l < "\$FIX/ssh.log")
if [ -e "\$FIX/drop" ] && [ "\$(cat "\$FIX/drop")" -eq "\$n" ]; then
    echo 'ssh: connection lost' >&2
    exit 255
fi
path=$work/mbin:\$PATH
case \$command in
    'powershell -NoProfile -NonInteractive -EncodedCommand '*)
        script=\$(printf '%s' "\${command##* }" | base64 -d | iconv -f UTF-16LE -t UTF-8) || exit 2
        line=\$(printf '%s\n' "\$script" | sed -n 2p)
        case \$line in
            *IsInRole*) [ -e "\$FIX/elevated" ]; exit ;;
            'if (Have '*') { exit 0 } else { exit 1 }') name=\${line#if (Have }; [ -e "\$FIX/have/\${name%%)*}" ]; exit ;;
            'Get '*) echo "pwsh \$line" >> "\$FIX/log"; : > "\$FIX/have/\${line#Get }"; exit ;;
        esac
        echo "powershell: unknown line: \$line" >&2
        exit 2 ;;
    '$windows_command')
        command='sh -s'
        path=$work/windows:\$path
        LOCALAPPDATA='C:\\Users\\fix\\AppData\\Local'
        ProgramFiles='C:\\Program Files'
        export LOCALAPPDATA ProgramFiles ;;
esac
exec env HOME="\$FIX/home" TMPDIR="\$FIX/tmp" XDG_STATE_HOME= PATH="\$path" \\
    perl -e 'setpgrp(0, 0); exec @ARGV or die "ssh: exec: \$!\\n"' sh -c "\$command"
EOF
cat > "$work/linux/uname" <<EOF
#!/bin/sh
case \$1 in
    -s) echo Linux ;;
    *) exec "$realuname" "\$@" ;;
esac
EOF
cat > "$work/windows/uname" <<'EOF'
#!/bin/sh
case $1 in
    -s) echo MINGW64_NT-10.0-26100 ;;
    -m) echo x86_64 ;;
    -r) echo 3.6.1-1.x86_64 ;;
    -n) echo winbox ;;
    *) exit 1 ;;
esac
EOF
cat > "$work/windows/cygpath" <<'EOF'
#!/bin/sh
case "$1 $2" in
    '-u C:\Users\fix\AppData\Local') echo "$FIX/appdata" ;;
    '-u C:\Program Files') echo "$FIX/programfiles" ;;
    "-m $FIX/programfiles/qemu/share/edk2-x86_64-code.fd")
        echo 'C:/Program Files/qemu/share/edk2-x86_64-code.fd' ;;
    -m\ /*) echo "$2" ;;
    *) exit 1 ;;
esac
EOF

# fakecc: -v prints a version; a compile logs its arguments to FIX/cc.log
# and writes to the -o file a digest of its sources. FIX/interrupt makes it
# send SIGTERM to its process group, the machine's session.
cat > "$work/bin/fakecc" <<'EOF'
#!/bin/sh
if [ "$1" = -v ]; then
    echo 'fakecc version 1' >&2
    exit 0
fi
printf '%s\n' "$*" >> "$FIX/cc.log"
if [ -e "$FIX/interrupt" ]; then
    rm -f "$FIX/interrupt"
    kill -s TERM 0
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
# The machine's stand-ins. `id` answers as the account `fixuser`, never
# root; `sudo -n` succeeds when FIX/sudo exists and logs what it runs;
# `qemu-system-x86_64` gives a version; `mv` to a file named `machine`
# sends SIGTERM to its process group when FIX/interrupt-record exists.
cat > "$work/mbin/id" <<'EOF'
#!/bin/sh
case $1 in
    -u) echo 1000 ;;
    -un) echo fixuser ;;
    *) exit 1 ;;
esac
EOF
cat > "$work/mbin/sudo" <<'EOF'
#!/bin/sh
[ -e "$FIX/sudo" ] && [ "$1" = -n ] || { echo 'sudo: a password is required' >&2; exit 1; }
shift
[ "$*" = true ] && exit 0
echo "sudo $*" >> "$FIX/log"
exec "$@"
EOF
cat > "$work/mbin/qemu-system-x86_64" <<'EOF'
#!/bin/sh
[ "$1" = --version ] || exit 1
echo 'QEMU emulator version 9.9.9 (fixture)'
echo 'Copyright (c) 2003-2026 Fabrice Bellard and the QEMU Project developers'
EOF
cat > "$work/mbin/mv" <<EOF
#!/bin/sh
for last; do :; done
if [ -e "\$FIX/interrupt-record" ] && [ "\${last##*/}" = machine ]; then
    rm -f "\$FIX/interrupt-record"
    kill -s TERM 0
fi
exec $realmv "\$@"
EOF
chmod 755 "$work/bin/ssh" "$work/bin/fakecc" "$work/windows/uname" "$work/windows/cygpath" \
    "$work/mbin/id" "$work/mbin/sudo" "$work/mbin/qemu-system-x86_64" "$work/mbin/mv" "$work/linux/uname" || exit 1

# What the record says of this machine, which the stand-in ssh reaches.
case $(uname -s) in
    Darwin)
        platform="macos $(uname -m)"
        cpus=$(sysctl -n hw.ncpu)
        memory=$(sysctl -n hw.memsize)
        accels=tcg
        [ "$(sysctl -n kern.hv_support 2>/dev/null)" != 1 ] || accels='hvf tcg' ;;
    *)
        platform="linux $(uname -m)"
        cpus=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN)
        memory=$(($(sed -n 's/^MemTotal: *\([0-9][0-9]*\) kB$/\1/p' /proc/meminfo) * 1024))
        accels=tcg
        if [ -r /dev/kvm ] && [ -w /dev/kvm ] && ( : <> /dev/kvm ) 2>/dev/null; then
            accels='kvm tcg'
        fi ;;
esac
windows_memory=$(sed -n 's/^MemTotal: *\([0-9][0-9]*\) kB$/\1/p' /proc/meminfo 2>/dev/null)
if [ -n "$windows_memory" ]; then windows_memory=$((windows_memory * 1024)); else windows_memory=-; fi

# fault NAME: put fault NAME into the private copy, or the original for
# `none`, and fail when a fault leaves the copy unchanged.
fault()
{
    cp "${deploy_source:-$repo/tools/etude/deploy.sh}" "$TREE/tools/etude/deploy.sh" || exit 1
    # shellcheck disable=SC2016 # the dollars are in the text sed matches
    case $1 in
        none) return 0 ;;
        rewrite) edit='s#^    if \[ -f "\$root/machine" \] && \[ "\$(cat "\$root/machine")" = "\$record" \]; then$#    if false; then#' ;;
        truncate-lock) edit='s#^    \[ -e "\$l/lock" \] || : >> "\$l/lock" ||#    : > "$l/lock" ||#' ;;
        fresh-ledger) edit='s#^    mkdir -p "\$l/verdicts" ||#    rm -rf "$l"; mkdir -p "$l/verdicts" ||#' ;;
        keep-temp) edit='s#^    \[ -z "\$tmp" \] || rm -f "\$tmp"$#    :#' ;;
        in-tree) edit='s#^    machine_sh < "\$work/prereq.sh" > "\$work/prereq"$#    if [ "$mode" = local ]; then (cd "$tree" \&\& sh tools/setup/prereq.sh $install); else machine_sh < "$work/prereq.sh"; fi > "$work/prereq"#' ;;
        no-install) edit='s#^install=--install$#install=#' ;;
        prereq-pass) edit='s#^\[ "\$prereq" = 0 \] || incomplete#true || incomplete#' ;;
        no-rights) edit='s#^        \[ -z "\$rights" \] ||$#        true ||#' ;;
        no-format) edit='s#^            \[ "\$format" = "\$ETUDE_ADMISSION" \] ||$#            true ||#' ;;
        tree-format) edit='s#^if \[ -n "\$rev" \]; then$#if false; then#' ;;
    esac
    sed "$edit" "$repo/tools/etude/deploy.sh" > "$TREE/tools/etude/deploy.sh" || exit 1
    ! cmp -s "$repo/tools/etude/deploy.sh" "$TREE/tools/etude/deploy.sh"
}

# The machine's home, made once and copied for each row: a clone with a
# commit and an untracked file, and an account's SSH keys. Each commit of the
# test turns off git's automatic maintenance, which would pack the objects of
# a clone in the background while a row compares it.
mkdir -p "$work/home/clone" "$work/home/.ssh" || exit 1
echo 'committed' > "$work/home/clone/file"
git -C "$work/home/clone" init -q && git -C "$work/home/clone" add file &&
    git -C "$work/home/clone" -c user.name=test -c user.email=test@example.com -c maintenance.auto=false -c gc.auto=0 commit -qm clone || exit 1
echo 'untracked' > "$work/home/clone/new"
echo 'ssh-ed25519 AAAAfixture fix@machine' > "$work/home/.ssh/authorized_keys"
echo 'fixture private key' > "$work/home/.ssh/id_ed25519"
echo 'machine ssh-ed25519 AAAAfixture' > "$work/home/.ssh/known_hosts"
chmod 600 "$work/home/.ssh/id_ed25519" || exit 1

# state: a fresh fixture: the home, both tools installed, no sudo, and
# nothing of etude.
state()
{
    [ ! -d "$FIX" ] || chmod -R u+rwx "$FIX"
    rm -rf "$FIX" && mkdir -p "$FIX/have" "$FIX/tmp" || exit 1
    cp -R "$work/home" "$FIX/home" || exit 1
    : > "$FIX/have/admin-tool"
    : > "$FIX/have/user-tool"
    : > "$FIX/cc.log"
    : > "$FIX/ssh.log"
    : > "$FIX/log"
    : > "$FIX/firmware code.fd"
    ovmf_code=$FIX/firmware\ code.fd
}

# run ARGS...: run the copy of deploy.sh with the stand-ins first on the
# path and the cache FIX/cache, its output in work/out, its errors in
# work/err, its status in work/status. Its own scratch must be gone after.
run()
{
    PATH="$work/bin:$work/mbin:$PATH" TMPDIR="$work/ctl" RACH2_OVMF_CODE="$ovmf_code" \
        sh "$TREE/tools/etude/deploy.sh" -c fakecc -C "$FIX/cache" "$@" > "$work/out" 2> "$work/err"
    echo "$?" > "$work/status"
    # shellcheck disable=SC2012 # the names are the scripts' own
    [ -z "$(ls -A "$work/ctl")" ] || echo "the controller left $(ls -A "$work/ctl")" >> "$work/why"
    [ -z "$(ls -A "$FIX/tmp" 2>/dev/null)" ] || echo "the machine's scratch stayed: $(ls -A "$FIX/tmp")" >> "$work/why"
}

# why TEXT: record why the row failed, and fail.
why()
{
    echo "$*" >> "$work/why"
    { echo '  stdout:'; sed 's/^/    /' "$work/out"; echo '  stderr:'; sed 's/^/    /' "$work/err"; } >> "$work/why"
    return 1
}

# deployed: require the last run to have exited 0 with `etude-deploy:
# result: deployed KEY` last, and no scratch left. Sets key and dir from the
# version it reports.
deployed()
{
    [ ! -s "$work/why" ] || { why 'scratch was left'; return 1; }
    [ "$(cat "$work/status")" = 0 ] || { why "status $(cat "$work/status"), wanted 0"; return 1; }
    key=$(sed -n 's/^etude-version: result: [a-z]* \([0-9a-f]*\) .*/\1/p' "$work/out")
    dir=$(sed -n 's/^etude-version: result: [a-z]* [0-9a-f]* //p' "$work/out")
    [ -n "$key" ] && [ "$(tail -n 1 "$work/out")" = "etude-deploy: result: deployed $key" ] ||
        { why 'no result deployed'; return 1; }
}

# incomplete WHY: require the last run to have exited 1 with `etude-deploy:
# result: incomplete: WHY` last, and no scratch left.
incomplete()
{
    [ ! -s "$work/why" ] || { why 'scratch was left'; return 1; }
    [ "$(cat "$work/status")" = 1 ] || { why "status $(cat "$work/status"), wanted 1"; return 1; }
    last=$(tail -n 1 "$work/out")
    # shellcheck disable=SC2254 # WHY is a pattern
    case $last in
        "etude-deploy: result: incomplete: "$1) ;;
        *) why "the last line is not incomplete: $1"; return 1 ;;
    esac
}

# record FILE LEDGER [HOST PLATFORM QEMU ACCELERATORS MEMORY]: require the
# record of version key in dir with the ledger LEDGER, of this machine unless
# the rest is given.
record()
{
    {
        echo 'etude-machine 1'
        echo "host ${3:-$(uname -n)}"
        echo "platform ${4:-$platform}"
        echo "cpus $cpus"
        echo "memory_bytes ${7:-$memory}"
        echo "qemu ${5:-QEMU emulator version 9.9.9 (fixture)}"
        echo "ovmf_x86_64 ${8:-$FIX/firmware code.fd}"
        echo "accelerators ${6:-$accels}"
        echo 'admission etude-ledger 1'
        echo "ledger $2"
        echo "version $key $dir"
    } > "$work/record"
    cmp -s "$work/record" "$1" || {
        why "$1 is not the record:"
        diff "$work/record" "$1" >> "$work/why" 2>&1
        return 1
    }
}

# ledger DIR: require DIR to be a ledger as deployment makes it: an empty
# lock and an empty directory of verdicts.
ledger()
{
    # shellcheck disable=SC2012 # the names are the ledger's own
    [ "$(ls -A "$1" | tr '\n' ' ')" = 'lock verdicts ' ] && [ ! -s "$1/lock" ] &&
        [ -z "$(ls -A "$1/verdicts")" ] || { why "$1 is not a new ledger"; return 1; }
}

# snapshot PATH...: print each file under the paths with its inode, mode,
# size, time of change in nanoseconds and digest, to tell a file that was
# written again from one left as it was.
snapshot()
{
    find "$@" -print | LC_ALL=C sort |
        perl -MTime::HiRes=lstat -ne 'chomp; my @s = lstat($_) or next; printf "%s %d %o %d %.9f\n", $_, $s[1], $s[2], $s[7], $s[9]'
    find "$@" -type f -exec cksum {} + | LC_ALL=C sort
}

# untouched: require the clone and the keys of the machine's home to be as
# the row began.
untouched()
{
    snapshot "$FIX/home/clone" "$FIX/home/.ssh" > "$work/home.after"
    cmp -s "$work/home.before" "$work/home.after" || { why 'the clone or the keys changed'; return 1; }
}

# begin: a fresh fixture whose clone and keys are recorded.
begin()
{
    state
    snapshot "$FIX/home/clone" "$FIX/home/.ssh" > "$work/home.before"
}

# change: change etude's source in the copy, and restore: restore it.
change() { echo '/* A change. */' >> "$TREE/tools/etude/json.c"; }
restore() { cp "$repo/tools/etude/json.c" "$TREE/tools/etude/json.c"; }

home_state=$FIX/home/.local/state/etude

# row NAME: run row NAME and succeed when its result is the expected one.
row()
{
    case $1 in
        deploy)
            begin
            rm -f "$FIX/have/user-tool"
            run machine
            deployed || return 1
            [ -e "$FIX/have/user-tool" ] || { why 'the account'\''s tool was not installed'; return 1; }
            grep -q '^prereq: result: pass: 3 tools present on ' "$work/out" || { why 'not three tools present'; return 1; }
            [ "$(sort -u "$FIX/ssh.log")" = '-T -o BatchMode=yes -- machine sh -s' ] &&
                [ "$(wc -l < "$FIX/ssh.log")" -eq 3 ] || { why "ssh was run as: $(cat "$FIX/ssh.log")"; return 1; }
            case $dir in
                "$FIX/cache/builds/$key".??????) ;;
                *) why "$dir is not a build of the cache"; return 1 ;;
            esac
            record "$home_state/machine" "$home_state/ledger" || return 1
            ledger "$home_state/ledger" || return 1
            # shellcheck disable=SC2012 # the names are deployment's own
            [ "$(ls -A "$home_state" | tr '\n' ' ')" = 'ledger machine ' ] ||
                { why "the state has $(ls -A "$home_state")"; return 1; }
            untouched ;;
        again)
            begin
            run machine
            deployed || return 1
            snapshot "$FIX/home" "$FIX/cache" "$FIX/have" > "$work/before"
            run machine
            deployed || return 1
            { grep -q '^etude-version: result: reused ' "$work/out" &&
                grep -qx "etude-deploy: machine: unchanged $home_state/machine" "$work/out"; } ||
                { why 'the second deployment was not a reuse'; return 1; }
            [ "$(wc -l < "$FIX/cc.log")" -eq 4 ] || { why 'the second deployment compiled'; return 1; }
            snapshot "$FIX/home" "$FIX/cache" "$FIX/have" > "$work/after"
            cmp -s "$work/before" "$work/after" || {
                why 'the second deployment changed the machine:'
                diff "$work/before" "$work/after" >> "$work/why"
                return 1
            } ;;
        admin)
            begin
            rm -f "$FIX/have/admin-tool"
            run machine
            incomplete "[0-9a-f]* is deployed on machine, and a prerequisite is missing" || return 1
            { grep -qx 'prereq: an administrator runs:' "$work/out" &&
                grep -qx "    sudo touch \"\$FIX/have/admin-tool\"" "$work/out"; } ||
                { why 'the administrator'\''s command was not printed'; return 1; }
            [ ! -e "$FIX/have/admin-tool" ] || { why 'the administrator'\''s command ran'; return 1; }
            : > "$FIX/have/admin-tool"
            run machine
            deployed || return 1
            record "$home_state/machine" "$home_state/ledger" || return 1
            untouched ;;
        sudo)
            begin
            rm -f "$FIX/have/admin-tool"
            : > "$FIX/sudo"
            run machine
            deployed || return 1
            { [ -e "$FIX/have/admin-tool" ] && grep -qxF "sudo sh -c touch \"\$FIX/have/admin-tool\"" "$FIX/log"; } ||
                { why 'the administrator'\''s command did not run through sudo'; return 1; } ;;
        check)
            begin
            rm -f "$FIX/have/user-tool"
            run -n machine
            incomplete "[0-9a-f]* is deployed on machine, and a prerequisite is missing" || return 1
            [ ! -e "$FIX/have/user-tool" ] || { why '-n installed a tool'; return 1; }
            { grep -qx 'prereq: this account runs:' "$work/out" &&
                grep -qx "    touch \"\$FIX/have/user-tool\"" "$work/out"; } ||
                { why 'the account'\''s command was not printed'; return 1; }
            run machine
            deployed || return 1 ;;
        busy)
            begin
            run machine
            deployed || return 1
            old="$key $dir"
            printf 'etude-ledger 1\nclaim run-1.case-1.1\nsystem linux\nstate running\nend\n' > "$home_state/ledger/claims"
            echo confirmed > "$home_state/ledger/verdicts/run-1.case-1.1"
            sleep 600 &
            testpid=$!
            snapshot "$home_state/ledger" "$FIX/cache/versions/$key" "$dir" > "$work/before"
            change
            run machine
            restore
            deployed || return 1
            { [ "$key $dir" != "$old" ] && grep -q '^etude-version: result: built ' "$work/out"; } ||
                { why 'no new version was built'; return 1; }
            record "$home_state/machine" "$home_state/ledger" || return 1
            snapshot "$home_state/ledger" "$FIX/cache/versions/${old%% *}" "${old#* }" > "$work/after"
            cmp -s "$work/before" "$work/after" || {
                why 'the running test'\''s ledger or version changed:'
                diff "$work/before" "$work/after" >> "$work/why"
                return 1
            }
            kill -0 "$testpid" 2>/dev/null || { why 'the running test ended'; return 1; }
            kill "$testpid"
            wait "$testpid" 2>/dev/null
            testpid=
            untouched ;;
        format)
            begin
            run machine
            deployed || return 1
            printf 'etude-ledger 2\n' > "$home_state/ledger/claims"
            snapshot "$home_state" > "$work/before"
            change
            run machine
            restore
            incomplete 'the record of machine was not written: status 1' || return 1
            grep -q "^etude-deploy: the ledger $home_state/ledger is of the format 'etude-ledger 2', and version [0-9a-f]* admits by 'etude-ledger 1'; the machine keeps its version$" "$work/err" ||
                { why 'no report of the format'; return 1; }
            snapshot "$home_state" > "$work/after"
            cmp -s "$work/before" "$work/after" || { why 'the machine'\''s state changed'; return 1; } ;;
        rights)
            [ "$root" = 0 ] || return 0
            begin
            mkdir "$FIX/shared" && chmod 555 "$FIX/shared" || exit 1
            run -L "$FIX/shared" machine
            incomplete 'the record of machine was not written: status 1' || return 1
            { grep -q "^etude-deploy: this account cannot use the ledger $FIX/shared; its owner or an administrator runs:$" "$work/err" &&
                grep -qx "    chmod a+rwx '$FIX/shared'" "$work/err"; } ||
                { why 'the command that gives the right was not printed'; return 1; }
            [ -z "$(ls -A "$FIX/shared")" ] && [ ! -e "$home_state/machine" ] ||
                { why 'a refused ledger was changed or recorded'; return 1; }
            chmod 755 "$FIX/shared" || exit 1
            run -L "$FIX/shared" machine
            deployed || return 1
            record "$home_state/machine" "$FIX/shared" || return 1
            ledger "$FIX/shared" ;;
        lost)
            for n in 1 2 3; do
                begin
                echo "$n" > "$FIX/drop"
                run machine
                rm -f "$FIX/drop"
                if [ "$n" = 2 ]; then
                    incomplete 'etude was not built on machine' || return 1
                else
                    incomplete '*' || return 1
                fi
                ! grep -q 'result: deployed' "$work/out" || { why "lost at $n, and deployed"; return 1; }
                [ ! -e "$home_state/machine" ] || { why "lost at $n, and recorded"; return 1; }
                run machine
                deployed || { why "after the connection lost at $n"; return 1; }
                record "$home_state/machine" "$home_state/ledger" || return 1
                ledger "$home_state/ledger" || return 1
                untouched || return 1
            done ;;
        stopped)
            begin
            : > "$FIX/interrupt"
            run machine
            incomplete 'etude was not built on machine' || return 1
            [ ! -e "$home_state" ] || { why 'a stopped build made the state'; return 1; }
            run machine
            deployed || return 1
            old="$key $dir"
            change
            : > "$FIX/interrupt-record"
            run machine
            incomplete 'the record of machine was not written: status 143' || { restore; return 1; }
            # shellcheck disable=SC2012 # the names are deployment's own
            [ "$(ls -A "$home_state" | tr '\n' ' ')" = 'ledger machine ' ] ||
                { restore; why "a stopped record left $(ls -A "$home_state")"; return 1; }
            key=${old%% *} dir=${old#* }
            record "$home_state/machine" "$home_state/ledger" || { restore; return 1; }
            run machine
            restore
            deployed || return 1
            [ "$key $dir" != "$old" ] || { why 'the version did not change'; return 1; }
            record "$home_state/machine" "$home_state/ledger" ;;
        local)
            begin
            git -C "$TREE" init -q && git -C "$TREE" add -A &&
                git -C "$TREE" -c user.name=test -c user.email=test@example.com -c maintenance.auto=false -c gc.auto=0 commit -qm source || exit 1
            echo untracked > "$TREE/untracked"
            snapshot "$TREE" > "$work/before"
            run -S "$FIX/state" -l
            snapshot "$TREE" > "$work/after"
            rm -rf "$TREE/.git" "$TREE/untracked"
            deployed || return 1
            [ ! -s "$FIX/ssh.log" ] || { why 'a local deployment ran ssh'; return 1; }
            record "$FIX/state/machine" "$FIX/state/ledger" || return 1
            cmp -s "$work/before" "$work/after" || {
                why 'the clone deployed from changed:'
                diff "$work/before" "$work/after" >> "$work/why"
                return 1
            } ;;
        revision)
            begin
            git -C "$TREE" init -q && git -C "$TREE" add -A &&
                git -C "$TREE" -c user.name=test -c user.email=test@example.com -c maintenance.auto=false -c gc.auto=0 commit -qm source || exit 1
            sed 's/^#define LEDGER_MAGIC "etude-ledger 1"$/#define LEDGER_MAGIC "etude-ledger 9"/' \
                "$repo/tools/etude/ledger.c" > "$TREE/tools/etude/ledger.c" || exit 1
            run -r HEAD machine
            cp "$repo/tools/etude/ledger.c" "$TREE/tools/etude/ledger.c"
            rm -rf "$TREE/.git"
            deployed || return 1
            record "$home_state/machine" "$home_state/ledger" ;;
        kvm-open|kvm-unavailable|kvm-missing)
            (
                begin
                cp "$TREE/tools/etude/deploy.sh" "$work/deploy.kvm" || exit 1
                trap 'cp "$work/deploy.kvm" "$TREE/tools/etude/deploy.sh"' EXIT
                sed "s#/dev/kvm#$FIX/kvm#g" "$work/deploy.kvm" > "$TREE/tools/etude/deploy.sh" || exit 1
                wanted=tcg
                case $1 in
                    kvm-open)
                        echo 'device data' > "$FIX/kvm"
                        wanted='kvm tcg' ;;
                    kvm-unavailable) mkdir "$FIX/kvm" || exit 1 ;;
                esac
                if [ "$1" != kvm-missing ]; then
                    [ -r "$FIX/kvm" ] && [ -w "$FIX/kvm" ] ||
                        { why 'the device fixture lacks read or write permission'; exit 1; }
                fi
                PATH="$work/linux:$PATH" run machine
                deployed || exit 1
                grep -qx "accelerators $wanted" "$home_state/machine" ||
                    { why "the device state $1 did not record $wanted"; exit 1; }
                case $1 in
                    kvm-open)
                        [ "$(cat "$FIX/kvm")" = 'device data' ] ||
                            { why 'the device data changed'; exit 1; } ;;
                    kvm-missing)
                        [ ! -e "$FIX/kvm" ] ||
                            { why 'the probe created the missing device'; exit 1; } ;;
                esac
                untouched
            ) ;;
        ovmf-discovery)
            begin
            mkdir -p "$work/share/qemu" || exit 1
            : > "$work/share/qemu/edk2-x86_64-code.fd"
            : > "$work/share/qemu/edk2-i386-vars.fd"
            ovmf_code=
            run machine
            rm -rf "$work/share"
            deployed || return 1
            record "$home_state/machine" "$home_state/ledger" '' '' '' '' '' \
                "$work/mbin/../share/qemu/edk2-x86_64-code.fd" ;;
        ovmf-missing)
            begin
            ovmf_code=$FIX/absent.fd
            run machine
            deployed || return 1
            record "$home_state/machine" "$home_state/ledger" '' '' '' '' '' - ;;
        windows)
            begin
            rm -f "$FIX/have/user-tool"
            : > "$FIX/have/whpx"
            mkdir -p "$FIX/programfiles/qemu" || exit 1
            printf '#!/bin/sh\necho "QEMU emulator version 9.9.8 (windows fixture)\r"\n' \
                > "$FIX/programfiles/qemu/qemu-system-x86_64.exe"
            chmod 755 "$FIX/programfiles/qemu/qemu-system-x86_64.exe"
            mkdir -p "$FIX/programfiles/qemu/share" || exit 1
            : > "$FIX/programfiles/qemu/share/edk2-x86_64-code.fd"
            ovmf_code=
            run -w machine
            deployed || return 1
            grep -qx 'pwsh Get user-tool' "$FIX/log" || { why 'the account'\''s tool was not installed'; return 1; }
            [ "$(grep -cxF -- "-T -o BatchMode=yes -- machine $windows_command" "$FIX/ssh.log")" = 2 ] ||
                { why "ssh was run as: $(cat "$FIX/ssh.log")"; return 1; }
            case $dir in
                "$FIX/cache/builds/$key".??????) ;;
                *) why "$dir is not a build of the cache"; return 1 ;;
            esac
            record "$FIX/appdata/etude-state/machine" "$FIX/appdata/etude-state/ledger" winbox \
                'windows x86_64' 'QEMU emulator version 9.9.8 (windows fixture)' 'whpx tcg' "$windows_memory" \
                'C:/Program Files/qemu/share/edk2-x86_64-code.fd' ;;
    esac
}

rows='deploy again admin sudo check busy format rights lost stopped local revision kvm-open kvm-unavailable kvm-missing ovmf-discovery ovmf-missing windows'
[ "$mode" != ovmf ] || rows='ovmf-discovery ovmf-missing windows'
[ "$mode" != kvm ] || rows='kvm-open kvm-unavailable kvm-missing'

# rows: run every row against the copy, print `etude-deploy-test: failed:
# NAME` for each that fails, and fail when any did.
rows()
{
    failed=0
    for r in $rows; do
        : > "$work/why"
        if ! row "$r"; then
            echo "etude-deploy-test: failed: $r"
            sed 's/^/    /' "$work/why"
            failed=1
        fi
        if [ -n "$testpid" ]; then
            kill "$testpid" 2>/dev/null
            wait "$testpid" 2>/dev/null
            testpid=
        fi
    done
    return "$failed"
}

# expect_fault FAULT ROWS: with FAULT in the copy, require exactly ROWS to
# fail.
expect_fault()
{
    fault "$1" || { echo "etude-deploy-test: fault $1 changes nothing" >&2; return 1; }
    rows > "$work/mutated.out" && { echo "etude-deploy-test: fault passed: $1" >&2; return 1; }
    got=$(sed -n 's/^etude-deploy-test: failed: //p' "$work/mutated.out" | tr '\n' ' ')
    if [ "$got" != "$2 " ]; then
        echo "etude-deploy-test: fault $1 failed rows: $got; expected: $2" >&2
        return 1
    fi
}

faults='rewrite truncate-lock fresh-ledger keep-temp in-tree no-install prereq-pass no-rights no-format tree-format'
if [ "$mode" = texts ]; then
    for f in $faults; do
        fault "$f" || { echo "etude-deploy-test: fault $f changes nothing" >&2; exit 1; }
    done
    exit 0
fi
fault none
rows >&2 || exit 1
if [ "$mode" = mutate ]; then
    expect_fault rewrite 'again' || exit 1
    expect_fault truncate-lock 'again busy' || exit 1
    expect_fault fresh-ledger 'again busy' || exit 1
    expect_fault keep-temp 'stopped' || exit 1
    expect_fault in-tree 'local' || exit 1
    expect_fault no-install 'deploy sudo check windows' || exit 1
    expect_fault prereq-pass 'admin check' || exit 1
    expect_fault no-rights 'rights' || exit 1
    expect_fault no-format 'format' || exit 1
    expect_fault tree-format 'revision' || exit 1
    fault none
    rows >&2 || exit 1
    echo 'etude-deploy-test: result: pass: ten faults caught and restored'
fi
[ "$root" = 0 ] || echo 'etude-deploy-test: not run: rights: root can write any ledger'
echo "etude-deploy-test: result: pass: $(printf '%s\n' "$rows" | wc -w | tr -d ' ') rows"
