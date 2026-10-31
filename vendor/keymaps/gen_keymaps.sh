#!/bin/sh

#
# gen_keymaps.sh: make the keymaps and their add-ons inside the pinned Ubuntu
# container. regen_keymaps.sh runs it; it is not run by hand.
#
# Usage: sh gen_keymaps.sh OUT SNAPURL
#
# Installs console-setup's ckbcomp, xkeyboard-config's data, kbd's loadkeys
# and libxkbcommon's xkbcli from the snapshot SNAPURL, writes each map and
# add-on into OUT, checks every one with `loadkeys -u -m` and
# check_keymap.awk, and writes OUT/manifest.head with the inputs' versions.
#

set -eu

OUT="$1"
SNAPURL="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
CHECK="$HERE/check_keymap.awk"
PACKAGES="keyboard-configuration xkb-data kbd libxkbcommon-tools x11proto-dev"
export DEBIAN_FRONTEND=noninteractive

# Each map as NAME LAYOUTS VARIANTS, with - for no variant. The CIS layouts
# come with US as the first layout and the XKB layout as the second, so that
# a Latin command line is one switch away; the Western layouts stand alone.
LAYOUTS='kg us,kg -
kz us,kz -
ru us,ru -
ua us,ua -
by us,by -
uz us,uz -
uz-latin us,uz ,latin
tj us,tj -
ru-tt us,ru ,tt
ru-bak us,ru ,bak
az us,az -
tm us,tm -
md us,md -
am us,am -
ge us,ge -
us us -
gb gb -
de de -
fr fr -
us-intl us intl
us-dvorak us dvorak
us-colemak us colemak'

ADDONS="alt-shift ctrl-shift caps-toggle ralt-toggle super-space caps-ctrl caps-escape"

dpkg-query -W -f '${Package} ${Version}\n' > /tmp/pkgs.before
# The snapshot service serves only TLS and the base image has no CA roots, so
# ca-certificates comes from the container's default archive first. It
# carries trust anchors, not bytes that reach a map; everything installed
# after it comes from the one dated snapshot.
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq ca-certificates >/dev/null 2>&1
sed -i "s|http://archive.ubuntu.com/ubuntu/|$SNAPURL|; s|http://security.ubuntu.com/ubuntu/|$SNAPURL|" \
    /etc/apt/sources.list.d/ubuntu.sources
