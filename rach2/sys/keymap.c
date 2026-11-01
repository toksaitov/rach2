/*
 * keymap.c: Linux keymap translation and xterm key encoding, shared by the
 * kernel console and term. This file has no kernel or C library calls.
 */
#include "keymap.h"

/* US keys from kbd.c, indexed by Linux keycode, with Linux keysyms. */
#define H    KM_HOLE
#define L(c) KM_K(KM_T_LETTER, (c))
#define C(c) KM_K(KM_T_LATIN, (c))
#define F(n) KM_K(KM_T_FN, (n))
#define P(n) KM_K(KM_T_PAD, (n))
#define S(n) KM_K(KM_T_SHIFT, (n))
#define R(n) KM_K(KM_T_CUR, (n))

static const unsigned short us[KM_NR_KEYS] = {
    H,
    C(0x1B),
    C(0x31),
    C(0x32),
    C(0x33),
    C(0x34),
    C(0x35),
    C(0x36),
    C(0x37),
    C(0x38),
    C(0x39),
    C(0x30),
    C(0x2D),
    C(0x3D),
    C(0x7F),
    C(0x09),
    L(0x71),
    L(0x77),
    L(0x65),
    L(0x72),
    L(0x74),
    L(0x79),
    L(0x75),
    L(0x69),
    L(0x6F),
    L(0x70),
    C(0x5B),
    C(0x5D),
    KM_ENTER,
    S(KM_G_CTRL),
    L(0x61),
    L(0x73),
    L(0x64),
    L(0x66),
    L(0x67),
    L(0x68),
    L(0x6A),
    L(0x6B),
    L(0x6C),
    C(0x3B),
    C(0x27),
    C(0x60),
    S(KM_G_SHIFT),
    C(0x5C),
    L(0x7A),
    L(0x78),
    L(0x63),
    L(0x76),
    L(0x62),
    L(0x6E),
    L(0x6D),
    C(0x2C),
    C(0x2E),
    C(0x2F),
    S(KM_G_SHIFT),
    P(12),
    S(KM_G_ALT),
    C(0x20),
    KM_CAPS,
    F(0),
    F(1),
    F(2),
    F(3),
    F(4),
    F(5),
    F(6),
    F(7),
    F(8),
    F(9),
    KM_NUM,
    KM_HOLD,
    P(7),
    P(8),
    P(9),
    P(11),
    P(4),
    P(5),
    P(6),
    P(10),
    P(1),
    P(2),
    P(3),
    P(0),
    P(16),
    H,
    H,
    C(0x3C),
    F(10),
    F(11),
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    P(14),
    S(KM_G_CTRL),
    P(13),
    H,
    S(KM_G_ALT),
    H,
    F(20),
    R(3),
    F(24),
    R(1),
    R(2),
    F(23),
    R(0),
    F(25),
    F(21),
    F(22),
    H,
    H,
    H,
    H,
    H,
    C(0x3D),
    H,
    H,
    H,
    P(15),
    H,
    H,
    H,
    H,
    H,
    F(15),
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
};

static const unsigned short us_shift[KM_NR_KEYS] = {
    H,
    C(0x1B),
    C(0x21),
    C(0x40),
    C(0x23),
    C(0x24),
    C(0x25),
    C(0x5E),
    C(0x26),
    C(0x2A),
    C(0x28),
    C(0x29),
    C(0x5F),
    C(0x2B),
    C(0x7F),
    C(0x09),
    L(0x51),
    L(0x57),
    L(0x45),
    L(0x52),
    L(0x54),
    L(0x59),
    L(0x55),
    L(0x49),
    L(0x4F),
    L(0x50),
    C(0x7B),
    C(0x7D),
    KM_ENTER,
    S(KM_G_CTRL),
    L(0x41),
    L(0x53),
    L(0x44),
    L(0x46),
    L(0x47),
    L(0x48),
    L(0x4A),
    L(0x4B),
    L(0x4C),
    C(0x3A),
    C(0x22),
    C(0x7E),
    S(KM_G_SHIFT),
    C(0x7C),
    L(0x5A),
    L(0x58),
    L(0x43),
    L(0x56),
    L(0x42),
    L(0x4E),
    L(0x4D),
    C(0x3C),
    C(0x3E),
    C(0x3F),
    S(KM_G_SHIFT),
    P(12),
    S(KM_G_ALT),
    C(0x20),
    KM_CAPS,
    F(0),
    F(1),
    F(2),
    F(3),
    F(4),
    F(5),
    F(6),
    F(7),
    F(8),
    F(9),
    KM_NUM,
    KM_HOLD,
    P(7),
    P(8),
    P(9),
    P(11),
    P(4),
    P(5),
    P(6),
    P(10),
    P(1),
    P(2),
    P(3),
    P(0),
    P(16),
    H,
    H,
    C(0x3E),
    F(10),
    F(11),
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    P(14),
    S(KM_G_CTRL),
    P(13),
    H,
    S(KM_G_ALT),
    H,
    F(20),
    R(3),
    F(24),
    R(1),
    R(2),
    F(23),
    R(0),
    F(25),
    F(21),
    F(22),
    H,
    H,
    H,
    H,
    H,
    C(0x3D),
    H,
    H,
    H,
    P(15),
    H,
    H,
    H,
    H,
    H,
    F(15),
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
};

static const unsigned short us_ctrl[KM_NR_KEYS] = {
    H,
    C(0x1B),
    C(0x31),
    C(0x00),
    C(0x1B),
    C(0x1C),
    C(0x1D),
    C(0x1E),
    C(0x1F),
    C(0x7F),
    C(0x39),
    C(0x30),
    C(0x2D),
    C(0x3D),
    C(0x08),
    C(0x09),
    C(0x11),
    C(0x17),
    C(0x05),
    C(0x12),
    C(0x14),
    C(0x19),
    C(0x15),
    C(0x09),
    C(0x0F),
    C(0x10),
    C(0x1B),
    C(0x1D),
    KM_ENTER,
    S(KM_G_CTRL),
    C(0x01),
    C(0x13),
    C(0x04),
    C(0x06),
    C(0x07),
    C(0x08),
    C(0x0A),
    C(0x0B),
    C(0x0C),
    C(0x3B),
    C(0x27),
    C(0x00),
    S(KM_G_SHIFT),
    C(0x1C),
    C(0x1A),
    C(0x18),
    C(0x03),
    C(0x16),
    C(0x02),
    C(0x0E),
    C(0x0D),
    C(0x2C),
    C(0x2E),
    C(0x1F),
    S(KM_G_SHIFT),
    P(12),
    S(KM_G_ALT),
    C(0x00),
    KM_CAPS,
    F(0),
    F(1),
    F(2),
    F(3),
    F(4),
    F(5),
    F(6),
    F(7),
    F(8),
    F(9),
    KM_NUM,
    KM_HOLD,
    P(7),
    P(8),
    P(9),
    P(11),
    P(4),
    P(5),
    P(6),
    P(10),
    P(1),
    P(2),
    P(3),
    P(0),
    P(16),
    H,
    H,
    C(0x3C),
    F(10),
    F(11),
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    P(14),
    S(KM_G_CTRL),
    P(13),
    H,
    S(KM_G_ALT),
    H,
    F(20),
    R(3),
    F(24),
    R(1),
    R(2),
    F(23),
    R(0),
    F(25),
    F(21),
    F(22),
    H,
    H,
    H,
    H,
    H,
    C(0x3D),
    H,
    H,
    H,
    P(15),
    H,
    H,
    H,
    H,
    H,
    F(15),
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
};

