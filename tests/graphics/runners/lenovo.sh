#!/bin/sh
# lenovo.sh: one of the Lenovo's GPU conformance cases (tests/native/gpu.cases):
# a runner on the machine's own GPU and display, judged against Linux's run
# on the same machine.
#
#   lenovo.sh igt LIST PART REF
#   lenovo.sh piglit PART REF
#   lenovo.sh deqp SUITE START/EVERY REF
#   lenovo.sh glmark2 gl|es2 REF
#   lenovo.sh webgl BROWSER CHUNK REF
#   lenovo.sh gate glmark2 gl|es2 GATE
#   lenovo.sh gate probe BROWSER GATE
#
# Run as root from a directory that holds the runners and the files the case
# names, as etude-guest's workspace holds them; it writes the runner's output
# under ./out. REF and GATE are files there, Linux's results for the case and
# the speed gate's settings, or `-`, which makes the run a reference and
# judges nothing; `exclusions`, when there, is the owner's list of approved
# differences, which judge.py takes. The cases:
#
#   igt      igt.sh, as root, on the lines of LIST from its `# part PART` to
#            the next part, or on the test program PART of LIST, with the
#            session's compositor stopped, since IGT's display tests take
#            the card's master as Linux's run did with none; judged by
#            name over the part's subtests
#   piglit   piglit.sh's PART on the GPU through GBM, as the session's user
#   deqp     deqp.sh's SUITE, every EVERYth case from the STARTth, as root,
#            as step 9 ran it
#   glmark2  glmark2.sh's validation of each scene, fullscreen on the
#            session's compositor, as the session's user
#   webgl    webgl.sh's CHUNK of the WebGL 2.0.1 list in BROWSER, on the
#            session's compositor, as the session's user, judged against
#            the same pages of REF, all of the browser's chunks
#   gate     glmark2.sh's benchmark of API, or probe.sh in BROWSER, on the
#            session's compositor, as the session's user, judged by
#            `judge.py gate`
#
# The session is Rach2's desktop: its user `desktop`, runtime directory
# /run/user/1001 and display wayland-0, which /run/rach2-desktop.pid's
# supervisor starts again on SIGHUP. LENOVO_USER, LENOVO_RUNTIME and
# LENOVO_DISPLAY name another, such as Debian's on the same machine;
# LENOVO_IGT, the IGT stage's directory, LENOVO_CTS, VK-GL-CTS's, and
# LENOVO_WEBGL, the WebGL suite's sdk/tests, other copies of those. A run's
# environment is guest-env.sh's, with Mesa's shader cache emptied first. It
# prints the runner's lines and the judge's, and exits 0 when both pass.
set -eu

here=$(pwd)
user=${LENOVO_USER:-desktop}
runtime=${LENOVO_RUNTIME:-/run/user/1001}
display=${LENOVO_DISPLAY:-wayland-0}
pidfile=/run/rach2-desktop.pid
out=$here/out
work='' stopped=0

usage()
{
    sed -n '7,13p' "$0" | sed 's/^# *//' >&2
    exit 2
}

die()
{
    echo "lenovo: $*" >&2
    exit 1
}

