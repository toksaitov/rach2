#!/bin/sh
# Run a Windows game with the GNU root's Proton and WineD3D on OpenGL.
set -eu

# Print the command line; a bad argument exits with status 2.
usage()
{
    echo 'usage: rach2-game [--id ID] [--prefix DIR] [--print] [--] PROGRAM [ARGUMENT...]'
}

# Report a launcher error and exit before starting Proton.
die()
{
    echo "rach2-game: $*" >&2
    exit 2
}

id=${SteamGameId:-0}
prefix=${STEAM_COMPAT_DATA_PATH:-}
print=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --id|--prefix)
            [ "$#" -ge 2 ] || die "$1 needs an argument"
            case "$1" in
                --id) id=$2 ;;
                --prefix) prefix=$2; [ -n "$prefix" ] || die 'empty prefix' ;;
            esac
            shift 2
            ;;
        --print) print=1; shift ;;
        --help) usage; exit 0 ;;
        --) shift; break ;;
        -*) die "unknown option: $1" ;;
        *) break ;;
    esac
done
case "$id" in ''|*[!0-9]*) die 'the game id must contain decimal digits' ;; esac
[ -n "$print" ] || [ "$#" -gt 0 ] || { usage >&2; exit 2; }

base=${XDG_DATA_HOME:-${HOME:?}/.local/share}/rach2/games
STEAM_COMPAT_DATA_PATH=${prefix:-$base/$id}
STEAM_COMPAT_CLIENT_INSTALL_PATH=${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$base/steam}
PROTON_LOG_DIR=${PROTON_LOG_DIR:-$STEAM_COMPAT_DATA_PATH/logs}
for path in "$STEAM_COMPAT_DATA_PATH" "$STEAM_COMPAT_CLIENT_INSTALL_PATH" "$PROTON_LOG_DIR"; do
    case "$path" in /*) ;; *) die "use an absolute directory: $path" ;; esac
done

PROTON_USE_WOW64=1
PROTON_USE_WINED3D=1
WINE_D3D_CONFIG=renderer=gl
PROTON_NO_FSYNC=0
PROTON_LOG=1
SteamGameId=$id
SteamAppId=$id
export PROTON_USE_WOW64 PROTON_USE_WINED3D WINE_D3D_CONFIG PROTON_NO_FSYNC
export PROTON_LOG PROTON_LOG_DIR SteamGameId SteamAppId
export STEAM_COMPAT_DATA_PATH STEAM_COMPAT_CLIENT_INSTALL_PATH

if [ -n "$print" ]; then
    printf '%s\n' "PROTON_USE_WOW64=$PROTON_USE_WOW64" \
        "PROTON_USE_WINED3D=$PROTON_USE_WINED3D" "WINE_D3D_CONFIG=$WINE_D3D_CONFIG" \
        "PROTON_NO_FSYNC=$PROTON_NO_FSYNC" "SteamGameId=$SteamGameId" \
        "SteamAppId=$SteamAppId" "PROTON_LOG=$PROTON_LOG" "PROTON_LOG_DIR=$PROTON_LOG_DIR" \
        "STEAM_COMPAT_DATA_PATH=$STEAM_COMPAT_DATA_PATH" \
        "STEAM_COMPAT_CLIENT_INSTALL_PATH=$STEAM_COMPAT_CLIENT_INSTALL_PATH"
    exit 0
fi

[ "$(uname -m)" = x86_64 ] || die 'proton needs the x86-64 GNU root'
[ -x /opt/proton/proton ] || die 'missing /opt/proton/proton'
mkdir -p "$STEAM_COMPAT_DATA_PATH" "$STEAM_COMPAT_CLIENT_INSTALL_PATH" "$PROTON_LOG_DIR"
# Boot a new prefix and give WineD3D the device's memory, which games size
# their textures by. The GPUs Rach2 drives draw from system memory, and virgl
# reports a size that matches no memory of guest or host (16,430,080 MiB).
if [ ! -f "$STEAM_COMPAT_DATA_PATH/pfx/user.reg" ]; then
    mib=$(awk '$1 == "MemTotal:" { print int($2 / 1024) }' /proc/meminfo)
    case "$mib" in ''|*[!0-9]*|0) die 'cannot read MemTotal from /proc/meminfo' ;; esac
    status=0
    /opt/proton/proton run 'c:\windows\system32\wineboot.exe' -u || status=$?
    [ "$status" -eq 0 ] || { echo "rach2-game: prefix: wineboot failed: status $status" >&2; exit 1; }
    /opt/proton/proton run 'c:\windows\system32\reg.exe' add 'HKCU\Software\Wine\Direct3D' \
        /v VideoMemorySize /t REG_SZ /d "$mib" /f > /dev/null || status=$?
    [ "$status" -eq 0 ] || { echo "rach2-game: prefix: reg failed: status $status" >&2; exit 1; }
    echo "rach2-game: prefix: made $STEAM_COMPAT_DATA_PATH/pfx with VideoMemorySize $mib MiB" >&2
fi
exec /opt/proton/proton run "$@"
