#!/bin/sh
# common.sh: what the runners share, sourced by webgl.sh, video.sh and
# deqp.sh inside the guest, Rach2's or Debian's, and run on the host as
# `sh common.sh host`, or as the cases' artifact `common host`, for the
# host's half of a run's record.
# shellcheck shell=sh
#
# A run writes its record to $RUNNER_OUT/record, one `NAME VALUE` a line, as
# tests/graphics/Readme.md's Runners section lists them: the system and its
# renderer, the IOMMU's state, the caches' preparation, the case list, the
# work beside the run, the browser's switches, the window's fixed size and
# the warm-up and repetitions. The record is what makes two runs comparable:
# browser fill numbers in QEMU varied by about 50% between runs on the first
# night of the discovery run, and only a record says which of these moved.
#
# The browser draws into a headless Weston of its own at 1280x800, the
# display profiles' fixed window, under kiosk-shell, so that each page is
# fullscreen; Chromium and Firefox are both Wayland clients there.

RUNNER_WIDTH=1280
RUNNER_HEIGHT=800

runner_die()
{
    echo "runner: $RUNNER_NAME: $*" >&2
    exit 1
}

# runner_record NAME VALUE...: append a line to the record.
runner_record()
{
    echo "$*" >> "$RUNNER_OUT/record"
}

# runner_busy: the CPUs' busy and total counts of /proc/stat's first line,
# as `BUSY TOTAL`, in the kernel's ticks.
runner_busy()
{
    awk '$1 == "cpu" { t = 0; for (i = 2; i <= NF; i++) t += $i; print t - $5 - $6, t; exit }' /proc/stat
}

# runner_mesa: the version of Debian's Mesa, from dpkg in a Debian guest and
# from the package's changelog on Rach2's root, which has no dpkg database.
runner_mesa()
{
    mesa_v=$(dpkg-query -W -f '${Version}' libgl1-mesa-dri 2>/dev/null) ||
        mesa_v=$(gzip -dc /usr/share/doc/libgl1-mesa-dri/changelog.Debian.gz 2>/dev/null |
            sed -n '1s/^mesa (\([^)]*\)).*/\1/p') || mesa_v=''
    echo "${mesa_v:-unknown}"
}

# runner_start [plain]: begin the record with what the guest is: its system,
# CPUs, memory, Mesa, the IOMMU's state, the caches' preparation and the
# other processes that run beside the runner. `plain`, for a runner that
# starts no browser, leaves out the browser's profile and the window's size.
runner_start()
{
    mkdir -p "$RUNNER_OUT"
    : > "$RUNNER_OUT/record"
    runner_record "runner $RUNNER_NAME"
    if [ -r /etc/os-release ]; then
        runner_record "system $(. /etc/os-release && echo "$PRETTY_NAME")"
    else
        runner_record "system $(uname -s)"
    fi
    runner_record "kernel $(uname -sr)"
    runner_record "cpus $(nproc 2>/dev/null || grep -c '^processor' /proc/cpuinfo)"
    runner_record "memory_kib $(awk '$1 == "MemTotal:" { print $2 }' /proc/meminfo)"
    runner_record "mesa $(runner_mesa)"
    runner_iommu
    runner_record "cache mesa $(find "${MESA_SHADER_CACHE_DIR:-/nonexistent}" -type f 2>/dev/null | wc -l) files in ${MESA_SHADER_CACHE_DIR:-none}, emptied at start: ${GRAPHICS_DONE:-0}"
    if [ "${1:-}" != plain ]; then
        runner_record "cache browser a new profile for each browser started"
        runner_record "size ${RUNNER_WIDTH}x$RUNNER_HEIGHT"
    fi
    runner_record "load start $(cut -d' ' -f1-3 /proc/loadavg)"
    runner_record "processes start $(ps -e -o pid= 2>/dev/null | wc -l)"
    RUNNER_BUSY0=$(runner_busy)
    sed 's/^/kernel_before /' /proc/stat >> "$RUNNER_OUT/record"
    if [ -f /tmp/graphics.env ]; then
        cat /tmp/graphics.env >> "$RUNNER_OUT/record"
    fi
}

# runner_iommu: the IOMMU's state as the guest's kernel gives it: the units
# under /sys/class/iommu, the groups under /sys/kernel/iommu_groups and the
# kernel's own words for it on its command line.
runner_iommu()
{
    units=none groups=0
    if [ -d /sys/class/iommu ]; then
        units=$(find /sys/class/iommu -mindepth 1 -maxdepth 1 -printf '%f ')
    fi
    if [ -d /sys/kernel/iommu_groups ]; then
        groups=$(find /sys/kernel/iommu_groups -mindepth 1 -maxdepth 1 | wc -l)
    fi
    words=$(tr ' ' '\n' < /proc/cmdline | grep -i iommu | tr '\n' ' ')
    runner_record "iommu units ${units:-none} groups $groups cmdline ${words:-none}"
}