static const unsigned short us_ctrl_shift[KM_NR_KEYS] = {
    H,
    C(0x1B),
    C(0x21),
    C(0x00),
    C(0x23),
    C(0x24),
    C(0x25),
    C(0x1E),
    C(0x26),
    C(0x2A),
    C(0x28),
    C(0x29),
    C(0x1F),
    C(0x2B),
    C(0x08),
    C(0x09),
    C(0x11),
    C(0x17),
    C(0x05),
    C(0x12),
    C(0x14),
    C(0x19),
    C(0x15),
    C(0x09),
    C(0x0F),
    C(0x10),
    C(0x1B),
    C(0x1D),
    KM_ENTER,
    S(KM_G_CTRL),
    C(0x01),
    C(0x13),
    C(0x04),
    C(0x06),
    C(0x07),
    C(0x08),
    C(0x0A),
    C(0x0B),
    C(0x0C),
    C(0x3A),
    C(0x22),
    C(0x1E),
    S(KM_G_SHIFT),
    C(0x1C),
    C(0x1A),
    C(0x18),
    C(0x03),
    C(0x16),
    C(0x02),
    C(0x0E),
    C(0x0D),
    C(0x3C),
    C(0x3E),
    C(0x7F),
    S(KM_G_SHIFT),
    P(12),
    S(KM_G_ALT),
    C(0x00),
    KM_CAPS,
    F(0),
    F(1),
    F(2),
    F(3),
    F(4),
    F(5),
    F(6),
    F(7),
    F(8),
    F(9),
    KM_NUM,
    KM_HOLD,
    P(7),
    P(8),
    P(9),
    P(11),
    P(4),
    P(5),
    P(6),
    P(10),
    P(1),
    P(2),
    P(3),
    P(0),
    P(16),
    H,
    H,
    C(0x3E),
    F(10),
    F(11),
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    P(14),
    S(KM_G_CTRL),
    P(13),
    H,
    S(KM_G_ALT),
    H,
    F(20),
    R(3),
    F(24),
    R(1),
    R(2),
    F(23),
    R(0),
    F(25),
    F(21),
    F(22),
    H,
    H,
    H,
    H,
    H,
    C(0x3D),
    H,
    H,
    H,
    P(15),
    H,
    H,
    H,
    H,
    H,
    F(15),
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
    H,
};

/* Alt is carried in the result; AltGr uses the US column by default. */
const unsigned short *const keymap_default_tables[KM_MAX_TABLES] = {
    us, us_shift, us, us_shift, us_ctrl, us_ctrl_shift, us_ctrl, us_ctrl_shift,
    us, us_shift, us, us_shift, us_ctrl, us_ctrl_shift, us_ctrl, us_ctrl_shift};

/* xterm ctlseqs, patch 412: PC-style function keys, oldXtermFKeys false. */
const char *const keymap_default_func[KM_MAX_FUNC] = {
    "\033OP",   "\033OQ",   "\033OR",   "\033OS",   "\033[15~", "\033[17~", "\033[18~",
    "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~", "\033[25~", "\033[26~",
    "\033[28~", "\033[29~", "\033[31~", "\033[32~", "\033[33~", "\033[34~", "\033[H",
    "\033[2~",  "\033[3~",  "\033[F",   "\033[5~",  "\033[6~"};

