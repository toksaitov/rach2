#!/bin/sh
#
# space.sh: refuse work that would fill the disk under a tree.
#
# Usage: sh space.sh NAME WORK FLOOR DIR
#
# It succeeds when the filesystem of DIR has FLOOR GiB free, and otherwise
# prints `NAME: result: refused: ...` with the free space and WORK, what
# needs it, such as `a run`, and fails. FLOOR 0 checks nothing. Under WSL the
# distribution's disk is a file that grows on a Windows drive, and when that
# drive fills, the Linux root turns read-only part way through the work, so
# the drive's free space is read as well: df(1) on the disk file, through the
# drive's mount under RACH2_WSL_MOUNTS (/mnt by default, the root that
# `[automount]` in /etc/wsl.conf sets), which gives the free space Windows
# reports without starting a Windows program. Starting one through WSL's
# interop can stall for many minutes at a time, so PowerShell is asked only
# for the disk file's Windows path, from the registry, when none is known.
# The path is kept in `$XDG_CACHE_HOME/rach2` or else in DIR's `build/cache`,
# and asked for again when the mount has no file at the kept path. PowerShell is
# started from /: Windows opens its working directory through WSL as the
# distribution's default user, who cannot enter another user's home of mode 750,
# and fails to start it there. A missing path or reading, including a query or a
# df that does not finish within a minute, refuses the work. tests/run and
# tools/setup/preflight.sh call it with the floor RACH2_MIN_FREE_GIB gives.
#
set -u

name=$1 work=$2 floor=$3 dir=$4
case "$floor" in
    ''|*[!0-9]*)
        echo "$name: result: refused: RACH2_MIN_FREE_GIB must be a whole number of GiB" >&2
        exit 1 ;;
esac
[ "$floor" -ne 0 ] || exit 0
free=$(df -Pk "$dir" | awk 'NR == 2 {printf "%d\n", $4 / 1048576}')
if [ -z "$free" ] || [ "$free" -lt "$floor" ]; then
    echo "$name: result: refused: ${free:-unknown} GiB free under $dir, below the $floor GiB $work needs" >&2
    exit 1
fi
[ -n "${WSL_DISTRO_NAME:-}" ] || exit 0
mounts=${RACH2_WSL_MOUNTS:-/mnt}
if [ -n "${XDG_CACHE_HOME:-}" ]; then
    cache=$XDG_CACHE_HOME/rach2
else
    cache=$dir/build/cache
fi

# drive_free DISK: print the drive of the Windows path DISK and its free GiB,
# read through the drive's mount, or fail when that mount has no such file.
drive_free()
{
    case $1 in [A-Za-z]:\\?*) ;; *) return 1 ;; esac
    letter=$(printf '%s\n' "${1%%:*}" | tr '[:upper:]' '[:lower:]')
    file=$mounts/$letter/$(printf '%s\n' "${1#?:\\}" | tr "\\\\" /)
    timeout -k 5 60 df -Pk "$file" 2> /dev/null | awk -v letter="${1%%:*}" '
        NR == 2 && toupper($1) == toupper(letter) ":\\" && $4 ~ /^[0-9]+$/ {
            printf "%s:\\ %d\n", toupper(letter), $4 / 1048576; found = 1
        }
        END { exit !found }'
}

disk=
[ -z "$cache" ] || IFS= read -r disk 2> /dev/null < "$cache/wsl-disk.$WSL_DISTRO_NAME"
if [ -z "$disk" ] || ! drive=$(drive_free "$disk"); then
    if ! command -v powershell.exe > /dev/null 2>&1; then
        echo "$name: result: refused: cannot check the Windows drive under $WSL_DISTRO_NAME: powershell.exe is missing" >&2
        exit 1
    fi
    # shellcheck disable=SC2016 # the dollars are PowerShell's
    disk=$(cd / &&
        RACH2_DISTRO=$WSL_DISTRO_NAME WSLENV="${WSLENV:+$WSLENV:}RACH2_DISTRO" \
        timeout -k 5 60 powershell.exe -NoProfile -NonInteractive -Command '
            $ErrorActionPreference = "Stop"
            $p = @(Get-ChildItem HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss |
                ForEach-Object { Get-ItemProperty $_.PSPath } |
                Where-Object { $_.DistributionName -eq $env:RACH2_DISTRO })
            if ($p.Count -ne 1) { exit 1 }
            $v = $p[0].VhdFileName
            if (-not $v) { $v = "ext4.vhdx" }
            Join-Path ($p[0].BasePath -replace "^\\\\\?\\", "") $v' </dev/null 2>/dev/null)
    status=$?
    if [ "$status" -ne 0 ]; then
        echo "$name: result: refused: cannot read free space on the Windows drive under $WSL_DISTRO_NAME (PowerShell status $status)" >&2
        exit 1
    fi
    disk=$(printf '%s\n' "$disk" | tr -d '\r' | awk 'NR == 1 { line = $0 } END { if (NR == 1) print line }')
    if ! drive=$(drive_free "$disk"); then
        echo "$name: result: refused: no valid free-space reading for the Windows drive under $WSL_DISTRO_NAME, whose disk is ${disk:-unknown}" >&2
        exit 1
    fi
    kept=$cache/wsl-disk.$WSL_DISTRO_NAME
    [ -z "$cache" ] || { mkdir -p "$cache" && printf '%s\n' "$disk" > "$kept.$$" &&
        mv -f "$kept.$$" "$kept"; } 2> /dev/null || rm -f "$kept.$$"
fi
if [ "${drive#* }" -lt "$floor" ]; then
    echo "$name: result: refused: ${drive#* } GiB free on ${drive%% *}, which holds the disk of $WSL_DISTRO_NAME, below the $floor GiB $work needs" >&2
    exit 1
fi
exit 0
