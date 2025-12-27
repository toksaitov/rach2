/*
 * fb.h - Common framebuffer interface
 *
 * Provides a unified interface for graphics framebuffer modes.
 * Currently supports VGA Mode 13h, designed to be extensible for
 * future VESA/VBE support.
 */

#ifndef _FB_H_
#define _FB_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/*
 * Graphics mode identifiers
 */
typedef enum {
    GFX_MODE_NONE = 0,      /* No graphics mode active (text mode) */
    GFX_MODE_VGA_13H,       /* VGA Mode 13h: 320x200, 256 colors */
    GFX_MODE_VBE            /* Future: VESA/VBE high-resolution modes */
} gfx_mode_t;

/*
 * Framebuffer information structure
 * Describes the current graphics mode and framebuffer location
 */
typedef struct {
    gfx_mode_t mode;        /* Current graphics mode */
    uint32_t width;         /* Screen width in pixels */
    uint32_t height;        /* Screen height in pixels */
    uint32_t bpp;           /* Bits per pixel (8, 16, 24, 32) */
    uint32_t pitch;         /* Bytes per scanline */
    uint8_t *framebuffer;   /* Pointer to framebuffer memory */
    uint32_t size;          /* Framebuffer size in bytes */
} gfx_info_t;

/*
 * Global framebuffer state
 * Set by graphics drivers when entering a graphics mode
 */
extern gfx_info_t gfx_current;

/*
 * Check if a graphics mode is currently active
 */
#define gfx_is_active() (gfx_current.mode != GFX_MODE_NONE)

#endif /* _FB_H_ */