/*
 * Latin-1 and Latin Extended A/B accents on ASCII letters, libX11 1.8.12,
 * nls/en_US.UTF-8/Compose.pre, originally by David Monniaux.
 * https://www.x.org/releases/individual/lib/libX11-1.8.12.tar.xz
 *
 * Copyright (C) 2003-2006,2008 Jamey Sharp, Josh Triplett
 * Copyright (C) 2009 Red Hat, Inc.
 * Copyright (c) 1990-1992, 1999, 2000, 2004, 2009, 2010, 2015, 2017, Oracle
 * and/or its affiliates.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
const struct keymap_diacr keymap_default_diacr[] = {
    {0x0022, 0x0041, 0x00C4}, {0x0022, 0x0045, 0x00CB}, {0x0022, 0x0049, 0x00CF},
    {0x0022, 0x004F, 0x00D6}, {0x0022, 0x0055, 0x00DC}, {0x0022, 0x0059, 0x0178},
    {0x0022, 0x0061, 0x00E4}, {0x0022, 0x0065, 0x00EB}, {0x0022, 0x0069, 0x00EF},
    {0x0022, 0x006F, 0x00F6}, {0x0022, 0x0075, 0x00FC}, {0x0022, 0x0079, 0x00FF},
    {0x0027, 0x0041, 0x00C1}, {0x0027, 0x0043, 0x0106}, {0x0027, 0x0045, 0x00C9},
    {0x0027, 0x0047, 0x01F4}, {0x0027, 0x0049, 0x00CD}, {0x0027, 0x004C, 0x0139},
    {0x0027, 0x004E, 0x0143}, {0x0027, 0x004F, 0x00D3}, {0x0027, 0x0052, 0x0154},
    {0x0027, 0x0053, 0x015A}, {0x0027, 0x0055, 0x00DA}, {0x0027, 0x0056, 0x01D7},
    {0x0027, 0x0059, 0x00DD}, {0x0027, 0x005A, 0x0179}, {0x0027, 0x0061, 0x00E1},
    {0x0027, 0x0063, 0x0107}, {0x0027, 0x0065, 0x00E9}, {0x0027, 0x0067, 0x01F5},
    {0x0027, 0x0069, 0x00ED}, {0x0027, 0x006C, 0x013A}, {0x0027, 0x006E, 0x0144},
    {0x0027, 0x006F, 0x00F3}, {0x0027, 0x0072, 0x0155}, {0x0027, 0x0073, 0x015B},
    {0x0027, 0x0075, 0x00FA}, {0x0027, 0x0076, 0x01D8}, {0x0027, 0x0079, 0x00FD},
    {0x0027, 0x007A, 0x017A}, {0x005E, 0x0041, 0x00C2}, {0x005E, 0x0043, 0x0108},
    {0x005E, 0x0045, 0x00CA}, {0x005E, 0x0047, 0x011C}, {0x005E, 0x0048, 0x0124},
    {0x005E, 0x0049, 0x00CE}, {0x005E, 0x004A, 0x0134}, {0x005E, 0x004F, 0x00D4},
    {0x005E, 0x0053, 0x015C}, {0x005E, 0x0055, 0x00DB}, {0x005E, 0x0057, 0x0174},
    {0x005E, 0x0059, 0x0176}, {0x005E, 0x0061, 0x00E2}, {0x005E, 0x0063, 0x0109},
    {0x005E, 0x0065, 0x00EA}, {0x005E, 0x0067, 0x011D}, {0x005E, 0x0068, 0x0125},
    {0x005E, 0x0069, 0x00EE}, {0x005E, 0x006A, 0x0135}, {0x005E, 0x006F, 0x00F4},
    {0x005E, 0x0073, 0x015D}, {0x005E, 0x0075, 0x00FB}, {0x005E, 0x0077, 0x0175},
    {0x005E, 0x0079, 0x0177}, {0x0060, 0x0041, 0x00C0}, {0x0060, 0x0045, 0x00C8},
    {0x0060, 0x0049, 0x00CC}, {0x0060, 0x004E, 0x01F8}, {0x0060, 0x004F, 0x00D2},
    {0x0060, 0x0055, 0x00D9}, {0x0060, 0x0056, 0x01DB}, {0x0060, 0x0061, 0x00E0},
    {0x0060, 0x0065, 0x00E8}, {0x0060, 0x0069, 0x00EC}, {0x0060, 0x006E, 0x01F9},
    {0x0060, 0x006F, 0x00F2}, {0x0060, 0x0075, 0x00F9}, {0x0060, 0x0076, 0x01DC},
    {0x007E, 0x0041, 0x00C3}, {0x007E, 0x0049, 0x0128}, {0x007E, 0x004E, 0x00D1},
    {0x007E, 0x004F, 0x00D5}, {0x007E, 0x0055, 0x0168}, {0x007E, 0x0061, 0x00E3},
    {0x007E, 0x0069, 0x0129}, {0x007E, 0x006E, 0x00F1}, {0x007E, 0x006F, 0x00F5},
    {0x007E, 0x0075, 0x0169}, {0x00AF, 0x0041, 0x0100}, {0x00AF, 0x0045, 0x0112},
    {0x00AF, 0x0049, 0x012A}, {0x00AF, 0x004F, 0x014C}, {0x00AF, 0x0055, 0x016A},
    {0x00AF, 0x0056, 0x01D5}, {0x00AF, 0x0059, 0x0232}, {0x00AF, 0x0061, 0x0101},
    {0x00AF, 0x0065, 0x0113}, {0x00AF, 0x0069, 0x012B}, {0x00AF, 0x006F, 0x014D},
    {0x00AF, 0x0075, 0x016B}, {0x00AF, 0x0076, 0x01D6}, {0x00AF, 0x0079, 0x0233},
    {0x00B0, 0x0041, 0x00C5}, {0x00B0, 0x0055, 0x016E}, {0x00B0, 0x0061, 0x00E5},
    {0x00B0, 0x0075, 0x016F}, {0x00B8, 0x0043, 0x00C7}, {0x00B8, 0x0045, 0x0228},
    {0x00B8, 0x0047, 0x0122}, {0x00B8, 0x004B, 0x0136}, {0x00B8, 0x004C, 0x013B},
    {0x00B8, 0x004E, 0x0145}, {0x00B8, 0x0052, 0x0156}, {0x00B8, 0x0053, 0x015E},
    {0x00B8, 0x0054, 0x0162}, {0x00B8, 0x0063, 0x00E7}, {0x00B8, 0x0065, 0x0229},
    {0x00B8, 0x0067, 0x0123}, {0x00B8, 0x006B, 0x0137}, {0x00B8, 0x006C, 0x013C},
    {0x00B8, 0x006E, 0x0146}, {0x00B8, 0x0072, 0x0157}, {0x00B8, 0x0073, 0x015F},
    {0x00B8, 0x0074, 0x0163}, {0x02C7, 0x0041, 0x01CD}, {0x02C7, 0x0043, 0x010C},
    {0x02C7, 0x0044, 0x010E}, {0x02C7, 0x0045, 0x011A}, {0x02C7, 0x0047, 0x01E6},
    {0x02C7, 0x0048, 0x021E}, {0x02C7, 0x0049, 0x01CF}, {0x02C7, 0x004B, 0x01E8},
    {0x02C7, 0x004C, 0x013D}, {0x02C7, 0x004E, 0x0147}, {0x02C7, 0x004F, 0x01D1},
    {0x02C7, 0x0052, 0x0158}, {0x02C7, 0x0053, 0x0160}, {0x02C7, 0x0054, 0x0164},
    {0x02C7, 0x0055, 0x01D3}, {0x02C7, 0x0056, 0x01D9}, {0x02C7, 0x005A, 0x017D},
    {0x02C7, 0x0061, 0x01CE}, {0x02C7, 0x0063, 0x010D}, {0x02C7, 0x0064, 0x010F},
    {0x02C7, 0x0065, 0x011B}, {0x02C7, 0x0067, 0x01E7}, {0x02C7, 0x0068, 0x021F},
    {0x02C7, 0x0069, 0x01D0}, {0x02C7, 0x006A, 0x01F0}, {0x02C7, 0x006B, 0x01E9},
    {0x02C7, 0x006C, 0x013E}, {0x02C7, 0x006E, 0x0148}, {0x02C7, 0x006F, 0x01D2},
    {0x02C7, 0x0072, 0x0159}, {0x02C7, 0x0073, 0x0161}, {0x02C7, 0x0074, 0x0165},
    {0x02C7, 0x0075, 0x01D4}, {0x02C7, 0x0076, 0x01DA}, {0x02C7, 0x007A, 0x017E},
    {0x02D8, 0x0041, 0x0102}, {0x02D8, 0x0045, 0x0114}, {0x02D8, 0x0047, 0x011E},
    {0x02D8, 0x0049, 0x012C}, {0x02D8, 0x004F, 0x014E}, {0x02D8, 0x0055, 0x016C},
    {0x02D8, 0x0061, 0x0103}, {0x02D8, 0x0065, 0x0115}, {0x02D8, 0x0067, 0x011F},
    {0x02D8, 0x0069, 0x012D}, {0x02D8, 0x006F, 0x014F}, {0x02D8, 0x0075, 0x016D},
    {0x02D9, 0x0041, 0x0226}, {0x02D9, 0x0043, 0x010A}, {0x02D9, 0x0045, 0x0116},
    {0x02D9, 0x0047, 0x0120}, {0x02D9, 0x0049, 0x0130}, {0x02D9, 0x004C, 0x013F},
    {0x02D9, 0x004F, 0x022E}, {0x02D9, 0x005A, 0x017B}, {0x02D9, 0x0061, 0x0227},
    {0x02D9, 0x0063, 0x010B}, {0x02D9, 0x0065, 0x0117}, {0x02D9, 0x0067, 0x0121},
    {0x02D9, 0x0069, 0x0131}, {0x02D9, 0x006A, 0x0237}, {0x02D9, 0x006C, 0x0140},
    {0x02D9, 0x006F, 0x022F}, {0x02D9, 0x007A, 0x017C}, {0x02DB, 0x0041, 0x0104},
    {0x02DB, 0x0045, 0x0118}, {0x02DB, 0x0049, 0x012E}, {0x02DB, 0x004F, 0x01EA},
    {0x02DB, 0x0055, 0x0172}, {0x02DB, 0x0061, 0x0105}, {0x02DB, 0x0065, 0x0119},
    {0x02DB, 0x0069, 0x012F}, {0x02DB, 0x006F, 0x01EB}, {0x02DB, 0x0075, 0x0173},
    {0x02DD, 0x004F, 0x0150}, {0x02DD, 0x0055, 0x0170}, {0x02DD, 0x006F, 0x0151},
    {0x02DD, 0x0075, 0x0171},
};

const unsigned int keymap_default_ndiacr =
    sizeof(keymap_default_diacr) / sizeof(keymap_default_diacr[0]);

/*
 * Simple uppercase mappings, UnicodeData.txt 17.0.0, BMP only.
 * https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt
 *
 * UNICODE LICENSE V3
 *
 * COPYRIGHT AND PERMISSION NOTICE
 *
 * Copyright (C) 1991-2026 Unicode, Inc.
 *
 * NOTICE TO USER: Carefully read the following legal agreement. BY
 * DOWNLOADING, INSTALLING, COPYING OR OTHERWISE USING DATA FILES, AND/OR
 * SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE
 * TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT
 * DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of data files and any associated documentation (the "Data Files") or
 * software and any associated documentation (the "Software") to deal in the
 * Data Files or Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, and/or sell
 * copies of the Data Files or Software, and to permit persons to whom the
 * Data Files or Software are furnished to do so, provided that either (a)
 * this copyright and permission notice appear with all copies of the Data
 * Files or Software, or (b) this copyright and permission notice appear in
 * associated Documentation.
 *
 * THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
 * KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE
 * BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
 * ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA
 * FILES OR SOFTWARE.
 *
 * Except as contained in this notice, the name of a copyright holder shall
 * not be used in advertising or otherwise to promote the sale, use or other
 * dealings in these Data Files or Software without prior written
 * authorization of the copyright holder.
 */
