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
# the drive's free space is asked of Windows as well, through its PowerShell.
# PowerShell is started from /: Windows opens its working directory through
# WSL as the distribution's default user, who cannot enter another user's
# home of mode 750, and fails to start it there. A missing reading, including
# a query that does not finish within a minute, refuses the work. tests/run and
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
if ! command -v powershell.exe > /dev/null 2>&1; then
    echo "$name: result: refused: cannot check the Windows drive under $WSL_DISTRO_NAME: powershell.exe is missing" >&2
    exit 1
fi
# shellcheck disable=SC2016 # the dollars are PowerShell's
reading=$(cd / &&
    RACH2_DISTRO=$WSL_DISTRO_NAME WSLENV="${WSLENV:+$WSLENV:}RACH2_DISTRO" \
    timeout -k 5 60 powershell.exe -NoProfile -NonInteractive -Command '
        $ErrorActionPreference = "Stop"
        $p = @(Get-ChildItem HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss |
            ForEach-Object { Get-ItemProperty $_.PSPath } |
            Where-Object { $_.DistributionName -eq $env:RACH2_DISTRO })
        if ($p.Count -ne 1) { exit 1 }
        $r = [System.IO.Path]::GetPathRoot(($p[0].BasePath -replace "^\\\\\?\\", ""))
        "$r $((New-Object System.IO.DriveInfo $r).AvailableFreeSpace)"' </dev/null 2>/dev/null)
status=$?
if [ "$status" -ne 0 ]; then
    echo "$name: result: refused: cannot read free space on the Windows drive under $WSL_DISTRO_NAME (PowerShell status $status)" >&2
    exit 1
fi
drive=$(printf '%s\n' "$reading" | tr -d '\r' | awk '
    NR == 1 && NF == 2 && $1 ~ /^[A-Za-z]:\\$/ && $2 ~ /^[0-9]+$/ {
        drive = $1; free = $2; valid = 1; next
    }
    { valid = 0 }
    END { if (valid) printf "%s %.0f\n", drive, int(free / 1073741824) }')
if [ -z "$drive" ]; then
    echo "$name: result: refused: no valid free-space reading for the Windows drive under $WSL_DISTRO_NAME" >&2
    exit 1
elif [ "${drive#* }" -lt "$floor" ]; then
    echo "$name: result: refused: ${drive#* } GiB free on ${drive%% *}, which holds the disk of $WSL_DISTRO_NAME, below the $floor GiB $work needs" >&2
    exit 1
fi
exit 0
