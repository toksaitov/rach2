# check_keymap.awk: check the keymaps regen_keymaps.sh makes, from the C
# tables `loadkeys -u -m` writes for them.
#
# Run in the container by gen_keymaps.sh, with -v mode=letters or mode=addon.
#
# mode=letters, -v groups=N, files KEYSYMDEF XKBMAP TABLES: every character
# XKB gives a key of the main block, by libxkbcommon's compiled keymap XKBMAP,
# is the one the map's table for its group and level holds, and on a map of
# two layouts Ctrl+C sends ETX on the second. Group 1 is tables 0 to 3, group
# 2 tables 16 to 19, and levels 1 to 4 are plain, Shift, AltGr and
# Shift+AltGr. KEYSYMDEF is X11's keysymdef.h, for each keysym's character.
#
# mode=addon, -v addon=NAME, files BASE COMBINED: the tables of a layout and
# of the layout with the add-on loaded on top differ only in the add-on's
# keys, and the add-on's keys do what its name says from either layout, by a
# replay of presses and releases with Linux's rules for KT_SHIFT and KT_LOCK.

function hex(s,   i, c, n) {
    s = tolower(s)
    sub(/^0x/, "", s)
    n = 0
    for (i = 1; i <= length(s); i++) {
        c = index("0123456789abcdef", substr(s, i, 1))
        if (c == 0)
            return -1
        n = n * 16 + c - 1
    }
    return n
}

# The character a table entry types, or -1: Unicode entries are stored as
# themselves, KT_LATIN (0xf0) and KT_LETTER (0xfb) carry a Latin-1 byte.
function entrychar(v,   t) {
    if (v < 61440)
        return v
    t = int(v / 256) - 240
    if (t == 0 || t == 11)
        return v % 256
    return -1
}

# Reads the C tables of FILE into cell[TAG, TABLE, KEY] and has[TAG, TABLE].
function readtables(file, tag,   line, name, k, n, i, f, pos, inmap, inkm) {
    inmap = 0
    inkm = 0
    pos = 0
    while ((getline line < file) > 0) {
        if (match(line, /unsigned short [a-z_]+_map\[NR_KEYS\]/)) {
            name = substr(line, RSTART + 15, RLENGTH - 15)
            sub(/\[NR_KEYS\]$/, "", name)
            inmap = 1
            k = 0
            continue
        }
        if (line ~ /unsigned short \*key_maps\[/) {
            inkm = 1
            continue
        }
        if ((inmap || inkm) && line ~ /^};/) {
            inmap = 0
            inkm = 0
            continue
        }
        if (inmap) {
            n = split(line, f, /[ \t,]+/)
            for (i = 1; i <= n; i++)
                if (f[i] ~ /^0x/)
                    val[tag, name, k++] = hex(f[i])
        } else if (inkm) {
            n = split(line, f, /[ \t,]+/)
            for (i = 1; i <= n; i++) {
                if (f[i] == "")
                    continue
                if (f[i] != "0")
                    mapname[tag, pos] = f[i]
                pos++
            }
        }
    }
    close(file)
    for (i = 0; i < 256; i++) {
        if (!((tag, i) in mapname))
            continue
        has[tag, i] = 1
        for (k = 0; k < 256; k++)
            cell[tag, i, k] = val[tag, mapname[tag, i], k]
    }
}

function fail(msg) {
    print "check_keymap: " label ": " msg > "/dev/stderr"
    bad = 1
}

# Linux's rules, in short: the table is the held modifiers XOR the locks; a
# KT_SHIFT entry sets its modifier's bit on press and clears it on release;
# a KT_LOCK entry toggles its lock on press. Returns the entry pressed.
function press(tag, key,   v, t) {
    v = cell[tag, xor(shift, lock), key]
    if (!has[tag, xor(shift, lock)])
        v = 61952
    t = int(v / 256) - 240
    if (t == 7 && !bit(shift, v % 256))
        shift += 2 ^ (v % 256)
    else if (t == 10)
        lock = bit(lock, v % 256) ? lock - 2 ^ (v % 256) : lock + 2 ^ (v % 256)
    return v
}

function release(tag, key,   v, t) {
    v = cell[tag, xor(shift, lock), key]
    if (!has[tag, xor(shift, lock)])
        v = 61952
    t = int(v / 256) - 240
    if (t == 7 && bit(shift, v % 256))
        shift -= 2 ^ (v % 256)
}