struct case_range {
    unsigned short first, last;
    unsigned short step; /* spacing between lowercase points in the run */
    int delta;           /* added to the lowercase code point */
};

static const struct case_range uppercase[] = {
    {0x0061, 0x007A, 1, -32},   {0x00B5, 0x00B5, 1, 743},    {0x00E0, 0x00F6, 1, -32},
    {0x00F8, 0x00FE, 1, -32},   {0x00FF, 0x00FF, 1, 121},    {0x0101, 0x012F, 2, -1},
    {0x0131, 0x0131, 1, -232},  {0x0133, 0x0137, 2, -1},     {0x013A, 0x0148, 2, -1},
    {0x014B, 0x0177, 2, -1},    {0x017A, 0x017E, 2, -1},     {0x017F, 0x017F, 1, -300},
    {0x0180, 0x0180, 1, 195},   {0x0183, 0x0185, 2, -1},     {0x0188, 0x018C, 4, -1},
    {0x0192, 0x0192, 1, -1},    {0x0195, 0x0195, 1, 97},     {0x0199, 0x0199, 1, -1},
    {0x019A, 0x019A, 1, 163},   {0x019B, 0x019B, 1, 42561},  {0x019E, 0x019E, 1, 130},
    {0x01A1, 0x01A5, 2, -1},    {0x01A8, 0x01AD, 5, -1},     {0x01B0, 0x01B4, 4, -1},
    {0x01B6, 0x01B9, 3, -1},    {0x01BD, 0x01BD, 1, -1},     {0x01BF, 0x01BF, 1, 56},
    {0x01C5, 0x01C5, 1, -1},    {0x01C6, 0x01C6, 1, -2},     {0x01C8, 0x01C8, 1, -1},
    {0x01C9, 0x01C9, 1, -2},    {0x01CB, 0x01CB, 1, -1},     {0x01CC, 0x01CC, 1, -2},
    {0x01CE, 0x01DC, 2, -1},    {0x01DD, 0x01DD, 1, -79},    {0x01DF, 0x01EF, 2, -1},
    {0x01F2, 0x01F2, 1, -1},    {0x01F3, 0x01F3, 1, -2},     {0x01F5, 0x01F9, 4, -1},
    {0x01FB, 0x021F, 2, -1},    {0x0223, 0x0233, 2, -1},     {0x023C, 0x023C, 1, -1},
    {0x023F, 0x0240, 1, 10815}, {0x0242, 0x0247, 5, -1},     {0x0249, 0x024F, 2, -1},
    {0x0250, 0x0250, 1, 10783}, {0x0251, 0x0251, 1, 10780},  {0x0252, 0x0252, 1, 10782},
    {0x0253, 0x0253, 1, -210},  {0x0254, 0x0254, 1, -206},   {0x0256, 0x0257, 1, -205},
    {0x0259, 0x0259, 1, -202},  {0x025B, 0x025B, 1, -203},   {0x025C, 0x025C, 1, 42319},
    {0x0260, 0x0260, 1, -205},  {0x0261, 0x0261, 1, 42315},  {0x0263, 0x0263, 1, -207},
    {0x0264, 0x0264, 1, 42343}, {0x0265, 0x0265, 1, 42280},  {0x0266, 0x0266, 1, 42308},
    {0x0268, 0x0268, 1, -209},  {0x0269, 0x0269, 1, -211},   {0x026A, 0x026A, 1, 42308},
    {0x026B, 0x026B, 1, 10743}, {0x026C, 0x026C, 1, 42305},  {0x026F, 0x026F, 1, -211},
    {0x0271, 0x0271, 1, 10749}, {0x0272, 0x0272, 1, -213},   {0x0275, 0x0275, 1, -214},
    {0x027D, 0x027D, 1, 10727}, {0x0280, 0x0280, 1, -218},   {0x0282, 0x0282, 1, 42307},
    {0x0283, 0x0283, 1, -218},  {0x0287, 0x0287, 1, 42282},  {0x0288, 0x0288, 1, -218},
    {0x0289, 0x0289, 1, -69},   {0x028A, 0x028B, 1, -217},   {0x028C, 0x028C, 1, -71},
    {0x0292, 0x0292, 1, -219},  {0x029D, 0x029D, 1, 42261},  {0x029E, 0x029E, 1, 42258},
    {0x0345, 0x0345, 1, 84},    {0x0371, 0x0373, 2, -1},     {0x0377, 0x0377, 1, -1},
    {0x037B, 0x037D, 1, 130},   {0x03AC, 0x03AC, 1, -38},    {0x03AD, 0x03AF, 1, -37},
    {0x03B1, 0x03C1, 1, -32},   {0x03C2, 0x03C2, 1, -31},    {0x03C3, 0x03CB, 1, -32},
    {0x03CC, 0x03CC, 1, -64},   {0x03CD, 0x03CE, 1, -63},    {0x03D0, 0x03D0, 1, -62},
    {0x03D1, 0x03D1, 1, -57},   {0x03D5, 0x03D5, 1, -47},    {0x03D6, 0x03D6, 1, -54},
    {0x03D7, 0x03D7, 1, -8},    {0x03D9, 0x03EF, 2, -1},     {0x03F0, 0x03F0, 1, -86},
    {0x03F1, 0x03F1, 1, -80},   {0x03F2, 0x03F2, 1, 7},      {0x03F3, 0x03F3, 1, -116},
    {0x03F5, 0x03F5, 1, -96},   {0x03F8, 0x03FB, 3, -1},     {0x0430, 0x044F, 1, -32},
    {0x0450, 0x045F, 1, -80},   {0x0461, 0x0481, 2, -1},     {0x048B, 0x04BF, 2, -1},
    {0x04C2, 0x04CE, 2, -1},    {0x04CF, 0x04CF, 1, -15},    {0x04D1, 0x052F, 2, -1},
    {0x0561, 0x0586, 1, -48},   {0x10D0, 0x10FA, 1, 3008},   {0x10FD, 0x10FF, 1, 3008},
    {0x13F8, 0x13FD, 1, -8},    {0x1C80, 0x1C80, 1, -6254},  {0x1C81, 0x1C81, 1, -6253},
    {0x1C82, 0x1C82, 1, -6244}, {0x1C83, 0x1C84, 1, -6242},  {0x1C85, 0x1C85, 1, -6243},
    {0x1C86, 0x1C86, 1, -6236}, {0x1C87, 0x1C87, 1, -6181},  {0x1C88, 0x1C88, 1, 35266},
    {0x1C8A, 0x1C8A, 1, -1},    {0x1D79, 0x1D79, 1, 35332},  {0x1D7D, 0x1D7D, 1, 3814},
    {0x1D8E, 0x1D8E, 1, 35384}, {0x1E01, 0x1E95, 2, -1},     {0x1E9B, 0x1E9B, 1, -59},
    {0x1EA1, 0x1EFF, 2, -1},    {0x1F00, 0x1F07, 1, 8},      {0x1F10, 0x1F15, 1, 8},
    {0x1F20, 0x1F27, 1, 8},     {0x1F30, 0x1F37, 1, 8},      {0x1F40, 0x1F45, 1, 8},
    {0x1F51, 0x1F57, 2, 8},     {0x1F60, 0x1F67, 1, 8},      {0x1F70, 0x1F71, 1, 74},
    {0x1F72, 0x1F75, 1, 86},    {0x1F76, 0x1F77, 1, 100},    {0x1F78, 0x1F79, 1, 128},
    {0x1F7A, 0x1F7B, 1, 112},   {0x1F7C, 0x1F7D, 1, 126},    {0x1F80, 0x1F87, 1, 8},
    {0x1F90, 0x1F97, 1, 8},     {0x1FA0, 0x1FA7, 1, 8},      {0x1FB0, 0x1FB1, 1, 8},
    {0x1FB3, 0x1FB3, 1, 9},     {0x1FBE, 0x1FBE, 1, -7205},  {0x1FC3, 0x1FC3, 1, 9},
    {0x1FD0, 0x1FD1, 1, 8},     {0x1FE0, 0x1FE1, 1, 8},      {0x1FE5, 0x1FE5, 1, 7},
    {0x1FF3, 0x1FF3, 1, 9},     {0x214E, 0x214E, 1, -28},    {0x2170, 0x217F, 1, -16},
    {0x2184, 0x2184, 1, -1},    {0x24D0, 0x24E9, 1, -26},    {0x2C30, 0x2C5F, 1, -48},
    {0x2C61, 0x2C61, 1, -1},    {0x2C65, 0x2C65, 1, -10795}, {0x2C66, 0x2C66, 1, -10792},
    {0x2C68, 0x2C6C, 2, -1},    {0x2C73, 0x2C76, 3, -1},     {0x2C81, 0x2CE3, 2, -1},
    {0x2CEC, 0x2CEE, 2, -1},    {0x2CF3, 0x2CF3, 1, -1},     {0x2D00, 0x2D25, 1, -7264},
    {0x2D27, 0x2D2D, 6, -7264}, {0xA641, 0xA66D, 2, -1},     {0xA681, 0xA69B, 2, -1},
    {0xA723, 0xA72F, 2, -1},    {0xA733, 0xA76F, 2, -1},     {0xA77A, 0xA77C, 2, -1},
    {0xA77F, 0xA787, 2, -1},    {0xA78C, 0xA791, 5, -1},     {0xA793, 0xA793, 1, -1},
    {0xA794, 0xA794, 1, 48},    {0xA797, 0xA7A9, 2, -1},     {0xA7B5, 0xA7C3, 2, -1},
    {0xA7C8, 0xA7CA, 2, -1},    {0xA7CD, 0xA7DB, 2, -1},     {0xA7F6, 0xA7F6, 1, -1},
    {0xAB53, 0xAB53, 1, -928},  {0xAB70, 0xABBF, 1, -38864}, {0xFF41, 0xFF5A, 1, -32},
};

