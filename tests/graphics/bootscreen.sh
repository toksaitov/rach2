#!/bin/sh
# bootscreen.sh: whether a boot left the screen to the console's login, lit
# and with the keyboard, and no desktop session on it.
#
#   bootscreen.sh [-l]
#
# Run as root on the machine, or piped to `sh -s` over ssh. It counts, and
# requires:
#
#   weston     no process named weston
#   console    the kernel's log names a card the console holds, and its last
#              line of that card is not `handed to a master`, so the console
#              has the screen and the keyboard
#   session    with rach2.console=term on the boot line, init's console
#              session, the child of init that leads a session on
#              /dev/console, is `term -l`
#   login      and that term has started its login: a child of term's that
#              leads a session of its own and has left term's image, by its
#              exec of /bin/login, which term makes only once it has loaded
#              its font and opened the screen and the keyboard
#   backlight  every backlight of /sys/class/backlight has brightness and
#              actual_brightness above 0 and bl_power 0, on
#
# With -l, require only no Weston and the term login. The release uses it
# after the native cases, when the boot's card messages may have rotated out.
#
# A boot may answer before init has started its console session, so the
# checks are made again, once a second, until they pass or 30 tries have
# failed; the last try's lines are printed. The last line is
# `bootscreen: result: pass` or `fail: why`, and the status 0 on a pass.

login_only=no
case $#:$* in
    0:) ;;
    1:-l) login_only=yes ;;
    *) echo 'usage: bootscreen.sh [-l]' >&2; exit 2 ;;
esac

# console_session: print the pid, name and arguments of init's console
# session, the child of init that leads a session whose controlling terminal
# is /dev/console, device 5:1, or nothing.
console_session()
{
    for file in /proc/[0-9]*/stat; do
        IFS= read -r line 2> /dev/null < "$file" || continue
        pid=${line%% *}
        comm=${line%\)*}
        # Split the fields after comm: state, ppid, pgrp, sid, tty_nr.
        # shellcheck disable=SC2086 # /proc's fields are separate words
        set -- ${line##*\) }
        [ "$#" -ge 5 ] && [ "$1" != Z ] && [ "$2" = 1 ] &&
            [ "$4" = "$pid" ] && [ "$5" = $(((5 << 8) | 1)) ] || continue
        printf '%s %s %s\n' "$pid" "${comm#*\(}" \
            "$(tr '\0' ' ' < "/proc/$pid/cmdline" 2> /dev/null | sed 's/ $//')"
        return
    done
}

# login_child PID: print the pid and name of the child of PID that leads a
# session of its own and has left PID's image by an exec, so that its name
# is no longer term; a zombie is left out. Prints nothing when none has.
login_child()
{
    parent=$1
    for file in /proc/[0-9]*/stat; do
        IFS= read -r line 2> /dev/null < "$file" || continue
        pid=${line%% *}
        comm=${line%\)*}
        comm=${comm#*\(}
        # shellcheck disable=SC2086 # /proc's fields are separate words
        set -- ${line##*\) }
        [ "$#" -ge 4 ] && [ "$1" != Z ] && [ "$2" = "$parent" ] &&
            [ "$4" = "$pid" ] && [ "$comm" != term ] || continue
        printf '%s %s\n' "$pid" "$comm"
        return
    done
}

# westons: the number of processes named weston, a zombie left out.
westons()
{
    n=0
    for file in /proc/[0-9]*/stat; do
        IFS= read -r line 2> /dev/null < "$file" || continue
        case $line in
            *' (weston) Z '*) ;;
            *' (weston) '*) n=$((n + 1)) ;;
        esac
    done
    echo "$n"
}

# console_card: from the kernel's log, `cardN on` when the console holds
# cardN with the screen and keyboard, `cardN handed` when it has handed
# them to a master, or nothing when it holds no card.
console_card()
{
    dmesg | awk '
        match($0, /console: card[0-9]+: /) {
            n = substr($0, RSTART + 13, RLENGTH - 15)
            what = substr($0, RSTART + RLENGTH)
            if (what ~ /^(the firmware.s framebuffer|a native display)$/) {
                card = n
                state = "on"
            } else if (n == card && what == "left") {
                card = ""
            } else if (n == card && what ~ /^handed to a master/) {
                state = "handed"
            } else if (n == card && what ~ /^taken back from a master/) {
                state = "on"
            }
        }
        END { if (card != "") print "card" card " " state }'
}

# check: make every check once, print its lines, and set `why` to the first
# failure, or leave it empty.
check()
{
    why=''
    n=$(westons)
    echo "bootscreen: weston: $n running"
    [ "$n" = 0 ] || why="$n weston running"

    if [ "$login_only" = no ]; then
        held=$(console_card)
        echo "bootscreen: console: ${held:-no card}"
        case $held in
            *' on') ;;
            *' handed') [ -n "$why" ] || why="the console handed ${held% *} to a master" ;;
            *) [ -n "$why" ] || why="the console holds no card" ;;
        esac
    fi

    current=$(console_session)
    echo "bootscreen: session: ${current:-none}"
    if [ "$login_only" = yes ] || tr ' ' '\n' < /proc/cmdline | grep -qx 'rach2\.console=term'; then
        case $current in
            *' term term -l')
                login=$(login_child "${current%% *}")
                echo "bootscreen: login: ${login:-none}"
                [ -n "$login" ] || [ -n "$why" ] || why="term -l has started no login"
                ;;
            *) [ -n "$why" ] || why="init's console session is ${current:-none}, not term -l" ;;
        esac
    fi

    [ "$login_only" = no ] || return 0
    for light in /sys/class/backlight/*; do
        [ -d "$light" ] || continue
        name=${light##*/}
        level=$(cat "$light/brightness" 2> /dev/null)
        actual=$(cat "$light/actual_brightness" 2> /dev/null)
        power=$(cat "$light/bl_power" 2> /dev/null)
        most=$(cat "$light/max_brightness" 2> /dev/null)
        echo "bootscreen: backlight: $name: brightness ${level:-none} of ${most:-none}, actual ${actual:-none}, bl_power ${power:-none}"
        [ "${level:-0}" -gt 0 ] && [ "${actual:-0}" -gt 0 ] && [ "$power" = 0 ] ||
            [ -n "$why" ] || why="$name is dark: brightness ${level:-none}, actual ${actual:-none}, bl_power ${power:-none}"
    done
}

tries=0
while :; do
    tries=$((tries + 1))
    lines=$(check; echo "why $why")
    why=$(printf '%s\n' "$lines" | sed -n 's/^why //p')
    [ -n "$why" ] && [ "$tries" -lt 30 ] || break
    sleep 1
done
printf '%s\n' "$lines" | grep -v '^why '
if [ -n "$why" ]; then
    echo "bootscreen: result: fail: $why"
    exit 1
fi
echo "bootscreen: result: pass"
