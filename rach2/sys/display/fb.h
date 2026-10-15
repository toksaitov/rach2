/*
 * fb.h: the graphics mode a program draws in through /fb, and the record of
 * the mode in force. VGA mode 13h is the only mode; the enum keeps a value
 * for VESA's modes, which nothing sets.
 */

#ifndef _FB_H_
#define _FB_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/* Graphics mode identifiers. */
typedef enum {
    GFX_MODE_NONE = 0, /* No graphics mode active (text mode) */
    GFX_MODE_VGA_13H,  /* VGA Mode 13h: 320x200, 256 colors */
    GFX_MODE_VBE       /* Reserved for VESA/VBE high-resolution modes */
} gfx_mode_t;

/* Current graphics mode and the location of its framebuffer. */
typedef struct {
    gfx_mode_t mode;      /* Current graphics mode */
    uint32_t width;       /* Screen width in pixels */
    uint32_t height;      /* Screen height in pixels */
    uint32_t bpp;         /* Bits per pixel, 8 in mode 13h */
    uint32_t pitch;       /* Bytes per scanline */
    uint8_t *framebuffer; /* The pixels `fb_enter` returned */
    uint32_t size;        /* Framebuffer size in bytes */
} gfx_info_t;

/** Live framebuffer state, set by drivers entering or leaving a mode. */
extern gfx_info_t gfx_current;

/* True when a graphics mode is active. */
#define gfx_is_active() (gfx_current.mode != GFX_MODE_NONE)

/*
 * The graphics mode the /fb device draws in, which the platform's display
 * driver defines (display/vga_gfx.c on a PC, VGA mode 13h: 320x200 at a byte
 * a pixel through a palette of 256 colors). Each sets `gfx_current`, and
 * each runs with interrupts off.
 */

/**
 * Enter the graphics mode, with its pixels cleared to color 0, and return
 * them: on a VGA the adapter's window at 0xA0000, with the text font saved
 * for `fb_exit`; on a framebuffer console memory of the kernel's, which
 * `fb_present` draws, with the screen given to the picture. In the mode
 * already, it returns the same pixels and changes nothing.
 */
uint8_t *fb_enter(void);

/** Leave the graphics mode and return to the text console, cleared: on a VGA
 * mode 3, with its palette and the saved font loaded again. Out of the mode,
 * it does nothing. */
void fb_exit(void);

/** Load a palette of 256 RGB entries, three bytes each of 0 to 255, from the
 * kernel's memory; the VGA keeps the top six bits of each. NULL is ignored. */
void fb_set_palette(const uint8_t *data);

/** Show the picture written to the framebuffer, on a framebuffer console,
 * where the VGA's memory reaches no screen; nothing on a VGA. */
void fb_present(void);

#endif /* _FB_H_ */