/** Return the simple uppercase of a BMP code point, or the point itself. */
static unsigned long upper(unsigned long c)
{
    unsigned int i;

    for (i = 0; i < sizeof(uppercase) / sizeof(uppercase[0]); i++) {
        const struct case_range *r = &uppercase[i];
        if (c < r->first) {
            break;
        }
        if (c <= r->last && (c - r->first) % r->step == 0) {
            return (unsigned long)((long)c + r->delta);
        }
    }
    return c;
}

/** Return the held or sticky modifiers, inverted by the modifier locks. */
static unsigned int table_index(const struct keymap_state *st)
{
    return (st->shift | st->slocks) ^ st->locks;
}

/** Return xterm modifiers; ShiftL's layout lock does not mean Shift. */
static unsigned int modifiers(const struct keymap_state *st)
{
    unsigned int bits = table_index(st);
    unsigned int mods = 0;

    if ((bits & (1U << KM_G_SHIFT)) || (st->shift & ((1U << KM_G_SHIFTL) | (1U << KM_G_SHIFTR)))) {
        mods |= KM_MOD_SHIFT;
    }
    if (bits & ((1U << KM_G_CTRL) | (1U << KM_G_CTRLL) | (1U << KM_G_CTRLR))) {
        mods |= KM_MOD_CTRL;
    }
    if (bits & (1U << KM_G_ALT)) {
        mods |= KM_MOD_ALT;
    }
    return mods;
}

