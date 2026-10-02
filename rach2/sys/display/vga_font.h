/*
 * The console's font: the upper half of code page 437 with the Cyrillic
 * letters in place of its Latin and Greek ones, which the kernel loads into
 * the VGA adapter's character memory at boot.
 */

#ifndef _VGA_FONT_H_
#define _VGA_FONT_H_

#include "../lib/kint.h"

/** The number of glyphs the font loads, the characters it adds. */
#define VGA_FONT_GLYPHS 98

/** A glyph to load: its character code and its 16 rows of 8 pixels. */
struct vga_glyph {
    uint8_t slot;
    uint8_t rows[16];
};

/** The Unicode value of each character from 0x80 to 0xFF of the font. */
extern const uint16_t vga_font_map[128];

/** The glyphs the font loads, from GNU Unifont. */
extern const struct vga_glyph vga_font_glyphs[VGA_FONT_GLYPHS];

/**
 * Load the font's glyphs into the adapter's character memory and read them
 * back; return 1 when every byte reads back as written, and 0 otherwise or
 * while the adapter is in a graphics mode.
 */
int vga_font_load(void);

#endif /* _VGA_FONT_H_ */
