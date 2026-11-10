#!/bin/sh
# Run a Windows game with the GNU root's Proton and WineD3D on OpenGL,
# directly or through umu-launcher in the Steam Linux Runtime.
set -eu

# Print the command line; a bad argument exits with status 2.
usage()
{
    echo 'usage: rach2-game [--umu] [--id ID] [--prefix DIR] [--print] [--] PROGRAM [ARGUMENT...]'
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
umu=
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
        --umu) umu=1; shift ;;
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

# umu-launcher takes the compatibility directory as WINEPREFIX, which it
# links as its own pfx, and derives SteamGameId and SteamAppId from GAMEID.
# Its runtime is UMU/steamrt3, which umu would download and update; the
# launcher unpacks the root's copy there instead and turns updates off.
umu_dir=${XDG_DATA_HOME:-$HOME/.local/share}/umu
if [ -n "$umu" ]; then
    PROTONPATH=/opt/proton
    GAMEID=umu-$id
    WINEPREFIX=$STEAM_COMPAT_DATA_PATH
    UMU_RUNTIME_UPDATE=0
    export PROTONPATH GAMEID WINEPREFIX UMU_RUNTIME_UPDATE
fi

if [ -n "$print" ]; then
    printf '%s\n' "PROTON_USE_WOW64=$PROTON_USE_WOW64" \
        "PROTON_USE_WINED3D=$PROTON_USE_WINED3D" "WINE_D3D_CONFIG=$WINE_D3D_CONFIG" \
        "PROTON_NO_FSYNC=$PROTON_NO_FSYNC" "SteamGameId=$SteamGameId" \
        "SteamAppId=$SteamAppId" "PROTON_LOG=$PROTON_LOG" "PROTON_LOG_DIR=$PROTON_LOG_DIR" \
        "PULSE_LATENCY_MSEC=$PULSE_LATENCY_MSEC" \
        "STEAM_COMPAT_DATA_PATH=$STEAM_COMPAT_DATA_PATH" \
        "STEAM_COMPAT_CLIENT_INSTALL_PATH=$STEAM_COMPAT_CLIENT_INSTALL_PATH"
    [ -z "$umu" ] || printf '%s\n' "PROTONPATH=$PROTONPATH" "GAMEID=$GAMEID" \
        "WINEPREFIX=$WINEPREFIX" "UMU_RUNTIME_UPDATE=$UMU_RUNTIME_UPDATE" \
        "UMU_RUNTIME=$umu_dir/steamrt3"
    exit 0
fi

# Run Proton's run verb with ARGUMENTs, directly or through umu.
proton_run()
{
    if [ -n "$umu" ]; then
        umu-run "$@"
    else
        /opt/proton/proton run "$@"
    fi
}

# Unpack the root's Steam Linux Runtime into umu's directory unless the copy
# there is of the same build. umu then checks it against its mtree with
# pv-verify and writes its .installed.ok, as after a download. The lock is
# umu's own, so that two launches unpack it once.
unpack_runtime()
{
    rt=$umu_dir/steamrt3
    [ -f /opt/umu/SteamLinuxRuntime_sniper.tar.xz ] && [ -f /opt/umu/VERSIONS.txt ] ||
        die 'missing /opt/umu/SteamLinuxRuntime_sniper.tar.xz'
    mkdir -p "$umu_dir"
    exec 9> "$umu_dir/umu.lock"
    flock 9
    if ! cmp -s /opt/umu/VERSIONS.txt "$rt/VERSIONS.txt"; then
        tmp=$(mktemp -d "$umu_dir/.rach2-game.XXXXXX")
        status=0
        tar -xJf /opt/umu/SteamLinuxRuntime_sniper.tar.xz -C "$tmp" || status=$?
        if [ "$status" -eq 0 ]; then
            [ ! -e "$rt" ] || mv "$rt" "$tmp/old"
            mv "$tmp/SteamLinuxRuntime_sniper" "$rt"
        fi
        rm -rf "$tmp"
        [ "$status" -eq 0 ] || { echo "rach2-game: umu: tar failed: status $status" >&2; exit 1; }
        echo "rach2-game: umu: unpacked the Steam Linux Runtime" \
            "$(awk '$1 == "sniper" { print $2 }' "$rt/VERSIONS.txt") in $rt" >&2
    fi
    exec 9>&-
}

[ "$(uname -m)" = x86_64 ] || die 'proton needs the x86-64 GNU root'
[ -x /opt/proton/proton ] || die 'missing /opt/proton/proton'
if [ -n "$umu" ]; then
    command -v umu-run > /dev/null || die 'missing umu-run'
    unpack_runtime
fi
mkdir -p "$STEAM_COMPAT_DATA_PATH" "$STEAM_COMPAT_CLIENT_INSTALL_PATH" "$PROTON_LOG_DIR"
# Boot a new prefix first, so that its first program does not wait for it.
# WineD3D takes the video memory from the GL driver, as under Linux.
if [ ! -f "$STEAM_COMPAT_DATA_PATH/pfx/user.reg" ]; then
    status=0
    proton_run 'c:\windows\system32\wineboot.exe' -u || status=$?
    [ "$status" -eq 0 ] || { echo "rach2-game: prefix: wineboot failed: status $status" >&2; exit 1; }
    echo "rach2-game: prefix: made $STEAM_COMPAT_DATA_PATH/pfx" >&2
fi
[ -n "$umu" ] || exec /opt/proton/proton run "$@"
exec umu-run "$@"
