#!/bin/sh
#
# deploy.sh: make a machine ready to run etude: check its prerequisites and
# install those this account may, build etude there, prepare the ledger of
# its resource claims, and record what the machine can do. Running it again,
# after a deployment that finished or one that was cut short, changes only
# what the first left undone.
#
# Usage: sh tools/etude/deploy.sh [-n] [-r REV] [-c CC] [-C CACHE] [-S STATE]
#            [-L LEDGER] [-w] HOST
#        sh tools/etude/deploy.sh [-n] [-r REV] [-c CC] [-C CACHE] [-S STATE]
#            [-L LEDGER] -l
#
# HOST is a destination of ssh(1), a Windows machine with -w, and -l deploys
# on this machine. -n checks the prerequisites and installs none. -r, -c and
# -C are version.sh's. STATE is the directory of the machine's record and
# LEDGER the ledger's, each a path on the machine, by default the platform's
# (tools/etude/Readme.md, The machine record).
#
# Deployment creates no account or key, changes no clone, deletes nothing of
# the machine's, writes no raw disk, restarts nothing and stops no test. The
# prerequisites are checked by tools/setup/prereq.sh in a scratch copy on the
# machine, removed afterwards, and through PowerShell for Windows. A ledger
# is made where it is missing, and files that exist in it are never written.
#
# It prints the steps and their output, and last `etude-deploy: result:
# deployed KEY` or `etude-deploy: result: incomplete: WHY`. Exit 0 when the
# machine is deployed and has every prerequisite, 1 when something is left
# undone, 2 on a usage error.
#
set -u

usage()
{
    echo 'usage: deploy.sh [-n] [-r REV] [-c CC] [-C CACHE] [-S STATE] [-L LEDGER] [-w] HOST | -l' >&2
    exit 2
}

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd) || exit 2
tree=$(CDPATH='' cd -- "$here/../.." && pwd) || exit 2
install=--install
rev=
cc=
cache=
state=
ledger=
windows=0
mode=remote
while getopts nr:c:C:S:L:wl opt; do
    case $opt in
        n) install= ;;
        r) rev=$OPTARG ;;
        c) cc=$OPTARG ;;
        C) cache=$OPTARG ;;
        S) state=$OPTARG ;;
        L) ledger=$OPTARG ;;
        w) windows=1 ;;
        l) mode=local ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
if [ "$mode" = remote ]; then
    [ "$#" -eq 1 ] || usage
    host=$1
    case $host in
        -*|*[!A-Za-z0-9@._:-]*|'') usage ;;
    esac
else
    [ "$#" -eq 0 ] && [ "$windows" = 0 ] || usage
    host=$(uname -n)
fi

work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-deploy.XXXXXX") || exit 1

# incomplete WHY: report the deployment unfinished and exit 1.
incomplete()
{
    echo "etude-deploy: result: incomplete: $*"
    exit 1
}