function bit(n, b) {
    return int(n / 2 ^ b) % 2
}

# XOR of two masks of 8 bits.
function xor(a, b,   i, r) {
    r = 0
    for (i = 0; i < 8; i++)
        if (bit(a, i) != bit(b, i))
            r += 2 ^ i
    return r
}

# Presses A and then B from the layout whose lock is FROM, releases them in
# both orders, and requires the layout switched with no modifier left held.
function toggle(a, b, from,   order) {
    for (order = 0; order < 2; order++) {
        shift = 0
        lock = from
        press("c", a)
        if (b != "")
            press("c", b)
        if (order == 0) {
            if (b != "")
                release("c", b)
            release("c", a)
        } else {
            release("c", a)
            if (b != "")
                release("c", b)
        }
        if (lock != xor(from, 16) || shift != 0)
            fail(sprintf("keys %s %s from table %d leave lock %d and held %d", a, b, from, lock, shift))
    }
}

# The entry KEY types with modifier key MOD held, from the layout FROM.
function under(mod, key, from,   v) {
    shift = 0
    lock = from
    if (mod != "")
        press("c", mod)
    v = press("c", key)
    return v
}

function checkaddon(   allowed, n, i, f, t, k, from, v) {
    n = split(keys[addon], f, " ")
    for (i = 1; i <= n; i++)
        allowed[f[i]] = 1
    for (t = 0; t < 256; t++) {
        if (has["b", t] && !has["c", t])
            fail("table " t " is gone")
        if (!has["c", t])
            continue
        for (k = 0; k < 256; k++) {
            if (has["b", t] && cell["b", t, k] == cell["c", t, k])
                continue
            if (!has["b", t] && cell["c", t, k] == 61952)
                continue
            if (!(k in allowed))
                fail("changes key " k " in table " t)
        }
    }
    for (from = 0; from <= 16; from += 16) {
        if (addon == "alt-shift") {
            toggle(56, 42, from); toggle(42, 56, from)
            toggle(56, 54, from); toggle(54, 56, from)
        } else if (addon == "ctrl-shift") {
            toggle(29, 42, from); toggle(42, 29, from)
            toggle(97, 54, from); toggle(54, 97, from)
        } else if (addon == "caps-toggle") {
            toggle(58, "", from)
            if ((v = under(42, 58, from)) != 61959)
                fail(sprintf("Shift+Caps Lock gives 0x%x", v))
        } else if (addon == "ralt-toggle") {
            toggle(100, "", from)
        } else if (addon == "super-space") {
            toggle(125, 57, from); toggle(126, 57, from)
        } else if (addon == "caps-ctrl") {
            if ((v = under(58, 46, from)) != cell["b", 4 + from, 46])
                fail(sprintf("Caps Lock with key 46 gives 0x%x", v))
        } else if (addon == "caps-escape") {
            if ((v = under("", 58, from)) != 61467)
                fail(sprintf("Caps Lock gives 0x%x", v))
        } else {
            fail("no check for this add-on")
        }
    }
}