rm -rf /var/lib/apt/lists/*
apt-get update -qq -o Acquire::Check-Valid-Until=false >/dev/null
apt-get install -y -qq --no-install-recommends $PACKAGES >/dev/null 2>&1
dpkg-query -W -f '${Package} ${Version}\n' > /tmp/pkgs.after

XKBVER="$(dpkg-query -W -f '${Version}' xkb-data)"
CKBVER="$(dpkg-query -W -f '${Version}' keyboard-configuration)"
KEYSYMDEF=/usr/include/X11/keysymdef.h
mkdir -p "$OUT"
cp "$HERE/xkb/xterm-strings.inc" "$OUT/"
WORK=/tmp/keymaps
rm -rf "$WORK"
mkdir -p "$WORK"

# The kbd modifier words for table T: Shift, AltGr, Control, Alt and ShiftL
# are bits 0 to 4, CtrlL bit 6.
mods() {
    t=$1
    w=""
    [ $((t & 1)) -ne 0 ] && w="$w shift"
    [ $((t & 2)) -ne 0 ] && w="$w altgr"
    [ $((t & 4)) -ne 0 ] && w="$w control"
    [ $((t & 8)) -ne 0 ] && w="$w alt"
    [ $((t & 16)) -ne 0 ] && w="$w shiftl"
    [ $((t & 64)) -ne 0 ] && w="$w ctrll"
    [ -n "$w" ] || w=" plain"
    echo "${w# }"
}

# entry TABLES KEYS VALUE: one line for each table and key.
entry() {
    for t in $1; do
        for k in $2; do
            echo "$(mods "$t") keycode $k = $3"
        done
    done
}

ALL="$(seq -s ' ' 0 31)"

# The add-on NAME, on stdout.
addon() {
    echo "# $1.map: loaded on top of a layout of this directory: loadkeys -u LAYOUT $1."
    echo "# Made by vendor/keymaps/regen_keymaps.sh; do not edit."
    echo "#"
    case "$1" in
    alt-shift)
        echo "# Left Alt with either Shift, pressed in either order, switches to the"
        echo "# other layout; the second key toggles ShiftL_Lock and does nothing else."
        entry "8 24" "42 54" ShiftL_Lock
        entry "1 17" "56" ShiftL_Lock ;;
    ctrl-shift)
        echo "# Either Ctrl with either Shift, pressed in either order, switches to the"
        echo "# other layout; the second key toggles ShiftL_Lock and does nothing else."
        entry "4 20" "42 54" ShiftL_Lock
        entry "1 17" "29 97" ShiftL_Lock ;;
    caps-toggle)
        echo "# Caps Lock switches to the other layout, and Shift+Caps Lock is Caps Lock."
        entry "0 16" 58 ShiftL_Lock
        entry "1 17" 58 Caps_Lock ;;
    ralt-toggle)
        echo "# Right Alt switches to the other layout in every table, so it is not AltGr."
        entry "$ALL" 100 ShiftL_Lock ;;
    super-space)
        echo "# Either Super, pressed first, holds CtrlL, which no other key uses, and"
        echo "# Space under it switches to the other layout. Super with any other key"
        echo "# types nothing, and Super after another modifier stays Alt."
        echo "keymaps 64,80"
        entry "0 16 64 80" "125 126" CtrlL
        entry "64 80" 57 ShiftL_Lock ;;
    caps-ctrl)
        echo "# Caps Lock is a Control key."
        entry "$ALL" 58 Control ;;
    caps-escape)
        echo "# Caps Lock is an Escape key, Meta_Escape under Alt, as Escape is."
        for t in $ALL; do
            if [ $((t & 8)) -ne 0 ]; then v=Meta_Escape; else v=Escape; fi
            entry "$t" 58 $v
        done ;;
    esac
}

# ckbcomp's map on stdin made fit for Rach2: its keycode rows on stdout.
# ckbcomp writes 128 tables: groups 3 and 4 on ShiftR, which no key of a map
# here holds, and a CtrlL half with capitals for Caps Lock, which ckbcomp
# moves to CtrlL_Lock because Linux's Caps Lock does not shift a Unicode
# letter. Rach2's does, so Caps Lock goes back to Caps_Lock and only tables 0
# to 31 are kept, once no entry in them reaches the others. ckbcomp also
# gives Ctrl and Alt on the second layout that layout's letters, so that
# Ctrl+C would type a Cyrillic letter; those tables take the first layout's
# entries, as a terminal under X takes the Latin key's, so Ctrl+C sends ETX
# and Alt+B ESC b on either layout. Linux's console strings, which ckbcomp
# asks for, give way to xterm's, and the comments of console-setup's
# remap.inc after them are dropped.
normalize() {
    awk -v name="$1" '
    BEGIN { tail = 0 }
    tail {
        if ($0 !~ /^#/ && $0 !~ /^[ \t]*$/) {
            print "gen_keymaps: " name ": unexpected line after the strings: " $0 > "/dev/stderr"
            exit 1
        }
        next
    }
    $1 == "keymaps" {
        if ($2 != "0-127") {
            print "gen_keymaps: " name ": keymaps " $2 > "/dev/stderr"
            exit 1
        }
        next
    }
    $1 == "keycode" {
        if (NF != 131 || $3 != "=") {
            print "gen_keymaps: " name ": keycode " $2 " has " NF - 3 " entries" > "/dev/stderr"
            exit 1
        }
        line = $1 " " $2 " ="
        for (t = 0; t < 32; t++) {
            v = $(t + 4)
            if (v ~ /^(ShiftR|CtrlL|CtrlR)/ && v != "CtrlL_Lock") {
                print "gen_keymaps: " name ": keycode " $2 " has " v > "/dev/stderr"
                exit 1
            }
            if (t >= 16 && t % 16 >= 4)
                v = e[t - 16]
            if (v == "CtrlL_Lock")
                v = "Caps_Lock"
            e[t] = v
            line = line " " v
        }
        print line
        next
    }
    $0 == "strings as usual" {
        tail = 1
        next
    }
    /^[ \t]*$/ { next }
    {
        print "gen_keymaps: " name ": unexpected line: " $0 > "/dev/stderr"
        exit 1
    }
    END {
        if (!tail) {
            print "gen_keymaps: " name ": no strings line" > "/dev/stderr"
            exit 1
        }
    }'
}

echo "$LAYOUTS" | while read -r name layouts variants; do
    [ "$variants" = - ] && variants=""
    ckbcomp -model pc105 -layout "$layouts" ${variants:+-variant "$variants"} \
        > "$WORK/$name.ckb" 2> "$WORK/$name.err"
    if grep -v '^WARNING: Unknown X keysym "dead_' "$WORK/$name.err" | grep -q .; then
        echo "gen_keymaps: $name: ckbcomp:" >&2
        cat "$WORK/$name.err" >&2
        exit 1
    fi
    normalize "$name" < "$WORK/$name.ckb" > "$WORK/$name.rows"
done

# The US map's rows go once into common.inc, which every map includes, and
# each map then gives only its entries that differ: a key with a few, as one
# line for each table, and a key with more, as its whole row. A CIS map
# differs from US only in its second layout, so the staged maps take a fifth
# of the room ckbcomp's would.
{
    echo "# common.inc: the US layout's keys, which every map of this directory"
    echo "# includes before its own."
    echo "# Made by vendor/keymaps/regen_keymaps.sh; do not edit."
    cat "$WORK/us.rows"
} > "$OUT/common.inc"

# The rows of $1 that differ from US, as the lines a map gives after the
# include.
differ() {
    awk '
    function mods(t,   w) {
        w = ""
        if (t % 2) w = w " shift"
        if (int(t / 2) % 2) w = w " altgr"
        if (int(t / 4) % 2) w = w " control"
        if (int(t / 8) % 2) w = w " alt"
        if (int(t / 16) % 2) w = w " shiftl"
        return w == "" ? "plain" : substr(w, 2)
    }
    FNR == NR { base[$2] = $0; next }
    $0 == base[$2] { next }
    {
        n = split(base[$2], b, " ")
        diff = 0
        for (i = 4; i <= NF; i++)
            if (n != NF || $i != b[i])
                diff++
        if (diff > 8) {
            print
            next
        }
        for (i = 4; i <= NF; i++)
            if ($i != b[i])
                print mods(i - 4) " keycode " $2 " = " $i
    }' "$WORK/us.rows" "$1"
}

for a in $ADDONS; do
    addon "$a" > "$OUT/$a.map"
done

echo "$LAYOUTS" | while read -r name layouts variants; do
    [ "$variants" = - ] && variants=""
    groups=1
    case "$layouts" in *,*) groups=2 ;; esac
    desc="XKB $layouts"
    [ -n "$variants" ] && desc="$desc, variants $variants"
    {
        echo "# $name.map: $desc, from xkeyboard-config $XKBVER by ckbcomp"
        echo "# of console-setup $CKBVER."
        echo "# Made by vendor/keymaps/regen_keymaps.sh; do not edit."
        echo "keymaps 0-31"
        echo 'include "common"'
        differ "$WORK/$name.rows"
        echo 'include "xterm-strings"'
    } > "$OUT/$name.map"
    loadkeys -u -m "$OUT/$name.map" > "$WORK/$name.c"
    grep -q 'func_buf + ' "$WORK/$name.c" || {
        echo "gen_keymaps: $name: the strings were not included" >&2; exit 1; }
    # The map with its includes must give the tables ckbcomp's rows give.
    { echo "keymaps 0-31"; cat "$WORK/$name.rows"; } > "$WORK/$name.whole"
    loadkeys -u -m "$WORK/$name.whole" | sed '/^char func_buf/,$d' > "$WORK/$name.wc"
    sed '/^char func_buf/,$d' "$WORK/$name.c" | cmp -s - "$WORK/$name.wc" || {
        echo "gen_keymaps: $name: the map and its rows give different tables" >&2; exit 1; }
    xkbcli compile-keymap --rules evdev --model pc105 --layout "$layouts" \
        ${variants:+--variant "$variants"} > "$WORK/$name.xkb"
    awk -v mode=letters -v groups=$groups -f "$CHECK" \
        "$KEYSYMDEF" "$WORK/$name.xkb" "$WORK/$name.c"
    for a in $ADDONS; do
        loadkeys -u -m "$OUT/$name.map" "$OUT/$a.map" > "$WORK/$name+$a.c"
        awk -v mode=addon -v addon="$a" -f "$CHECK" "$WORK/$name.c" "$WORK/$name+$a.c"
    done
    echo "gen_keymaps: $name: checked"
done

{
    echo '# vendor/keymaps/manifest.txt: provenance of the generated keymaps.'
    echo '# Regenerated only by regen_keymaps.sh; see vendor/keymaps/Readme.md.'
    echo "snapshot $SNAPURL"
    mkdir -p /tmp/debs
    ( cd /tmp/debs && apt-get download -qq $PACKAGES >/dev/null 2>&1 )
    for p in $PACKAGES; do
        deb=$(ls /tmp/debs/${p}_*.deb)
        echo "package $p $(dpkg-query -W -f '${Version}' $p) deb-sha256 $(sha256sum "$deb" | cut -d' ' -f1)"
    done
    # Every package the install added or changed, dependencies included.
    comm -13 /tmp/pkgs.before /tmp/pkgs.after | \
        while read -r p v; do echo "installed $p $v"; done
    echo "$LAYOUTS" | while read -r name layouts variants; do
        echo "layout $name $layouts $variants"
    done
    echo "addons $ADDONS"
} > "$OUT/manifest.head"