# quote TEXT: print TEXT as one word that sh reads back as TEXT.
quote()
{
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

# machine_sh: run the script on standard input with the machine's POSIX sh,
# as version.sh does.
machine_sh()
{
    if [ "$mode" = local ]; then
        sh -s
    elif [ "$windows" = 1 ]; then
        ssh -T -o BatchMode=yes -- "$host" 'C:\msys64\usr\bin\env.exe PATH=/usr/bin:/ucrt64/bin sh -s'
    else
        ssh -T -o BatchMode=yes -- "$host" 'sh -s'
    fi
}

nonce=$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')
[ "${#nonce}" -eq 16 ] || { echo 'etude-deploy: cannot read /dev/urandom' >&2; exit 1; }
end=@etude-$nonce

# The prerequisites. A POSIX machine gets prereq.sh and the files its probes
# and install lines read, each the body of a quoted here-document, and runs
# them in a scratch tree, since a probe runs in the tree's root.
echo "etude-deploy: prerequisites of $host"
if [ "$windows" = 1 ]; then
    RACH2_PREREQ_POWERSHELL="ssh -T -o BatchMode=yes -- $host powershell -NoProfile -NonInteractive -EncodedCommand"
    export RACH2_PREREQ_POWERSHELL
    # shellcheck disable=SC2086 # install is one flag or none
    sh "$tree/tools/setup/prereq.sh" --platform windows $install > "$work/prereq"
    prereq=$?
else
    # shellcheck disable=SC2016 # the dollars are the machine's script's
    {
        echo "ETUDE_INSTALL=$install"
        echo 'etude_prereq()'
        echo '{'
        echo '    d=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-prereq.XXXXXX") || return 2'
        echo '    trap '\''rm -rf "$d"'\'' EXIT'
        echo "    trap 'exit 129' HUP"
        echo "    trap 'exit 130' INT"
        echo "    trap 'exit 143' TERM"
        echo '    mkdir -p "$d/tools/setup" || return 2'
        for f in prereq.sh prereqs e2fsprogs_path.sh ovmf_path.sh elfwrap.sh; do
            [ -z "$(tail -c 1 "$tree/tools/setup/$f")" ] || { echo "etude-deploy: $f: the last line has no newline" >&2; exit 1; }
            ! grep -qx -- "$end" "$tree/tools/setup/$f" || { echo "etude-deploy: $f: a line is $end" >&2; exit 1; }
            echo "    cat > \"\$d/tools/setup/$f\" <<'$end' || return 2"
            cat "$tree/tools/setup/$f"
            echo "$end"
        done
        echo '    (cd "$d" && sh tools/setup/prereq.sh $ETUDE_INSTALL)'
        echo '}'
        echo 'etude_prereq < /dev/null'
    } > "$work/prereq.sh" || exit 1
    machine_sh < "$work/prereq.sh" > "$work/prereq"
    prereq=$?
fi
cat "$work/prereq"
case $prereq in
    0|1) ;;
    *) incomplete "the prerequisites of $host were not checked: status $prereq" ;;
esac

echo "etude-deploy: etude on $host"
set --
[ -z "$rev" ] || set -- "$@" -r "$rev"
[ -z "$cc" ] || set -- "$@" -c "$cc"
[ -z "$cache" ] || set -- "$@" -C "$cache"
if [ "$mode" = local ]; then
    set -- "$@" -l
elif [ "$windows" = 1 ]; then
    set -- "$@" -w "$host"
else
    set -- "$@" "$host"
fi
sh "$here/version.sh" "$@" > "$work/version" || incomplete "etude was not built on $host"
cat "$work/version"
key=$(sed -n 's/^etude-version: result: [a-z]* \([0-9a-f]*\) .*/\1/p' "$work/version")
dir=$(sed -n 's/^etude-version: result: [a-z]* [0-9a-f]* //p' "$work/version")
[ -n "$key" ] && [ -n "$dir" ] || incomplete "version.sh gave no version"

if [ -n "$rev" ]; then
    git -C "$tree" show "$rev:tools/etude/ledger.c" > "$work/ledger.c" || exit 1
else
    cp "$tree/tools/etude/ledger.c" "$work/ledger.c" || exit 1
fi
admission=$(sed -n 's/^#define LEDGER_MAGIC "\(.*\)"$/\1/p' "$work/ledger.c")
[ -n "$admission" ] || { echo 'etude-deploy: ledger.c gives no format' >&2; exit 1; }
whpx=0
! grep -q '^  \[PASS\]  *whpx$' "$work/prereq" || whpx=1

# The record and the ledger, by a script that, as bootstrap.sh's, defines
# functions until its last line.
echo "etude-deploy: the machine record of $host"
{
    echo "ETUDE_NONCE=$nonce"
    echo "ETUDE_STATE=$(quote "$state")"
    echo "ETUDE_LEDGER=$(quote "$ledger")"
    echo "ETUDE_KEY=$key"
    echo "ETUDE_DIR=$(quote "$dir")"
    echo "ETUDE_ADMISSION=$(quote "$admission")"
    echo "ETUDE_WHPX=$whpx"
    cat "$here/bootstrap.sh"
    echo 'deploy_find_ovmf()'
    echo '{'
    cat "$tree/tools/setup/ovmf_path.sh"
    echo '}'
    cat <<'EOF'

# deploy_fail WHAT: report WHAT on stderr and end with status 1.
deploy_fail()
{
    printf 'etude-deploy: %s\n' "$*" >&2
    exit 1
}

# deploy_number: copy a line of standard input that is a decimal number to
# standard output, or print `-` for anything else.
deploy_number()
{
    read -r n || n=
    case $n in
        *[!0-9]*|'') echo - ;;
        *) printf '%s\n' "$n" ;;
    esac
}

