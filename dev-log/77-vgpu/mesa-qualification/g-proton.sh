#!/bin/sh
#
# g-proton.sh OUT: the Direct3D probes through GE-Proton11-12 outside Steam,
# Venus the only Vulkan device, under the session of g-sess.sh. Each row runs
# in a new process tree in the prefix ~/pfx and ends with wineserver -k; a row
# that has not ended in 300 s is killed and says so.
#
set -u
o=$1
. ~/sess.env
P=$HOME/ge/GE-Proton11-12-x86_64
mkdir -p "$o" ~/pfx ~/steam
export STEAM_COMPAT_DATA_PATH=$HOME/pfx STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/steam
export SteamGameId=77 PROTON_LOG=1 PROTON_LOG_DIR=$o
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json
export PROTON_USE_WOW64=1

# row NAME EXE [VAR=VALUE...]: run probe EXE under Proton with the variables.
row() {
    n=$1 exe=$2
    shift 2
    mkdir -p "$o/$n"
    rm -rf ~/.cache/mesa_shader_cache* ~/pfx/pfx/drive_c/users/*/AppData/Local/*/shader* 2>/dev/null
    t0=$(date +%s.%N)
    env "$@" DXVK_LOG_PATH="$o/$n" VKD3D_DEBUG=warn PROBE_OUT="Z:$o/$n/probe.out" \
        timeout -k 10 300 python3 "$P/proton" run "$HOME/probes/$exe" >"$o/$n/proton.txt" 2>&1
    st=$?
    t1=$(date +%s.%N)
    WINEPREFIX=$HOME/pfx/pfx "$P/files/bin/wineserver" -k 2>/dev/null
    [ -f "$o/steam-77.log" ] && mv "$o/steam-77.log" "$o/$n/proton.log"
    printf '%-28s exit %3d %6.1f s  %s\n' "$n" "$st" "$(echo "$t1 - $t0" | bc)" \
        "$(grep -h 'adapter\|draw\|level\|failed\|no DXGI' "$o/$n/probe.out" 2>/dev/null | tr '\n' '|')"
}

row d3d11-64-dxvk d3d11probe64.exe
row d3d12-64-vkd3dproton d3d12probe64.exe
row d3d11-32-dxvk d3d11probe32.exe
row d3d12-32-vkd3dproton d3d12probe32.exe
row d3d11-64-wined3d-vk d3d11probe64.exe PROTON_USE_WINED3D=1 WINE_D3D_CONFIG=renderer=vulkan
row d3d11-32-wined3d-vk d3d11probe32.exe PROTON_USE_WINED3D=1 WINE_D3D_CONFIG=renderer=vulkan
row d3d12-64-winevkd3d d3d12probe64.exe PROTON_USE_WINED3D=1
row d3d12-32-winevkd3d d3d12probe32.exe PROTON_USE_WINED3D=1
