#!/bin/sh
#
# tools/setup/prereq.sh: check a platform for the tools tools/setup/prereqs
# lists, and print, or with --install run, the commands that install the
# missing ones.
#
# Usage: sh tools/setup/prereq.sh [--install | --list] [--platform NAME]
#            [--recipe FILE]
#
# The platform is the one this runs on: macos on Darwin, rach2 on a kernel
# whose release ends in -rach2, linux on any other Linux. --platform windows
# checks the Windows machine through PowerShell: RACH2_PREREQ_POWERSHELL is
# the command that runs a script given as its last argument, encoded as
# PowerShell's -EncodedCommand takes it, and is `powershell.exe -NoProfile
# -NonInteractive -EncodedCommand` by default, which reaches the Windows side
# of the machine from WSL; `ssh HOST powershell -NoProfile -NonInteractive
# -EncodedCommand` reaches one over SSH. A POSIX platform other than the
# local one is probed here, which only a test of this script wants.
#
# Each name is probed and reported PASS or MISSING. The install commands of
# the missing ones follow, in the recipe's order, under who runs them: this
# account; an administrator, through sudo on a POSIX platform or in an
# elevated PowerShell on Windows; or a person at the machine. The account has
# administrator rights when it is root, when `sudo -n true` succeeds, or on
# Windows when its PowerShell runs elevated. With --install the commands this
# account may run are run, an administrator's among them when it has the
# rights, every name is probed again, and the commands left are printed. A
# person's commands are never run.
#
# --list probes nothing and prints the platform's section as lines of
# tab-separated fields: `N STANZA NAME PROBE` for each name and `I STANZA WHO
# COMMAND` for each install line, with the placeholders replaced.
#
# Exit 0 when every name is present, 1 when one is missing, 2 on a usage
# error or a malformed recipe.
#
set -u

usage()
{
    echo 'usage: prereq.sh [--install | --list] [--platform linux|macos|windows|rach2] [--recipe FILE]' >&2
    exit 2
}

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd) || exit 2
tree=$(CDPATH='' cd -- "$here/../.." && pwd) || exit 2
recipe=$here/prereqs
install=0
list=0
platform=
while [ "$#" -gt 0 ]; do
    case $1 in
        --install) install=1 ;;
        --list) list=1 ;;
        --platform) [ "$#" -ge 2 ] || usage; platform=$2; shift ;;
        --recipe) [ "$#" -ge 2 ] || usage; recipe=$2; shift ;;
        *) usage ;;
    esac
    shift
done
[ "$install$list" != 11 ] || usage
if [ -z "$platform" ]; then
    case $(uname -s) in
        Darwin) platform=macos ;;
        Linux)
            case $(uname -r) in
                *-rach2) platform=rach2 ;;
                *) platform=linux ;;
            esac ;;
        *) echo "prereq: unknown system $(uname -s); name one with --platform" >&2; exit 2 ;;
    esac
fi
case $platform in
    linux|macos|windows|rach2) ;;
    *) usage ;;
esac
[ -r "$recipe" ] || { echo "prereq: cannot read $recipe" >&2; exit 2; }

tmp=
trap '[ -z "$tmp" ] || rm -rf "$tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
tmp=$(mktemp -d "${TMPDIR:-/tmp}/rach2-prereq.XXXXXX") || exit 2