/** Reset all translation state, keeping only the requested LED lock bits. */
void keymap_state_init(struct keymap_state *st, unsigned int ledflags)
{
    unsigned int i;

    for (i = 0; i < KM_NR_SHIFT; i++) {
        st->down[i] = 0;
    }
    for (i = 0; i < KM_NR_KEYS; i++) {
        st->key_shift[i] = 0;
    }
    for (i = 0; i < sizeof(st->pressed); i++) {
        st->pressed[i] = 0;
    }
    st->shift = st->locks = st->slocks = 0;
    st->ledflags = ledflags & 7;
    st->diacr = 0;
    st->compose = 0;
    st->npad = 0;
    st->npad_on = 0;
}

/** Rebuild the held modifiers from the keys down; keymap.h has the contract. */
void keymap_state_held(struct keymap_state *st, const struct keymap *map, const unsigned char *held)
{
    unsigned int i, entry, type, v;

    for (i = 0; i < KM_NR_SHIFT; i++) {
        st->down[i] = 0;
    }
    st->shift = 0;
    st->npad = 0;
    st->npad_on = 0;
    for (i = 0; i < KM_NR_KEYS; i++) {
        st->key_shift[i] = 0;
        if (held[i >> 3] & (1U << (i & 7))) {
            st->pressed[i >> 3] |= (unsigned char)(1U << (i & 7));
        } else {
            st->pressed[i >> 3] &= (unsigned char)~(1U << (i & 7));
            continue;
        }
        entry = map && map->entry ? map->entry(map->ctx, 0, i) : KM_HOLE;
        type = KM_TYPE(entry);
        v = KM_VALUE(entry);
        if ((type != KM_T_SHIFT && type != KM_T_SLOCK) || v >= KM_NR_SHIFT) {
            continue;
        }
        if (v == KM_G_CAPSSHIFT) {
            v = KM_G_SHIFT;
        }
        st->key_shift[i] = (unsigned char)(v + 1);
        st->down[v]++;
        st->shift |= 1U << v;
    }
}

/**
 * Whether `entry` is an action of Linux's console that Rach2's has not: a
 * switch of virtual console, or a KM_T_SPEC action such as Boot or
 * KeyboardSignal. The shipped layouts put them on Alt with F1 to F12 and the
 * arrows, and on Ctrl+Alt+Delete.
 */
static int foreign_action(unsigned int entry)
{
    if (KM_TYPE(entry) == KM_T_CONS) {
        return 1;
    }
    if (KM_TYPE(entry) != KM_T_SPEC) {
        return 0;
    }
    switch (entry) {
    case KM_HOLE:
    case KM_ENTER:
    case KM_BREAK:
    case KM_CAPS:
    case KM_NUM:
    case KM_HOLD:
    case KM_SCROLLFORW:
    case KM_SCROLLBACK:
    case KM_CAPSON:
    case KM_COMPOSE:
    case KM_BARENUMLOCK:
    case KM_NOSUCHMAP:
        return 0;
    default:
        return 1;
    }
}

/** Compose a character or emit the pending accent and character separately. */
static void character(struct keymap_state *st, const struct keymap *map, unsigned long c,
                      struct keymap_result *res)
{
    unsigned int i;
    unsigned long accent = st->diacr;

    if ((st->compose || accent) && (c == 27 || c == 8 || c == 127)) {
        st->compose = 0;
        st->diacr = 0;
        if (c != 27) {
            return;
        }
    } else if (st->compose) {
        st->compose = 0;
        st->diacr = c;
        return;
    } else if (accent) {
        st->diacr = 0;
        for (i = 0; map->diacr && i < map->ndiacr && i < KM_MAX_DIACR; i++) {
            if (map->diacr[i].diacr == accent && map->diacr[i].base == c) {
                c = map->diacr[i].result;
                break;
            }
        }
        if (!map->diacr || i == map->ndiacr || i == KM_MAX_DIACR) {
            if (c == accent || c == ' ') {
                c = accent;
            } else {
                res->text[res->ntext++] = accent;
            }
        }
    }
    res->kind = KM_RESULT_TEXT;
    res->text[res->ntext++] = c;
}

/** Resolve one key event; the public header defines ownership and results. */
int keymap_key(struct keymap_state *st, const struct keymap *map, unsigned int keycode, int value,
               struct keymap_result *res)
{
    static const unsigned long dead[] = {'`',    '\'',   '^',   '~',   '"',   0xB8,  0xAF,
                                         0x2D8,  0x2D9,  0xB0,  0x2DD, 0x2C7, 0x2DB, 0x37A,
                                         0x309B, 0x309C, 0x323, 0x2C0, 0x31B, '/',   0x2BC,
                                         0x2BD,  0x2F5,  0x311, ',',   0xA4,  0x37E};
    unsigned int table, entry, type, v, base, bit, mod, old_leds;
    int was_pressed;

