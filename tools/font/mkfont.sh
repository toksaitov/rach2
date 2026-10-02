#!/usr/bin/env sh
#
# mkfont.sh: write rach2/sys/display/vga_font.c, the glyphs the console loads
# into the VGA adapter's font, from GNU Unifont.
#
# Usage: mkfont.sh UNIFONT_HEX_GZ > rach2/sys/display/vga_font.c
#
# UNIFONT_HEX_GZ is unifont_all-18.0.01.hex.gz from
# https://ftp.gnu.org/gnu/unifont/unifont-18.0.01/, whose signature gpgv
# verified with the GNU keyring, made by Paul Hardy's key
# 95D2E9AB8740D8046387FD151A09227B1F435A33; the script refuses a file with
# another digest. The kernel's sources carry the output, since a build on
# Rach2 has no network to fetch the font from.
#
# The layout keeps code page 437's box drawing, shades and blocks at 0xB0 to
# 0xDF, where the adapter widens the line glyphs to nine columns, and puts
# the Cyrillic letters where code page 866 has them. The eighteen box glyphs
# that join a single line to a double one give their places to the letters
# of Kyrgyz and Kazakh, and code page 866's signs at 0xF8 to 0xFF to a few
# signs and the Ukrainian letters.
#
set -e
HEX_SHA256=14c96e497466a82e46cf20032ce510e186bdc8658c9158e2290f452cda9bc498

if [ $# -ne 1 ]; then
    echo "usage: mkfont.sh UNIFONT_HEX_GZ" >&2
    exit 2
fi
sum=$(shasum -a 256 "$1" 2>/dev/null || sha256sum "$1")
if [ "${sum%% *}" != "$HEX_SHA256" ]; then
    echo "mkfont: $1: not unifont_all-18.0.01.hex.gz" >&2
    exit 1
fi

# Slot and Unicode value of every character of the upper half; a slot this
# list gives a letter or a sign to is loaded from Unifont.
layout()
{
    i=0
    while [ $i -lt 32 ]; do
        printf '%d %d load\n' $((0x80 + i)) $((0x410 + i))
        i=$((i + 1))
    done
    i=0
    while [ $i -lt 16 ]; do
        printf '%d %d load\n' $((0xA0 + i)) $((0x430 + i))
        printf '%d %d load\n' $((0xE0 + i)) $((0x440 + i))
        i=$((i + 1))
    done
    set -- \
        B0 2591 B1 2592 B2 2593 B3 2502 B4 2524 B5 04A2 B6 04A3 B7 04E8 \
        B8 04E9 B9 2563 BA 2551 BB 2557 BC 255D BD 04AE BE 04AF BF 2510 \
        C0 2514 C1 2534 C2 252C C3 251C C4 2500 C5 253C C6 04D8 C7 04D9 \
        C8 255A C9 2554 CA 2569 CB 2566 CC 2560 CD 2550 CE 256C CF 0492 \
        D0 0493 D1 049A D2 049B D3 04B0 D4 04B1 D5 04BA D6 04BB D7 0406 \
        D8 0456 D9 2518 DA 250C DB 2588 DC 2584 DD 258C DE 2590 DF 2580 \
        F0 0401 F1 0451 F2 0404 F3 0454 F4 0407 F5 0457 F6 040E F7 045E \
        F8 00B0 F9 2022 FA 00B7 FB 00AB FC 2116 FD 00BB FE 0491 FF 0490
    while [ $# -gt 0 ]; do
        case $2 in
        25*) how=keep ;;
        *) how=load ;;
        esac
        printf '%d %d %s\n' $((0x$1)) $((0x$2)) $how
        shift 2
    done
}

layout | sort -n >"${TMPDIR:-/tmp}/mkfont.$$"
trap 'rm -f "${TMPDIR:-/tmp}/mkfont.$$"' EXIT

gzip -dc "$1" | awk -v layout="${TMPDIR:-/tmp}/mkfont.$$" '
BEGIN {
    while ((getline line < layout) > 0) {
        split(line, f, " ")
        code[f[1]] = f[2]
        how[f[1]] = f[3]
        if (f[3] == "load")
            want[sprintf("%04X", f[2])] = f[1]
    }
}
{
    split($0, f, ":")
    if (f[1] in want) {
        if (length(f[2]) != 32) {
            print "mkfont: U+" f[1] " is not 8 pixels wide" > "/dev/stderr"
            exit 1
        }
        glyph[want[f[1]]] = f[2]
    }
}
END {
    print "/*"
    print " * vga_font.c: the glyphs the console loads into the VGA adapter."
    print " *"
    print " * Written by tools/font/mkfont.sh from GNU Unifont 18.0.01, under the"
    print " * SIL Open Font License 1.1; do not edit. The upper half of the font"
    print " * keeps code page 437 box drawing and has the Cyrillic letters of"
    print " * Russian, Kyrgyz, Kazakh, Ukrainian and Belarusian."
    print " */"
    print "#include \"vga_font.h\""
    print ""
    print "const uint16_t vga_font_map[128] = {"
    line = "   "
    for (s = 128; s < 256; s++) {
        line = line sprintf(" 0x%04X,", code[s])
        if ((s - 127) % 12 == 0 || s == 255) {
            print line
            line = "   "
        }
    }
    print "};"
    print ""
    print "const struct vga_glyph vga_font_glyphs[VGA_FONT_GLYPHS] = {"
    n = 0
    for (s = 128; s < 256; s++) {
        if (how[s] != "load")
            continue
        if (!(s in glyph)) {
            printf "mkfont: U+%04X is not in the font\n", code[s] > "/dev/stderr"
            exit 1
        }
        printf "    {0x%02X,\n     {", s
        for (r = 0; r < 15; r++)
            printf "0x%s%s", substr(glyph[s], r * 2 + 1, 2), (r < 14 ? ", " : ",\n")
        printf "      0x%s}},\n", substr(glyph[s], 31, 2)
        n++
    }
    print "};"
    if (n != 98) {
        print "mkfont: " n " glyphs, not the 98 vga_font.h declares" > "/dev/stderr"
        exit 1
    }
}'
