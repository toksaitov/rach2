/*
 * vga_gfx.h: the VGA's graphics mode 13h, 320x200 in 256 colors, which
 * vga_gfx.c gives /fb through fb.h, and the VGA's registers.
 */

#ifndef _VGA_GFX_H_
#define _VGA_GFX_H_

#include "fb.h"

/* VGA Mode 13h constants (320x200, 256 colors). */
#define VGA_GFX_WIDTH       320
#define VGA_GFX_HEIGHT      200
#define VGA_GFX_BPP         8     /* 8 bits per pixel (palette index) */
#define VGA_GFX_SIZE        64000 /* 320 * 200 bytes */
/* The kernel's address of the adapter's window at physical 0xA0000. */
#define VGA_GFX_FRAMEBUFFER phys_to_virt(0xA0000)

/* The same constants under a second set of names. */
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

/** True while the graphics mode is in force, on the VGA or on a framebuffer
 * console. */
extern bool vga_graphics_mode;

#endif /* _VGA_GFX_H_ */
