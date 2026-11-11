#!/bin/sh
# Install and run GOG Crysis 2.0.0.7 on the persistent data partition.
set -eu

# Report an error before changing the install or starting the game.
die()
{
    echo "crysis: $*" >&2
    exit 1
}

# Print the commands, whose game files and state all live under /data.
usage()
{
    echo 'usage: crysis.sh install SETUP.exe | verify | run [--umu] [--32] [ARGUMENT...]'
}

action=${1:-}
case "$action" in
    install) [ "$#" -eq 2 ] || { usage >&2; exit 2; } ;;
    verify) [ "$#" -eq 1 ] || { usage >&2; exit 2; } ;;
    run) ;;
    --help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac
shift

awk '$2 == "/data" && $3 == "ext2" && $4 ~ /(^|,)rw(,|$)/ { ok = 1 }
     END { exit !ok }' /proc/mounts || die '/data must be a writable ext2 mount'
base=/data/crysis
assets=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
[ -r "$assets/game.sha256" ] || assets=/usr/share/rach2/crysis
[ -r "$assets/game.sha256" ] || die 'missing game.sha256'

if [ "$action" = install ]; then
    setup=$(CDPATH='' cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")
    [ "$(basename -- "$setup")" = 'setup_crysis_2.0.0.7_(84418).exe' ] ||
        die 'use the GOG 2.0.0.7 installer'
    echo 'crysis: checking the three installer files'
    (cd -- "$(dirname -- "$setup")" && sha256sum -c "$assets/crysis-installers.sha256")
    mkdir -p "$base"
    [ "$(CDPATH='' cd -- "$base" && pwd -P)" = "$base" ] || die 'the install directory is a symbolic link'
    [ ! -e "$base/game" ] && [ ! -L "$base/game" ] || die 'game already exists; use verify'
    work=
    locked=
    trap '[ -z "$work" ] || rm -rf -- "$work"; [ -z "$locked" ] || rmdir "$base/.install-lock"' EXIT
    trap 'exit 1' HUP INT TERM
    mkdir "$base/.install-lock" || die 'another installation holds .install-lock'
    locked=1
    work=$(mktemp -d "$base/.install.XXXXXX")
    echo 'crysis: unpacking the installer'
    innoextract --silent --exclude-temp --output-dir "$work/unpacked" "$setup"
    mkdir "$work/game"
    while IFS= read -r line; do
        file=${line#*  }
        file=${file#./}
        source=$work/unpacked/$file
        if [ "$file" = Game/Localized/Default.lng ]; then
            source=$work/unpacked/__support/app/$file
        fi
        mkdir -p -- "$work/game/$(dirname -- "$file")"
        mv -- "$source" "$work/game/$file"
    done < "$assets/game.sha256"
    echo 'crysis: checking the unpacked game'
    (cd "$work/game" && sha256sum --quiet -c "$assets/game.sha256")
    [ ! -e "$base/game" ] && [ ! -L "$base/game" ] || die 'game appeared during installation'
    mv -T -- "$work/game" "$base/game"
    sync
    echo 'crysis: result: installed, all 2358 files match'
    exit 0
fi

[ -d "$base/game" ] || die 'install the game first'
if [ "$action" = verify ]; then
    (cd "$base/game" && sha256sum --quiet -c "$assets/game.sha256")
    echo 'crysis: result: all 2358 files match'
    exit 0
fi

MESA_SHADER_CACHE_DIR=$base/cache/mesa
MESA_SHADER_CACHE_DISABLE=false
PROTON_LOG_DIR=$base/logs
STEAM_COMPAT_CLIENT_INSTALL_PATH=$base/steam
export MESA_SHADER_CACHE_DIR MESA_SHADER_CACHE_DISABLE PROTON_LOG_DIR
export STEAM_COMPAT_CLIENT_INSTALL_PATH
mkdir -p "$base/saves" "$MESA_SHADER_CACHE_DIR" "$PROTON_LOG_DIR"
route=
bin=Bin64
while [ "$#" -gt 0 ]; do
    case "$1" in
        --umu) route=--umu; shift ;;
        --32) bin=Bin32; shift ;;
        *) break ;;
    esac
done
if [ -n "$route" ]; then
    # umu maps drive S: to the directory above the install path, which is
    # the program's own by default; from S:\ the game finds no language.
    # The container shares /data, which holds the prefix and the saves.
    STEAM_COMPAT_INSTALL_PATH=$base/game
    PRESSURE_VESSEL_FILESYSTEMS_RW=/data${PRESSURE_VESSEL_FILESYSTEMS_RW:+:$PRESSURE_VESSEL_FILESYSTEMS_RW}
    export STEAM_COMPAT_INSTALL_PATH PRESSURE_VESSEL_FILESYSTEMS_RW
fi
cd "$base/game/$bin"
exec rach2-game ${route:+"$route"} --id 17300 --prefix "$base/compat" "$PWD/Crysis.exe" \
    -dx9 -userpath 'Z:\data\crysis\saves' "$@"