# runner_finish: end the record with the work the guest did beside the run:
# the load average and the share of the CPUs busy from start to end.
runner_finish()
{
    # shellcheck disable=SC2046,SC2086 # four numbers
    set -- $RUNNER_BUSY0 $(runner_busy)
    runner_record "load end $(cut -d' ' -f1-3 /proc/loadavg)"
    runner_record "busy $(awk -v b="$(($3 - $1))" -v t="$(($4 - $2))" 'BEGIN { printf "%.1f%%", t ? 100 * b / t : 0 }')"
    sed 's/^/kernel_after /' /proc/stat >> "$RUNNER_OUT/record"
}

# runner_weston: start a headless Weston at the fixed size under kiosk-shell,
# with its own runtime directory, and export WAYLAND_DISPLAY for the browser.
# Weston's renderer is Pixman, which offers clients shared memory alone: with
# its GL renderer Weston offers linux-dmabuf on the display's card0, where
# Chromium's GPU process, as an ordinary user that is not the DRM master,
# cannot make a dumb buffer ("DRM_IOCTL_MODE_CREATE_DUMB failed: Permission
# denied") and loses every context. The browser's own GL, WebGL's, runs on
# the guest's Mesa either way.
runner_weston()
{
    XDG_RUNTIME_DIR=$RUNNER_WORK/run
    WAYLAND_DISPLAY=wayland-runner
    export XDG_RUNTIME_DIR WAYLAND_DISPLAY
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
    setsid weston ${RUNNER_SEAT_MODULE:+--modules="$RUNNER_SEAT_MODULE"} --backend=headless --renderer=pixman --width="$RUNNER_WIDTH" --height="$RUNNER_HEIGHT" \
        --shell=kiosk --idle-time=0 --socket="$WAYLAND_DISPLAY" \
        --log="$RUNNER_OUT/weston.log" < /dev/null > /dev/null 2>&1 &
    RUNNER_WESTON=$!
    tries=0
    until [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; do
        kill -0 "$RUNNER_WESTON" 2>/dev/null || runner_die "weston ended: $(tail -n 3 "$RUNNER_OUT/weston.log")"
        tries=$((tries + 1))
        [ "$tries" -lt 600 ] || runner_die "weston made no socket in 600 tries"
        sleep 0.1
    done
    runner_record "compositor $(weston --version 2>/dev/null) headless pixman kiosk"
}

# runner_server ROOT...: serve ROOT on a free port of the loopback address
# with server.py, which writes each page fetched and each result posted to
# $RUNNER_WORK/events, and set RUNNER_PORT.
runner_server()
{
    : > "$RUNNER_WORK/events"
    python3 -I "$RUNNER_DIR/server.py" "$RUNNER_WORK/events" "$RUNNER_WORK/port" "$@" \
        > "$RUNNER_OUT/server.log" 2>&1 &
    RUNNER_SERVER=$!
    tries=0
    until [ -s "$RUNNER_WORK/port" ]; do
        kill -0 "$RUNNER_SERVER" 2>/dev/null || runner_die "the server ended: $(tail -n 3 "$RUNNER_OUT/server.log")"
        tries=$((tries + 1))
        [ "$tries" -lt 600 ] || runner_die "the server took no port in 600 tries"
        sleep 0.1
    done
    # shellcheck disable=SC2034 # webgl.sh and video.sh read it
    RUNNER_PORT=$(cat "$RUNNER_WORK/port")
}

# runner_switches BROWSER: the browser's fixed words, which the record lists.
# Chromium's are those Chromium's own WebGL conformance runner uses or that
# keep a test page from being throttled, with --ignore-gpu-blocklist, which
# llvmpipe needs for WebGL; Firefox's are the prefs of its profile.
runner_switches()
{
    case $1 in
        chromium)
            echo "--ozone-platform=wayland --no-first-run --no-default-browser-check" \
                "--password-store=basic --start-fullscreen --ignore-gpu-blocklist" \
                "--disable-gpu-process-crash-limit --js-flags=--expose-gc" \
                "--disable-background-timer-throttling --disable-renderer-backgrounding" \
                "--disable-background-networking --disable-hang-monitor" \
                "--autoplay-policy=no-user-gesture-required"
            ;;
        firefox)
            echo "--new-instance --no-remote --kiosk"
            ;;
    esac
}

# runner_prefs: Firefox's profile, user.js: no first-run pages, no slow
# script dialog, which would stop a page, no session restored after a
# crash, WebGL on whatever renderer the guest has, and autoplay allowed.
runner_prefs()
{
    cat << 'EOF'
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.sessionstore.resume_from_crash", false);
user_pref("toolkit.startup.max_resumed_crashes", -1);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("datareporting.healthreport.uploadEnabled", false);
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("app.update.auto", false);
user_pref("dom.max_script_run_time", 0);
user_pref("dom.max_chrome_script_run_time", 0);
user_pref("webgl.force-enabled", true);
user_pref("media.autoplay.default", 0);
EOF
}