# deploy_cpus, deploy_memory: print the machine's processors and memory in
# bytes, or `-` when the system does not say.
deploy_cpus()
{
    case $platform in
        macos*) sysctl -n hw.ncpu ;;
        *) nproc || getconf _NPROCESSORS_ONLN ;;
    esac 2>/dev/null | deploy_number
}

deploy_memory()
{
    case $platform in
        macos*) sysctl -n hw.memsize 2>/dev/null | deploy_number ;;
        *)
            kib=$(sed -n 's/^MemTotal: *\([0-9][0-9]*\) kB$/\1/p' /proc/meminfo 2>/dev/null)
            if [ -n "$kib" ]; then echo $((kib * 1024)); else echo -; fi ;;
    esac
}

# deploy_qemu: print the path of the machine's qemu-system-x86_64, or fail.
deploy_qemu()
{
    case $platform in
        windows*)
            pf=${ProgramFiles:-${PROGRAMFILES:-}}
            [ -n "$pf" ] || return 1
            q=$(cygpath -u "$pf")/qemu/qemu-system-x86_64.exe
            [ -x "$q" ] && printf '%s\n' "$q" ;;
        rach2*) return 1 ;;
        *) command -v qemu-system-x86_64 ;;
    esac
}

# deploy_ovmf QEMU: print the readable x86-64 firmware's code image, or `-`.
# Windows paths use the form C:/... that native QEMU reads.
deploy_ovmf()
{
    (
        [ -n "$1" ] || { echo -; exit; }
        PATH=${1%/*}:$PATH
        case $platform in
            windows*) RACH2_OVMF_CODE=${RACH2_OVMF_CODE:-${1%/*}/share/edk2-x86_64-code.fd} ;;
        esac
        deploy_find_ovmf
        if [ -z "$OVMF_CODE" ] || [ ! -f "$OVMF_CODE" ] || [ ! -r "$OVMF_CODE" ]; then
            echo -
        elif [ "${platform%% *}" = windows ]; then
            cygpath -m "$OVMF_CODE"
        else
            printf '%s\n' "$OVMF_CODE"
        fi
    )
}

# deploy_accels QEMU: print the accelerators this account can run guests
# under, or `native` on Rach2, whose cases run on the machine itself.
deploy_accels()
{
    case $platform in
        rach2*) echo native; return ;;
    esac
    [ -n "$1" ] || { echo -; return; }
    a=
    case $platform in
        linux*) [ -r /dev/kvm ] && [ -w /dev/kvm ] && a='kvm ' ;;
        macos*) [ "$(sysctl -n kern.hv_support 2>/dev/null)" = 1 ] && a='hvf ' ;;
        windows*) [ "$ETUDE_WHPX" = 1 ] && a='whpx ' ;;
    esac
    echo "${a}tcg"
}

# deploy_ledger DIR: make the ledger DIR where it is missing, or require this
# account to be able to use the one there and its format to be the version's.
# A file that exists in it is read and never written.
deploy_ledger()
{
    l=$1
    if [ -e "$l" ] || [ -L "$l" ]; then
        [ -d "$l" ] || deploy_fail "the ledger $l is not a directory"
        rights=
        [ -w "$l" ] && [ -x "$l" ] || rights="$rights
    chmod a+rwx '$l'"
        if [ -e "$l/verdicts" ]; then
            [ -d "$l/verdicts" ] && [ -w "$l/verdicts" ] && [ -x "$l/verdicts" ] ||
                rights="$rights
    chmod a+rwx '$l/verdicts'"
        fi
        if [ -e "$l/lock" ]; then
            [ -r "$l/lock" ] && [ -w "$l/lock" ] || rights="$rights
    chmod a+rw '$l/lock'"
        fi
        if [ -e "$l/claims" ]; then
            [ -r "$l/claims" ] || rights="$rights
    chmod a+r '$l/claims'"
        fi
        [ -z "$rights" ] ||
            deploy_fail "this account cannot use the ledger $l; its owner or an administrator runs:$rights"
        if [ -e "$l/claims" ]; then
            IFS= read -r format < "$l/claims" || :
            [ "$format" = "$ETUDE_ADMISSION" ] ||
                deploy_fail "the ledger $l is of the format '$format', and version $ETUDE_KEY admits by '$ETUDE_ADMISSION'; the machine keeps its version"
        fi
    fi
    mkdir -p "$l/verdicts" || deploy_fail "cannot make the ledger $l"
    [ -e "$l/lock" ] || : >> "$l/lock" || deploy_fail "cannot make $l/lock"
}

# deploy_cleanup: remove the record this run was writing.
deploy_cleanup()
{
    [ -z "$tmp" ] || rm -f "$tmp"
}

# deploy_main: prepare the ledger, and write the machine's record when it
# differs from the one there, so that a deployment that changes nothing
# writes nothing. Prints `@etude-deploy NONCE recorded|unchanged
# PATH`.
deploy_main()
{
    platform=$(etude_platform) || deploy_fail "unknown system $(uname -s)"
    root=$ETUDE_STATE
    if [ -z "$root" ]; then
        case $platform in
            windows*)
                [ -n "${LOCALAPPDATA:-}" ] || deploy_fail 'LOCALAPPDATA is not set'
                root=$(cygpath -u "$LOCALAPPDATA") || deploy_fail "cygpath cannot read $LOCALAPPDATA"
                root=$root/etude-state ;;
            *)
                [ -n "${HOME:-}" ] || deploy_fail 'HOME is not set'
                root=${XDG_STATE_HOME:-$HOME/.local/state}/etude ;;
        esac
    fi
    ledger=${ETUDE_LEDGER:-$root/ledger}
    tmp=
    trap deploy_cleanup EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    mkdir -p "$root" || deploy_fail "cannot make $root"
    deploy_ledger "$ledger"
    shown=$ledger
    case $platform in
        windows*) shown=$(cygpath -m "$ledger") || deploy_fail "cygpath cannot read $ledger" ;;
    esac
    q=$(deploy_qemu) || q=
    v=-
    [ -z "$q" ] || v=$("$q" --version 2>/dev/null | sed -n 1p | tr -d '\r')
    record="etude-machine 1
host $(uname -n)
platform $platform
cpus $(deploy_cpus)
memory_bytes $(deploy_memory)
qemu ${v:--}
ovmf_x86_64 $(deploy_ovmf "$q")
accelerators $(deploy_accels "$q")
admission $ETUDE_ADMISSION
ledger $shown
version $ETUDE_KEY $ETUDE_DIR"
    if [ -f "$root/machine" ] && [ "$(cat "$root/machine")" = "$record" ]; then
        echo "@etude-deploy $ETUDE_NONCE unchanged $root/machine"
        return
    fi
    tmp=$(mktemp "$root/machine.XXXXXX") || deploy_fail "cannot write in $root"
    printf '%s\n' "$record" > "$tmp" || deploy_fail "cannot write the record in $root"
    mv -f "$tmp" "$root/machine" || deploy_fail "cannot write $root/machine"
    tmp=
    echo "@etude-deploy $ETUDE_NONCE recorded $root/machine"
}

deploy_main < /dev/null
EOF
} > "$work/machine.sh" || exit 1
machine_sh < "$work/machine.sh" > "$work/machine"
status=$?
grep -v "^@etude-deploy $nonce " "$work/machine" >&2
result=$(sed -n "s/^@etude-deploy $nonce //p" "$work/machine")
[ "$status" -eq 0 ] || incomplete "the record of $host was not written: status $status"
case $result in
    recorded\ *|unchanged\ *) echo "etude-deploy: machine: $result" ;;
    *) incomplete "the machine gave no record" ;;
esac
[ "$prereq" = 0 ] || incomplete "$key is deployed on $host, and a prerequisite is missing"
echo "etude-deploy: result: deployed $key"