# The recipe's section for the platform, in the form --list prints.
tab=$(printf '\t')
awk -v want="$platform" -v tree="$tree" -v user="$(id -un)" -v file="$recipe" '
function bad(why) { printf "prereq: %s:%d: %s\n", file, NR, why > "/dev/stderr"; failed = 1; exit 2 }
function put(s, from, to,    i, out) {
    out = ""
    while ((i = index(s, from)) > 0) {
        out = out substr(s, 1, i - 1) to
        s = substr(s, i + length(from))
    }
    return out s
}
function rest(s) { sub(/^[a-z]+[ ]+/, "", s); return s }
function close_stanza() {
    if (stanza == 0) return
    if (probe == "") bad("stanza " stanza " has no probe")
    if (installs == 0) bad("stanza " stanza " has no install line")
    if (section == want)
        for (n = 1; n <= nnames; n++)
            printf "N\t%d\t%s\t%s\n", stanza, names[n], put(probe, "@TOOL@", names[n])
}
/^[ ]*(#|$)/ { next }
$1 == "platform" {
    close_stanza(); stanza = 0
    if (NF != 2 || $2 !~ /^(linux|macos|windows|rach2)$/) bad("unknown platform: " $0)
    if ($2 in seen_platform) bad("platform " $2 " named twice")
    seen_platform[$2] = 1
    section = $2
    next
}
section == "" { bad("a line before the first platform") }
$1 == "need" {
    close_stanza()
    if (NF < 2) bad("need names nothing")
    stanza++; probe = ""; installs = 0; nnames = 0
    for (f = 2; f <= NF; f++) {
        if ((section SUBSEP $f) in seen) bad($f " is needed twice on " section)
        seen[section, $f] = 1
        names[++nnames] = $f
    }
    next
}
stanza == 0 { bad("a line before the first need of " section) }
$1 == "probe" {
    if (probe != "") bad("a second probe")
    if (installs > 0) bad("a probe after an install line")
    probe = rest($0)
    if (probe == "probe") bad("an empty probe")
    next
}
$1 == "user" || $1 == "admin" || $1 == "manual" {
    command = rest($0)
    if (command == $1) bad("an empty install line")
    installs++
    if (section == want)
        printf "I\t%d\t%s\t%s\n", stanza, $1, put(put(command, "@TREE@", tree), "@USER@", user)
    next
}
{ bad("unknown line: " $1) }
END {
    if (failed) exit 2
    close_stanza()
    if (!(want in seen_platform)) { printf "prereq: %s: no platform %s\n", file, want > "/dev/stderr"; exit 2 }
}' "$recipe" > "$tmp/rows" || exit 2
if [ "$list" = 1 ]; then
    cat "$tmp/rows"
    exit 0
fi

# pwsh SCRIPT: run SCRIPT in the Windows machine's PowerShell, after the
# definitions the recipe's probes use, and exit as it does.
pwsh()
{
    # shellcheck disable=SC2016 # the dollars are PowerShell's
    printf '%s\n%s\n' \
        '$ErrorActionPreference = "Stop"; $ProgressPreference = "SilentlyContinue"; $env:Path = "$env:LOCALAPPDATA\Microsoft\WindowsApps;$env:Path"; function Runs($exe) { try { & $exe --version *> $null; $LASTEXITCODE -eq 0 } catch { $false } }; function Has($path) { Test-Path -LiteralPath $path }' \
        "$1" | iconv -f UTF-8 -t UTF-16LE | base64 | tr -d '\n' > "$tmp/encoded" || return 1
    # shellcheck disable=SC2086 # the command is words, split as given
    (cd / && ${RACH2_PREREQ_POWERSHELL:-powershell.exe -NoProfile -NonInteractive -EncodedCommand} "$(cat "$tmp/encoded")")
}

# present PROBE: whether PROBE succeeds.
present()
{
    if [ "$platform" = windows ]; then
        pwsh "if ($1) { exit 0 } else { exit 1 }" < /dev/null > /dev/null 2>&1
        return
    fi
    rm -rf "$tmp/probe" && mkdir "$tmp/probe" || return 1
    (
        cd "$tree" || exit 1
        if [ "$platform" != rach2 ]; then
            . "$here/e2fsprogs_path.sh"
        fi
        PREREQ_TMP=$tmp/probe sh -c "$1"
    ) < /dev/null > /dev/null 2>&1
}

# probe_all: probe every name, write the stanzas with a name missing to
# tmp/missing, and print the report.
probe_all()
{
    : > "$tmp/missing"
    total=0 absent=0
    while IFS="$tab" read -r kind stanza name probe; do
        [ "$kind" = N ] || continue
        total=$((total + 1))
        if present "$probe"; then
            printf '  [PASS]     %s\n' "$name"
        else
            printf '  [MISSING]  %s\n' "$name"
            absent=$((absent + 1))
            echo "$stanza" >> "$tmp/missing"
        fi
    done < "$tmp/rows"
}

# commands: print the distinct install lines of the missing stanzas, as
# `WHO<tab>COMMAND`, in the recipe's order.
commands()
{
    while IFS="$tab" read -r kind stanza who command; do
        [ "$kind" = I ] && grep -qx "$stanza" "$tmp/missing" &&
            printf '%s\t%s\n' "$who" "$command"
    done < "$tmp/rows" | awk '!seen[$0]++'
}

if [ "$platform" = windows ]; then
    if pwsh 'if (([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { exit 0 } else { exit 1 }' < /dev/null > /dev/null 2>&1; then
        rights=elevated
    else
        rights=no
    fi
elif [ "$(id -u)" = 0 ]; then
    rights=root
elif sudo -n true < /dev/null > /dev/null 2>&1; then
    rights=sudo
else
    rights=no
fi
echo "prereq: $platform; administrator rights: $rights"
probe_all

if [ "$install" = 1 ] && [ "$absent" -gt 0 ]; then
    commands > "$tmp/run"
    while IFS="$tab" read -r who command; do
        case $who/$rights in
            manual/*|admin/no) continue ;;
        esac
        echo "prereq: running: $command"
        if [ "$platform" = windows ]; then
            pwsh "$command
if (\$LASTEXITCODE) { exit \$LASTEXITCODE }" < /dev/null
        elif [ "$who/$rights" = admin/sudo ]; then
            (cd "$tree" && sudo -n sh -c "$command") < /dev/null
        else
            (cd "$tree" && sh -c "$command") < /dev/null
        fi || echo "prereq: failed: $command" >&2
    done < "$tmp/run"
    echo "prereq: probing again"
    probe_all
fi

if [ "$absent" -eq 0 ]; then
    echo "prereq: result: pass: $total tools present on $platform"
    exit 0
fi
commands > "$tmp/left"
# heading WHO: print the heading of WHO's commands.
heading()
{
    case $1/$platform/$rights in
        user/windows/*) echo 'prereq: this account runs, in PowerShell on the Windows machine:' ;;
        user/*) echo 'prereq: this account runs:' ;;
        admin/windows/elevated) echo 'prereq: this account runs, in its elevated PowerShell on the Windows machine:' ;;
        admin/windows/*) echo 'prereq: an administrator runs, in an elevated PowerShell on the Windows machine:' ;;
        admin/*/root|admin/*/sudo) echo 'prereq: this account runs, as an administrator:' ;;
        admin/*) echo 'prereq: an administrator runs:' ;;
        manual/windows/*) echo 'prereq: a person at the Windows machine runs:' ;;
        manual/*) echo 'prereq: a person at the machine runs:' ;;
    esac
}
for who in user admin manual; do
    first=1
    while IFS="$tab" read -r kind command; do
        [ "$kind" = "$who" ] || continue
        [ "$first" = 0 ] || heading "$who"
        first=0
        if [ "$who" = admin ] && [ "$platform" != windows ] && [ "$rights" != root ]; then
            command="sudo $command"
        fi
        printf '    %s\n' "$command"
    done < "$tmp/left"
done
echo "prereq: result: missing: $absent of $total tools on $platform"
exit 1