# shellcheck disable=SC2329 # the exit trap runs it
cleanup()
{
    [ -z "$work" ] || rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# session_stop: end the desktop's compositor, so that a test may take the
# card's master, and wait until it has gone; session_start brings it back.
session_stop()
{
    pgrep -x weston > /dev/null || return 0
    stopped=1
    [ -f "$pidfile" ] || die "a compositor not the desktop's holds the display: $(pgrep -l -x weston | tr '\n' ' ')"
    pkill -TERM -x weston || true
    tries=0
    while pgrep -x weston > /dev/null; do
        tries=$((tries + 1))
        [ "$tries" -lt 600 ] || die "weston did not end in 600 tries"
        sleep 0.1
    done
    echo "lenovo: session: stopped"
}

# session_start: have the desktop's supervisor start the session when its
# compositor is not up, and wait for its socket.
session_start()
{
    socket=$runtime/$display
    if [ ! -S "$socket" ] || ! pgrep -x weston > /dev/null; then
        [ -f "$pidfile" ] || die "no session compositor at $socket and no desktop supervisor"
        kill -HUP "$(cat "$pidfile")"
        tries=0
        until [ -S "$socket" ] && pgrep -x weston > /dev/null; do
            tries=$((tries + 1))
            [ "$tries" -lt 600 ] || die "the session made no socket $socket in 600 tries"
            sleep 0.1
        done
        echo "lenovo: session: started"
    fi
}

# prepare: a work directory with a copy of the runners, which an ordinary
# user can read and write, and the empty output directory.
prepare()
{
    rm -rf "$out"
    mkdir -p "$out"
    work=$(mktemp -d "${TMPDIR:-/tmp}/lenovo.XXXXXX")
    for f in "$here"/*.sh "$here"/*.py "$here"/*.html; do
        [ -f "$f" ] && cp "$f" "$work/"
    done
    mkdir "$work/out" "$work/tmp"
    chmod 755 "$work"
}

# run AS CLASS RUNNER [ARGUMENT...]: run a runner of the work directory as
# root or as the session's user (AS `root` or `user`), in guest-env.sh's
# CLASS, with its output in the work directory's out.
run()
{
    as=$1 class=$2
    shift 2
    {
        echo "set -e"
        echo "cd '$work'"
        echo "TMPDIR='$work/tmp' GRAPHICS_CLASS=$class GRAPHICS_DONE=1"
        echo "MESA_SHADER_CACHE_DIR='$work/tmp/shader-cache'"
        echo "export TMPDIR GRAPHICS_CLASS GRAPHICS_DONE MESA_SHADER_CACHE_DIR"
        if [ "$as" = user ]; then
            echo "HOME='$work' XDG_RUNTIME_DIR='$runtime' WAYLAND_DISPLAY='$display' RUNNER_SESSION=1"
            echo "export HOME XDG_RUNTIME_DIR WAYLAND_DISPLAY RUNNER_SESSION"
        fi
        echo ". ./guest-env.sh"
        echo "graphics_env > '$work/tmp/graphics.env'"
        printf 'sh'
        for a; do
            printf " '%s'" "$a"
        done
        echo
    } > "$work/run.sh"
    status=0
    if [ "$as" = user ]; then
        chown -R "$user" "$work"
        su -s /bin/sh "$user" -c "sh '$work/run.sh'" || status=$?
    else
        sh "$work/run.sh" || status=$?
    fi
    cp -R "$work/out/." "$out/"
    return "$status"
}

# judge KIND ARGUMENT...: judge.py's KIND with the exclusions when there are
# any, and nothing for a reference run.
judge()
{
    kind=$1
    shift
    set -- "$kind" "$@"
    if [ "$kind" != gate ] && [ -f "$here/exclusions" ]; then
        set -- "$@" -x "$here/exclusions"
    fi
    python3 -I "$here/judge.py" "$@"
}

# mixer_low: the volume at the step's level before a browser may play.
mixer_low()
{
    if command -v mixer > /dev/null; then
        mixer vol 1 > /dev/null || true
    fi
}

[ $# -ge 1 ] || usage
[ "$(id -u)" = 0 ] || die "run as root"
kind=$1
shift
case $kind:$# in
    igt:3 | piglit:2 | deqp:3 | glmark2:2 | webgl:3 | gate:3) ;;
    *) usage ;;
esac
ref=$(eval echo "\${$#}")
[ "$ref" = - ] || [ -f "$here/$ref" ] || die "no file $ref"
prepare
status=0
case $kind in
    igt)
        list=$1 part=$2
        [ -f "$here/$list" ] || die "no list $list"
        awk -v p="# part $part" '$0 == p { on = 1; next } /^# part / { on = 0 } on' \
            "$here/$list" > "$work/part.list"
        session_stop
        if [ -s "$work/part.list" ]; then
            run root conformance igt.sh -l "$work/part.list" ${LENOVO_IGT:+-t "$LENOVO_IGT"} \
                -o "$work/out" || status=$?
        else
            run root conformance igt.sh -l "$here/$list" ${LENOVO_IGT:+-t "$LENOVO_IGT"} \
                -o "$work/out" "$part" || status=$?
        fi
        [ "$stopped" = 0 ] || session_start
        if [ "$ref" != - ]; then
            sed 's/^/=/' "$out/list" > "$work/scope"
            cat "$out"/*.results > "$work/got" 2> /dev/null || : > "$work/got"
            judge results "$here/$ref" "$work/got" -s "$work/scope" || status=1
        fi
        ;;
    piglit)
        part=$1
        run user conformance piglit.sh -p gbm -o "$work/out" "$part" || status=$?
        if [ "$ref" != - ]; then
            judge results "$here/$ref" "$out/$part.results" -s "$out/$part.scope" || status=1
        fi
        ;;
    deqp)
        suite=$1 start=${2%/*} every=${2#*/}
        run root conformance deqp.sh -e "$every" -s "$start" ${LENOVO_CTS:+-c "$LENOVO_CTS"} \
            -o "$work/out" "$suite" || status=$?
        if [ "$ref" != - ]; then
            if [ -s "$out/flakes" ]; then
                judge deqp "$here/$ref" "$out/results.csv.gz" "$out/flakes" || status=1
            else
                judge deqp "$here/$ref" "$out/results.csv.gz" || status=1
            fi
        fi
        ;;
    glmark2)
        session_start
        run user performance glmark2.sh -a "$1" -m validate -o "$work/out" || status=$?
        [ "$ref" = - ] || judge glmark2 "$here/$ref" "$out/validations" || status=1
        ;;
    webgl)
        browser=$1 chunk=$2
        session_start
        mixer_low
        run user conformance webgl.sh -b "$browser" ${LENOVO_WEBGL:+-s "$LENOVO_WEBGL"} \
            -o "$work/out" "$chunk" || status=$?
        mixer_low
        if [ "$ref" != - ]; then
            pages=$out/$browser-2.0.1-$(printf '%02d' "$chunk").pages
            awk -v lo=$((chunk * 250)) -v hi=$((chunk * 250 + 249)) '$1 >= lo && $1 <= hi' \
                "$here/$ref" > "$work/ref.pages"
            [ -s "$work/ref.pages" ] || die "$ref has no page of chunk $chunk"
            judge webgl "$work/ref.pages" "$pages" || status=1
        fi
        ;;
    gate)
        what=$1 which=$2
        session_start
        case $what in
            glmark2)
                run user performance glmark2.sh -a "$which" -m benchmark -o "$work/out" || status=$?
                sed -n "s/^score \\([0-9][0-9]*\\)\$/glmark2-$which \\1/p" "$out/scores" > "$out/measures" ||
                    : > "$out/measures"
                ;;
            probe)
                mixer_low
                run user performance probe.sh -b "$which" -o "$work/out" || status=$?
                mixer_low
                ;;
            *) usage ;;
        esac
        [ "$ref" = - ] || judge gate "$here/$ref" "$out/measures" || status=1
        ;;
esac
exit "$status"