# runner_browser BROWSER URL LOG: start BROWSER on URL in a new profile, in
# a session of its own so that runner_stop ends all of its processes, and
# set RUNNER_BROWSER to its process. Chromium runs under cdp.py when
# RUNNER_CDP names a file, which then gets the Media domain's events.
runner_browser()
{
    profile=$RUNNER_WORK/profile
    rm -rf "$profile"
    mkdir -p "$profile"
    case $1 in
        chromium)
            if [ -n "${RUNNER_CDP:-}" ]; then
                # shellcheck disable=SC2046 # the switches are words
                setsid python3 -I "$RUNNER_DIR/cdp.py" "$RUNNER_CDP" "$2" \
                    chromium $(runner_switches chromium) --user-data-dir="$profile" \
                    < /dev/null >> "$3" 2>&1 &
            else
                # shellcheck disable=SC2046 # the switches are words
                setsid chromium $(runner_switches chromium) --user-data-dir="$profile" "$2" \
                    < /dev/null >> "$3" 2>&1 &
            fi
            ;;
        firefox)
            runner_prefs > "$profile/user.js"
            # shellcheck disable=SC2046 # the switches are words
            setsid env MOZ_ENABLE_WAYLAND=1 ${RUNNER_MOZ_LOG:+MOZ_LOG=$RUNNER_MOZ_LOG} \
                firefox-esr $(runner_switches firefox) --profile "$profile" "$2" \
                < /dev/null >> "$3" 2>&1 &
            ;;
        *)
            runner_die "no browser $1; chromium or firefox"
            ;;
    esac
    RUNNER_BROWSER=$!
}

# runner_browser_record BROWSER: the browser's version and switches.
runner_browser_record()
{
    case $1 in
        chromium)
            runner_record "browser $(chromium --version 2>/dev/null | head -n 1)"
            runner_record "switches $(runner_switches chromium)"
            ;;
        firefox)
            runner_record "browser $(firefox-esr --version 2>/dev/null | head -n 1)"
            runner_record "switches $(runner_switches firefox) MOZ_ENABLE_WAYLAND=1"
            runner_prefs | sed 's/^/prefs /' >> "$RUNNER_OUT/record"
            ;;
    esac
}

# runner_stop: end the browser's session, if it runs, and wait for it.
runner_stop()
{
    [ -n "${RUNNER_BROWSER:-}" ] || return 0
    kill -TERM "-$RUNNER_BROWSER" 2>/dev/null || true
    tries=0
    while kill -0 "$RUNNER_BROWSER" 2>/dev/null && [ "$tries" -lt 50 ]; do
        sleep 0.1
        tries=$((tries + 1))
    done
    kill -KILL "-$RUNNER_BROWSER" 2>/dev/null || true
    wait "$RUNNER_BROWSER" 2>/dev/null || true
    RUNNER_BROWSER=
}

# runner_cleanup: end the browser, Weston and the server, and remove the
# work directory; installed as the runner's exit trap.
runner_cleanup()
{
    runner_stop
    for pid in $RUNNER_SERVER $RUNNER_WESTON; do
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    [ -z "${RUNNER_WORK:-}" ] || rm -rf "$RUNNER_WORK"
}

# runner_init NAME OUT: set the runner's name, output and work directories,
# and install the exit trap.
runner_init()
{
    RUNNER_NAME=$1
    RUNNER_OUT=$2
    RUNNER_BROWSER='' RUNNER_SERVER='' RUNNER_WESTON=''
    mkdir -p "$RUNNER_OUT"
    RUNNER_WORK=$(mktemp -d "${TMPDIR:-/tmp}/$RUNNER_NAME.XXXXXX")
    trap runner_cleanup EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
}

# runner_host: the host's half of a run's record, which the side that boots
# the guest writes beside the guest's: the host's load and the claims of
# etude's admission ledger, the other runs on the testing host.
runner_host()
{
    site=${ETUDE_SITE:-/etc/rach2/etude-site}
    ledger=/var/lib/rach2-etude/ledger
    if [ -r "$site" ]; then
        ledger=$(sed -n 's/^ledger //p' "$site")
    fi
    echo "host $(uname -n) $(uname -sr)"
    echo "host_load $(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null || echo unknown)"
    if [ -r "$ledger/claims" ]; then
        echo "host_claims $(awk '$1 == "claim" { n++ } END { print n+0 }' "$ledger/claims")"
        sed 's/^/host_ledger /' "$ledger/claims"
    else
        echo "host_claims none: no ledger at $ledger"
    fi
}

# Run as the cases' artifact, the script is named `common`.
case "${0##*/}:${1:-}" in
common.sh:host | common:host) runner_host ;;
esac
