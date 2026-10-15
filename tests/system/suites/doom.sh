#!/bin/sh
# doom.sh: the Doom port on a disk root, where the machine has its free
# memory to give to the game's zone.
#
# One boot runs four rows. First the game is started with its WAD moved
# away, as on a machine built without one: the engine must refuse with
# `IWAD file '/demo/doom1.wad' not found!` and the game exit with status 255,
# and the WAD is put back. Then the game is started in the background, and
# after ten seconds it must still run, must have opened the framebuffer, and
# must have printed its zone line; the disk commands of those ten seconds are
# printed. The zone is MemFree less 8 MiB, up to the engine's default of 16
# MiB, so a 32 MiB machine reports 12 MiB (0xc00000), and the row requires at
# least 10 MiB. With that game killed, the first demo is played to its end,
# where the engine quits as a menu's Quit does and must exit with status 0.
# Last, a demo is recorded with a buffer of 1 KiB, which fills in seconds and
# is written through the port's fopen before the engine's error ends the
# game: the file must have mode 644, the 0666 that fopen passes less the
# shell's umask.
#
# Usage: doom.sh KERNEL DISK HARNESS PASSWORD
set -u
KERNEL=$1; DISK=$2; HARNESS=$3; PASSWORD=$4
fail() { echo "doom-test: FAIL: $1" >&2; exit 1; }
LOG="$DISK.doom.log"
# interval: doom runs for ten seconds; the row counts its disk commands
if "$HARNESS" --kernel "$KERNEL" --append "rach2.fsdisk rach2.diskdiag" \
    --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" \
    --timeout 420 --steptmo 300 --ok "DEMODONE" --log "$LOG" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=mv /demo/doom1.wad /demo/doom1.off && { doom > /tmp/nowad.txt 2>&1; echo DOOMNOWAD=\$?; mv /demo/doom1.off /demo/doom1.wad; }; echo DOOMNOWADMSG=\$(grep -c 'doom1.wad. not found' /tmp/nowad.txt); echo NOWAD\"\"DONE" \
    "expect=NOWADDONE" \
    "send=cd /root && A=\$(sed -n 's/^disk-commands //p' /proc/disk) && (doom > /tmp/doom.txt 2>&1 &) && sleep 10 && B=\$(sed -n 's/^disk-commands //p' /proc/disk) && echo RAN\"\"DOOM" \
    "expect=RANDOOM" "send=echo DOOMRUNNING=\$(ps | grep -c '[d]oom') && echo DOOMCOMMANDS=\$((B-A)) && grep -a 'zone memory' /tmp/doom.txt | sed 's/^/DOOM/'; echo REPORT\"\"ONE" \
    "expect=REPORTONE" "send=echo DOOMFB=\$(grep -ac 'framebuffer: x_res' /tmp/doom.txt); echo DOOMFBFAIL=\$(grep -ac 'Failed to open framebuffer' /tmp/doom.txt); echo DOOM\"\"DONE" \
    "expect=DOOMDONE" \
    "send=killall doom; while ps | grep -q '[d]oom'; do :; done; cd /tmp && doom -playdemo demo1 > /tmp/demo.txt 2>&1; echo DEMOEXIT=\$?; echo PLAYED\"\"DEMO" \
    "expect=PLAYEDDEMO" \
    "send=rm -f /tmp/rec.lmp; doom -record /tmp/rec -maxdemo 1 > /tmp/rec.txt 2>&1; echo RECMODE=\$(stat -c %a /tmp/rec.lmp 2>/dev/null || echo none); echo DEMO\"\"DONE" \
    "expect=DEMODONE" > /dev/null 2>&1; then
    :
else
    fail "the boot"
fi
NOWAD="$(grep -a '^DOOMNOWAD=' "$LOG" | tail -1 | tr -d '\r' | cut -d= -f2)"
NOWADMSG="$(grep -a '^DOOMNOWADMSG=' "$LOG" | tail -1 | tr -d '\r' | cut -d= -f2)"
[ "$NOWAD" = 255 ] && [ "${NOWADMSG:-0}" -ge 1 ] ||
    fail "without its WAD the game exited with status ${NOWAD:-unknown} and ${NOWADMSG:-no} not-found line(s); the engine's error and status 255 were required"
echo "doom-test: without its WAD the game ends with the engine's error and status 255: OK"
RUNNING="$(grep -a '^DOOMRUNNING=' "$LOG" | tail -1 | tr -d '\r' | cut -d= -f2)"
COMMANDS="$(grep -a '^DOOMCOMMANDS=' "$LOG" | tail -1 | tr -d '\r' | cut -d= -f2)"
ZONE="$(grep -a '^DOOMzone memory' "$LOG" | tail -1 | tr -d '\r' | sed -n 's/.*, \([0-9a-f]*\) allocated.*/\1/p')"
[ "${RUNNING:-0}" -ge 1 ] || fail "the game was not running after ten seconds"
FB="$(grep -a '^DOOMFB=' "$LOG" | tail -1 | tr -d '\r' | cut -d= -f2)"
FBFAIL="$(grep -a '^DOOMFBFAIL=' "$LOG" | tail -1 | tr -d '\r' | cut -d= -f2)"
[ "${FBFAIL:-1}" = 0 ] || fail "the game could not open the framebuffer from /root"
[ "${FB:-0}" -ge 1 ] || fail "the game reported no framebuffer geometry"
echo "doom-test: the game runs on the disk root and cost the disk ${COMMANDS:-?} commands in ten seconds: OK"
[ -n "$ZONE" ] || fail "the game printed no zone line"
if [ "$((0x$ZONE))" -ge "$((10 * 1024 * 1024))" ]; then
    echo "doom-test: the zone is 0x$ZONE bytes, sized from the machine's free memory: OK"
else
    fail "the zone is 0x$ZONE bytes, under the 10 MiB of a zone sized from the free memory"
fi
DEMOEXIT="$(grep -a '^DEMOEXIT=' "$LOG" | tail -1 | tr -d '\r' | cut -d= -f2)"
RECMODE="$(grep -a '^RECMODE=' "$LOG" | tail -1 | tr -d '\r' | cut -d= -f2)"
[ "$DEMOEXIT" = 0 ] || fail "the demo played to its end exited with status ${DEMOEXIT:-unknown}; 0 was required"
echo "doom-test: a demo played to its end quits with status 0: OK"
[ "$RECMODE" = 644 ] ||
    fail "the demo the game recorded through fopen has mode ${RECMODE:-unknown}; 644 was required"
echo "doom-test: a demo recorded until its buffer fills is written through fopen with mode 644: OK"
echo "doom-test: PASS"
