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
# Wine keeps 30 ms queued in PulseAudio, less than a wakeup's delay while the
# machine is busy; libpulse takes this target instead, and the server raises
# it over the OSS sink's latency.
PULSE_LATENCY_MSEC=${PULSE_LATENCY_MSEC:-60}
SteamGameId=$id
SteamAppId=$id
export PROTON_USE_WOW64 PROTON_USE_WINED3D WINE_D3D_CONFIG PROTON_NO_FSYNC
export PROTON_LOG PROTON_LOG_DIR PULSE_LATENCY_MSEC SteamGameId SteamAppId
export STEAM_COMPAT_DATA_PATH STEAM_COMPAT_CLIENT_INSTALL_PATH

if [ -n "$print" ]; then
    printf '%s\n' "PROTON_USE_WOW64=$PROTON_USE_WOW64" \
        "PROTON_USE_WINED3D=$PROTON_USE_WINED3D" "WINE_D3D_CONFIG=$WINE_D3D_CONFIG" \
        "PROTON_NO_FSYNC=$PROTON_NO_FSYNC" "SteamGameId=$SteamGameId" \
        "SteamAppId=$SteamAppId" "PROTON_LOG=$PROTON_LOG" "PROTON_LOG_DIR=$PROTON_LOG_DIR" \
        "PULSE_LATENCY_MSEC=$PULSE_LATENCY_MSEC" \
        "STEAM_COMPAT_DATA_PATH=$STEAM_COMPAT_DATA_PATH" \
        "STEAM_COMPAT_CLIENT_INSTALL_PATH=$STEAM_COMPAT_CLIENT_INSTALL_PATH"
    exit 0
fi

[ "$(uname -m)" = x86_64 ] || die 'proton needs the x86-64 GNU root'
[ -x /opt/proton/proton ] || die 'missing /opt/proton/proton'
mkdir -p "$STEAM_COMPAT_DATA_PATH" "$STEAM_COMPAT_CLIENT_INSTALL_PATH" "$PROTON_LOG_DIR"
# Boot a new prefix first, so that its first program does not wait for it.
# WineD3D takes the video memory from the GL driver, as under Linux.
if [ ! -f "$STEAM_COMPAT_DATA_PATH/pfx/user.reg" ]; then
    status=0
    /opt/proton/proton run 'c:\windows\system32\wineboot.exe' -u || status=$?
    [ "$status" -eq 0 ] || { echo "rach2-game: prefix: wineboot failed: status $status" >&2; exit 1; }
    echo "rach2-game: prefix: made $STEAM_COMPAT_DATA_PATH/pfx" >&2
fi
exec /opt/proton/proton run "$@"