    res->kind = KM_RESULT_NONE;
    res->text[0] = res->text[1] = 0;
    res->ntext = res->func = 0;
    res->key = keycode;
    res->mods = modifiers(st);
    res->ledflags = st->ledflags;
    res->repeat = value == 2;
    if (keycode >= KM_NR_KEYS || value < 0 || value > 2) {
        return res->kind;
    }
    bit = 1U << (keycode & 7);
    was_pressed = (st->pressed[keycode >> 3] & bit) != 0;
    if (value == 0) {
        st->pressed[keycode >> 3] &= ~bit;
        mod = st->key_shift[keycode];
        st->key_shift[keycode] = 0;
        if (mod) {
            mod--;
            if (st->down[mod] && --st->down[mod] == 0) {
                st->shift &= ~(1U << mod);
            }
            if (st->npad_on && (mod == KM_G_ALT || mod == KM_G_ALTGR) &&
                !(st->shift & ((1U << KM_G_ALT) | (1U << KM_G_ALTGR)))) {
                character(st, map, st->npad, res);
                st->npad_on = 0;
                st->npad = 0;
                res->mods = 0;
            }
        }
        return res->kind;
    }
    if (value == 2 && !was_pressed) {
        return res->kind;
    }
    st->pressed[keycode >> 3] |= bit;
    if (was_pressed && st->key_shift[keycode]) {
        return res->kind;
    }
    if (!map || !map->entry) {
        return res->kind;
    }
    table = table_index(st);
    entry = table < KM_MAX_TABLES ? map->entry(map->ctx, table, keycode) : KM_NOSUCHMAP;
    if (entry == KM_NOSUCHMAP) {
        base = map->entry(map->ctx, 0, keycode);
        type = KM_TYPE(base);
        if (type != KM_T_SHIFT && type != KM_T_SLOCK && type != KM_T_LOCK) {
            return res->kind;
        }
        entry = base;
    }
    if (foreign_action(entry)) {
        base = map->entry(map->ctx, 0, keycode);
        type = KM_TYPE(base);
        entry = type == KM_T_SHIFT || type == KM_T_SLOCK || type == KM_T_LOCK ? KM_HOLE : base;
    }
    type = KM_TYPE(entry);
    v = KM_VALUE(entry);
    old_leds = st->ledflags;
    if (type == KM_T_SHIFT || type == KM_T_SLOCK || type == KM_T_LOCK) {
        if (v >= KM_NR_SHIFT || value != 1 || was_pressed) {
            return res->kind;
        }
        if (v == KM_G_CAPSSHIFT) {
            v = KM_G_SHIFT;
            st->ledflags &= ~KM_LED_CAPS;
        }
        if (type != KM_T_LOCK) {
            st->key_shift[keycode] = v + 1;
            st->down[v]++;
            st->shift |= 1U << v;
        }
        if (type == KM_T_LOCK) {
            st->locks ^= 1U << v;
            res->kind = KM_RESULT_LOCKS;
        } else if (type == KM_T_SLOCK) {
            st->slocks ^= 1U << v;
            res->kind = KM_RESULT_LOCKS;
        }
    } else {
        if ((st->ledflags & KM_LED_CAPS) && (type == KM_T_LETTER || KM_IS_U(entry))) {
            base = table < KM_MAX_TABLES ? map->entry(map->ctx, table ^ (1U << KM_G_SHIFT), keycode)
                                         : KM_NOSUCHMAP;
            if (base != KM_NOSUCHMAP &&
                (type == KM_T_LETTER ||
                 (KM_IS_U(base) && KM_U(base) != KM_U(entry) &&
                  (upper(KM_U(base)) == KM_U(entry) || upper(KM_U(entry)) == KM_U(base))))) {
                entry = base;
                type = KM_TYPE(entry);
                v = KM_VALUE(entry);
            }
        }
        if (KM_IS_U(entry)) {
            character(st, map, KM_U(entry), res);
        } else if (type == KM_T_LATIN || type == KM_T_LETTER || type == KM_T_META) {
            if (type == KM_T_META) {
                res->mods |= KM_MOD_ALT;
            }
            character(st, map, v, res);
        } else if (type == KM_T_DEAD || type == KM_T_DEAD2) {
            unsigned long accent;
            if (type == KM_T_DEAD && v >= sizeof(dead) / sizeof(dead[0])) {
                return res->kind;
            }
            accent = type == KM_T_DEAD ? dead[v] : v;
            if (st->diacr) {
                character(st, map, accent, res);
                if (res->ntext == 2) {
                    res->ntext = 1;
                    st->diacr = accent;
                }
            } else {
                st->diacr = accent;
                st->compose = 0;
            }
        } else if (type == KM_T_FN) {
            if ((v == 24 || v == 25) && (res->mods & KM_MOD_SHIFT)) {
                res->kind = KM_RESULT_SPECIAL;
                res->key = v == 24 ? KM_SCROLLBACK : KM_SCROLLFORW;
            } else {
                res->kind = KM_RESULT_FUNC;
                res->func = v;
            }
        } else if (type == KM_T_CUR || type == KM_T_PAD) {
            res->kind = KM_RESULT_SPECIAL;
            res->key = entry;
        } else if (type == KM_T_ASCII && v < 26) {
            unsigned int radix = v < 10 ? 10 : 16;
            st->npad = st->npad * radix + (v < 10 ? v : v - 10);
            if (st->npad > 0x10FFFF) {
                st->npad = 0x110000;
            }
            st->npad_on = 1;
        } else if (type == KM_T_SPEC) {
            if (entry == KM_ENTER && (st->compose || st->diacr)) {
                character(st, map, '\r', res);
            } else if (entry == KM_ENTER || entry == KM_BREAK || entry == KM_SCROLLFORW ||
                       entry == KM_SCROLLBACK) {
                res->kind = KM_RESULT_SPECIAL;
                res->key = entry;
            } else if (value == 1 && !was_pressed) {
                if (entry == KM_CAPS) {
                    st->ledflags ^= KM_LED_CAPS;
                } else if (entry == KM_CAPSON) {
                    st->ledflags |= KM_LED_CAPS;
                } else if (entry == KM_NUM || entry == KM_BARENUMLOCK) {
                    st->ledflags ^= KM_LED_NUM;
                } else if (entry == KM_HOLD) {
                    st->ledflags ^= KM_LED_SCROLL;
                } else if (entry == KM_COMPOSE) {
                    st->diacr = 0;
                    st->compose = 1;
                }
            }
        }
        if (entry != KM_HOLE && entry != KM_NOSUCHMAP) {
            st->slocks = 0;
        }
    }
    if (st->ledflags != old_leds && res->kind == KM_RESULT_NONE) {
        res->kind = KM_RESULT_LOCKS;
    }
    res->ledflags = st->ledflags;
    return res->kind;
}

/** Read a bounded table entry; a missing table differs from a hole. */
unsigned int keymap_default_entry(const void *ctx, unsigned int table, unsigned int keycode)
{
    const unsigned short *const *tables = (const unsigned short *const *)ctx;

    if (keycode >= KM_NR_KEYS) {
        return KM_HOLE;
    }
    if (!tables || table >= KM_MAX_TABLES || !tables[table]) {
        return KM_NOSUCHMAP;
    }
    return tables[table][keycode];
}

