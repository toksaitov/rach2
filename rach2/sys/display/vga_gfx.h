/*
 * VGA graphics mode driver (Mode 13h, 320x200, 256 colors).
 *
 * Implements the common framebuffer interface defined in fb.h.
 */

#ifndef _VGA_GFX_H_
#define _VGA_GFX_H_

#include "fb.h"

/* VGA Mode 13h constants (320x200, 256 colors). */
#define VGA_GFX_WIDTH       320
#define VGA_GFX_HEIGHT      200
#define VGA_GFX_BPP         8     /* 8 bits per pixel (palette index) */
#define VGA_GFX_SIZE        64000 /* 320 * 200 bytes */
#define VGA_GFX_FRAMEBUFFER 0xA0000

/* Legacy macro names kept for compatibility. */
#define VGA_MODE13H_WIDTH  VGA_GFX_WIDTH
#define VGA_MODE13H_HEIGHT VGA_GFX_HEIGHT
#define VGA_MODE13H_BPP    1 /* 1 byte per pixel */
#define VGA_MODE13H_SIZE   VGA_GFX_SIZE
#define VGA_FRAMEBUFFER    VGA_GFX_FRAMEBUFFER

/* VGA I/O ports (shared between text and graphics modes). */
#define VGA_MISC_WRITE      0x3C2
#define VGA_MISC_READ       0x3CC
#define VGA_SEQ_INDEX       0x3C4
#define VGA_SEQ_DATA        0x3C5
#define VGA_CRTC_INDEX      0x3D4
#define VGA_CRTC_DATA       0x3D5
#define VGA_GC_INDEX        0x3CE
#define VGA_GC_DATA         0x3CF
#define VGA_AC_INDEX        0x3C0
#define VGA_AC_WRITE        0x3C0
#define VGA_AC_READ         0x3C1
#define VGA_INSTAT_READ     0x3DA
#define VGA_DAC_WRITE_INDEX 0x3C8
#define VGA_DAC_DATA        0x3C9

/** True while the VGA is in graphics mode. */
extern bool vga_graphics_mode;

/** Enter Mode 13h; returns the framebuffer at 0xA0000. */
uint8_t *vga_mode13h_enter(void);

/** Leave Mode 13h and return to text mode (Mode 3). */
void vga_mode13h_exit(void);

/** Load a 256-entry RGB palette into the DAC. */
void vga_mode13h_set_palette(const uint8_t *data);

/** Return a pointer to the Mode 13h framebuffer at 0xA0000. */
uint8_t *vga_mode13h_get_framebuffer(void);

/** Copy a full VGA_GFX_SIZE frame to the framebuffer. */
void vga_mode13h_blit(const uint8_t *src);

#endif /* _VGA_GFX_H_ */