# keysymdef.h: each keysym's character, from its value or its U+ comment.
function readkeysyms(file,   line, name, v) {
    while ((getline line < file) > 0) {
        if (line !~ /^#define XK_[A-Za-z0-9_]+[ \t]+0x[0-9a-fA-F]+/)
            continue
        name = line
        sub(/^#define XK_/, "", name)
        sub(/[ \t].*/, "", name)
        v = line
        sub(/^#define XK_[A-Za-z0-9_]+[ \t]+/, "", v)
        sub(/[ \t].*/, "", v)
        v = hex(v)
        if (match(line, /U\+[0-9A-Fa-f]+/))
            ksym[name] = hex(substr(line, RSTART + 2, RLENGTH - 2))
        else if (v >= 16777216)
            ksym[name] = v - 16777216
        else if (v >= 32 && v < 256)
            ksym[name] = v
    }
    close(file)
}

function symchar(s) {
    if (s in ksym)
        return ksym[s]
    if (s ~ /^U[0-9A-Fa-f]+$/)
        return hex(substr(s, 2))
    if (s ~ /^0x0*1[0-9a-fA-F]{6}$/)
        return hex(s) - 16777216
    return -1
}

# One key of the compiled XKB keymap: compares each group's levels 1 to 4.
function checkkey(name, body,   g, rest, list, n, i, f, u, t, e, kc) {
    if (name !~ /^(TLDE|AE[0-9][0-9]|AD[0-9][0-9]|AC[0-9][0-9]|AB[0-9][0-9]|BKSL|LSGT|SPCE)$/)
        return
    if (!(name in xkc))
        return
    kc = xkc[name] - 8
    g = 0
    rest = body
    while (match(rest, /(symbols\[(Group)?[0-9]\][ \t]*=[ \t]*)?\[[^]]*\]/)) {
        list = substr(rest, RSTART, RLENGTH)
        rest = substr(rest, RSTART + RLENGTH)
        if (list ~ /^symbols/) {
            g = list
            sub(/^symbols\[(Group)?/, "", g)
            sub(/\].*/, "", g)
            g = g + 0
        } else {
            g++
        }
        sub(/^.*\[/, "", list)
        sub(/\]$/, "", list)
        if (g > groups)
            continue
        n = split(list, f, /[ \t]*,[ \t]*/)
        for (i = 1; i <= n && i <= 4; i++) {
            gsub(/[ \t]/, "", f[i])
            u = symchar(f[i])
            if (u < 32 || u == 127 || u >= 61440)
                continue
            t = (g - 1) * 16 + (i - 1)
            e = entrychar(cell["m", t, kc])
            checked++
            if (e != u)
                fail(sprintf("<%s> group %d level %d: XKB %s U+%04X, table %d has 0x%04x", name, g, i, f[i], u, t, cell["m", t, kc]))
        }
    }
}

function checkletters(file,   line, sect, name, body, inkey, v) {
    sect = ""
    inkey = 0
    while ((getline line < file) > 0) {
        if (line ~ /^[ \t]*xkb_keycodes/)
            sect = "keycodes"
        else if (line ~ /^[ \t]*xkb_symbols/)
            sect = "symbols"
        else if (line ~ /^[ \t]*xkb_(types|compatibility|compat|geometry)/)
            sect = ""
        if (sect == "keycodes" && match(line, /<[A-Z0-9+-]+>[ \t]*=[ \t]*[0-9]+;/)) {
            name = substr(line, RSTART + 1)
            sub(/>.*/, "", name)
            v = line
            sub(/^.*=[ \t]*/, "", v)
            sub(/;.*/, "", v)
            xkc[name] = v + 0
        }
        if (sect != "symbols")
            continue
        if (!inkey && match(line, /key[ \t]+<[A-Z0-9+-]+>/)) {
            name = substr(line, RSTART, RLENGTH)
            sub(/^key[ \t]+</, "", name)
            sub(/>$/, "", name)
            body = substr(line, RSTART + RLENGTH)
            inkey = 1
        } else if (inkey) {
            body = body " " line
        }
        if (inkey && body ~ /};/) {
            checkkey(name, body)
            inkey = 0
        }
    }
    close(file)
}

BEGIN {
    bad = 0
    label = ARGV[ARGC - 1]
    keys["alt-shift"] = "42 54 56"
    keys["ctrl-shift"] = "29 97 42 54"
    keys["caps-toggle"] = "58"
    keys["ralt-toggle"] = "100"
    keys["super-space"] = "57 125 126"
    keys["caps-ctrl"] = "58"
    keys["caps-escape"] = "58"
    if (mode == "letters") {
        readkeysyms(ARGV[1])
        readtables(ARGV[3], "m")
        if (!groups)
            groups = has["m", 16] ? 2 : 1
        checked = 0
        checkletters(ARGV[2])
        if (checked < 47 * groups)
            fail("only " checked " characters checked")
        # US is the first layout of every map of two, so Ctrl+C sends ETX
        # on the second as on the first.
        if (groups == 2 && cell["m", 20, 46] != 61443)
            fail(sprintf("Ctrl+C on the second layout gives 0x%x", cell["m", 20, 46]))
    } else if (mode == "addon") {
        label = addon " on " ARGV[1]
        readtables(ARGV[1], "b")
        readtables(ARGV[2], "c")
        checkaddon()
    } else {
        fail("no mode")
    }
    exit bad
}