/** Append one Unicode scalar as UTF-8; return zero for an invalid scalar. */
static unsigned int utf8(unsigned long c, char *buf)
{
    if (c <= 0x7F) {
        buf[0] = (char)c;
        return 1;
    }
    if (c <= 0x7FF) {
        buf[0] = (char)(0xC0 | (c >> 6));
        buf[1] = (char)(0x80 | (c & 63));
        return 2;
    }
    if (c >= 0xD800 && c <= 0xDFFF) {
        return 0;
    }
    if (c <= 0xFFFF) {
        buf[0] = (char)(0xE0 | (c >> 12));
        buf[1] = (char)(0x80 | ((c >> 6) & 63));
        buf[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    if (c <= 0x10FFFF) {
        buf[0] = (char)(0xF0 | (c >> 18));
        buf[1] = (char)(0x80 | ((c >> 12) & 63));
        buf[2] = (char)(0x80 | ((c >> 6) & 63));
        buf[3] = (char)(0x80 | (c & 63));
        return 4;
    }
    return 0;
}

/** Return xterm's traditional control character for an ASCII key. */
static unsigned long control(unsigned long c)
{
    if (c >= '@' && c <= '~') {
        return c & 31;
    }
    switch (c) {
    case ' ':
    case '2':
        return 0;
    case '3':
        return 27;
    case '4':
        return 28;
    case '5':
        return 29;
    case '6':
        return 30;
    case '7':
    case '/':
        return 31;
    case '8':
    case '?':
        return 127;
    default:
        return c;
    }
}

/** Append a decimal parameter of at most two digits. */
static unsigned int decimal(unsigned int v, char *buf)
{
    unsigned int n = 0;

    if (v >= 10) {
        buf[n++] = (char)('0' + v / 10);
    }
    buf[n++] = (char)('0' + v % 10);
    return n;
}

/** Encode a cursor, function or editing key with an optional modifier. */
static unsigned int sequence(char final, unsigned int number, int application, unsigned int mods,
                             char *buf)
{
    unsigned int n = 0;

    buf[n++] = 27;
    buf[n++] = application && !mods && !number ? 'O' : '[';
    if (number || mods) {
        n += decimal(number ? number : 1, buf + n);
    }
    if (mods) {
        buf[n++] = ';';
        n += decimal((mods & 15) + 1, buf + n);
    }
    buf[n++] = final;
    return n;
}

/** Copy a function string, adding modifiers only to a single key sequence. */
static unsigned int function_string(const char *s, unsigned int mods, unsigned int modes, char *buf)
{
    unsigned int n = 0, i, number = 0;
    char final;

    if (!s) {
        return 0;
    }
    while (n < KM_FUNC_BYTES && s[n]) {
        n++;
    }
    if (n == KM_FUNC_BYTES) {
        return 0;
    }
    if (n >= 3 && s[0] == 27 && (s[1] == '[' || s[1] == 'O')) {
        final = s[n - 1];
        for (i = 2; i < n - 1 && s[i] >= '0' && s[i] <= '9'; i++) {
            number = number * 10 + (unsigned int)(s[i] - '0');
            if (number > 99) {
                break;
            }
        }
        if (i == n - 1 && ((n == 3 && final >= 'A' && final <= 'Z') ||
                           (s[1] == '[' && n > 3 && number && final == '~'))) {
            return sequence(final, number,
                            s[1] == 'O' ||
                                ((final == 'H' || final == 'F') && (modes & KM_MODE_CURSOR)),
                            mods, buf);
        }
    }
    for (i = 0; i < n; i++) {
        buf[i] = s[i];
    }
    return n;
}

/** Encode a keypad key, using Num Lock, Shift and the application's mode. */
static unsigned int keypad(const struct keymap_result *res, unsigned int modes, char *buf)
{
    static const char numeric[] = "0123456789+-*/\r,.?()";
    static const char application[] = "pqrstuvwxykmjoMln";
    static const char navigation[] = "~FB~DECHA~";
    static const unsigned char parameter[] = {2, 0, 0, 6, 0, 0, 0, 0, 0, 5};
    unsigned int v = KM_VALUE(res->key), mods = res->mods & 15;
    int num = (res->ledflags & KM_LED_NUM) != 0;
    unsigned long c;

    if (v >= sizeof(numeric) - 1) {
        return 0;
    }
    if (mods & KM_MOD_SHIFT) {
        num = !num;
    }
    if (!num && (v == 15 || v == 16)) {
        return sequence('~', 3, 0, mods, buf);
    }
    if (!num && v <= 9) {
        return sequence(navigation[v], parameter[v], (modes & KM_MODE_CURSOR) != 0, mods, buf);
    }
    if ((modes & KM_MODE_KEYPAD) && !(res->ledflags & KM_LED_NUM) && v < sizeof(application) - 1) {
        unsigned int n = 2;
        buf[0] = 27;
        buf[1] = 'O';
        if (mods) {
            n += decimal(mods + 1, buf + n);
        }
        buf[n++] = application[v];
        return n;
    }
    c = v == 17 ? 0xB1 : (unsigned char)numeric[v];
    return utf8(c, buf);
}

/** Encode a whole result into temporary storage before changing the buffer. */
unsigned int keymap_encode(const struct keymap *map, const struct keymap_result *res,
                           unsigned int modes, char *buf, unsigned int size)
{
    char tmp[KM_FUNC_BYTES + 16];
    unsigned int n = 0, i, count, mods = res->mods & 15;
    unsigned long c;

    if (!buf || !size) {
        return 0;
    }
    if (res->kind == KM_RESULT_TEXT) {
        if (!res->ntext || res->ntext > 2) {
            return 0;
        }
        if (res->ntext == 1 && res->text[0] == '\t' && (mods & KM_MOD_SHIFT) &&
            (!(mods & KM_MOD_CTRL) || res->key == 15)) {
            tmp[n++] = 27;
            tmp[n++] = '[';
            tmp[n++] = 'Z';
        } else {
            if (mods & (KM_MOD_ALT | KM_MOD_META)) {
                tmp[n++] = 27;
            }
            for (i = 0; i < res->ntext; i++) {
                c = res->text[i];
                if (mods & KM_MOD_CTRL) {
                    c = control(c);
                }
                count = utf8(c, tmp + n);
                if (!count) {
                    return 0;
                }
                n += count;
            }
        }
    } else if (res->kind == KM_RESULT_FUNC) {
        if ((res->func == 24 || res->func == 25) && (mods & KM_MOD_SHIFT)) {
            return 0;
        }
        if (map && map->func && res->func < KM_MAX_FUNC) {
            n = function_string(map->func[res->func], mods, modes, tmp);
        }
    } else if (res->kind == KM_RESULT_SPECIAL) {
        if (KM_TYPE(res->key) == KM_T_CUR && KM_VALUE(res->key) < 4) {
            n = sequence("DBCA"[KM_VALUE(res->key)], 0, (modes & KM_MODE_CURSOR) != 0, mods, tmp);
        } else if (KM_TYPE(res->key) == KM_T_PAD) {
            n = keypad(res, modes, tmp);
        } else if (res->key == KM_ENTER) {
            if (mods & (KM_MOD_ALT | KM_MOD_META)) {
                tmp[n++] = 27;
            }
            tmp[n++] = '\r';
        }
    }
    if (n > size) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        buf[i] = tmp[i];
    }
    return n;
}
