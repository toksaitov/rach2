/*
 * fb.h: the graphics modes a program draws in through /fb, mode 13h and the
 * framebuffer's native mode, the record of the mode in force, and the /fb
 * device's operations.
 */

#ifndef _FB_H_
#define _FB_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/* The requests, structures and events programs use, which they include as
 * <rach2/fb.h>. */
#include "../../include/rach2/fb.h"

struct file_ops;
struct fd_entry;

/* Graphics mode identifiers. */
typedef enum {
    GFX_MODE_NONE = 0, /* No graphics mode active (text mode) */
    GFX_MODE_VGA_13H,  /* VGA Mode 13h: 320x200, 256 colors */
    GFX_MODE_NATIVE    /* The framebuffer itself, at its own size and format */
} gfx_mode_t;

/* Current graphics mode and the location of its framebuffer. */
typedef struct {
    gfx_mode_t mode;      /* Current graphics mode */
    uint32_t width;       /* Screen width in pixels */
    uint32_t height;      /* Screen height in pixels */
    uint32_t bpp;         /* Bits per pixel, 8 in mode 13h */
    uint32_t pitch;       /* Bytes per scanline */
    uint8_t *framebuffer; /* The pixels `fb_enter` or `fb_enter_native` returned */
    uint32_t size;        /* Framebuffer size in bytes: pitch times height */
} gfx_info_t;

/** Live framebuffer state, set by drivers entering or leaving a mode. */
extern gfx_info_t gfx_current;

/* True when a graphics mode is active. */
#define gfx_is_active() (gfx_current.mode != GFX_MODE_NONE)

/*
 * The graphics modes the /fb device draws in, which the platform's display
 * driver defines (display/vga_gfx.c on a PC): VGA mode 13h, 320x200 at a
 * byte a pixel through a palette of 256 colors, and on a framebuffer console
 * the native mode, the framebuffer at its own size and pixel format. Each
 * sets `gfx_current`, and each runs with interrupts off.
 */

/**
 * Enter mode 13h, with its pixels cleared to color 0, and return them: on a
 * VGA the adapter's window at 0xA0000, with the text font saved for
 * `fb_exit`; on a framebuffer console memory of the kernel's, which
 * `fb_present` draws, with the screen given to the picture, from the text or
 * the native mode alike. In mode 13h already, it returns the same pixels and
 * changes nothing.
 */
uint8_t *fb_enter(void);

/**
 * Enter the native mode and return the framebuffer's first pixel, the
 * screen painted black and no cell drawn while the mode lasts; from mode 13h
 * it goes straight there. In the mode already, it returns the same pixels.
 * Returns NULL, with the mode unchanged, when the console is on no
 * framebuffer or on one that is not RGB at 15, 16, 24 or 32 bits a pixel.
 */
uint8_t *fb_enter_native(void);

/** Describe the framebuffer in `*s` as FB_GETINFO reports it, whatever the
 * mode; 0, or -1 when the console is on the VGA's text mode. */
int fb_screen(struct fb_screen *s);

/** Leave the graphics mode, either, and return to the text console,
 * redrawn: on a VGA mode 3, with its palette and the saved font loaded
 * again. Out of the modes, it does nothing. */
void fb_exit(void);

/** Load mode 13h's palette of 256 RGB entries, three bytes each of 0 to
 * 255, from the kernel's memory; the VGA keeps the top six bits of each.
 * NULL is ignored. */
void fb_set_palette(const uint8_t *data);

/** Show the picture written to mode 13h's pixels, on a framebuffer console,
 * where the VGA's memory reaches no screen; nothing on a VGA or at the
 * native mode, whose pixels are the screen's. */
void fb_present(void);

/*
 * The /fb device, display/fbdev.c: the descriptor an open of "fb" makes
 * (`open_framebuffer` in syscall_fs.c) has these operations. One process at
 * a time owns the display, by the thread group id in `fb_owner_pid`
 * (syscall_internal.h), 0 for none.
 */

/** The operations of a /fb descriptor: event reads, writes, poll, ioctl
 * and the last close. */
extern const struct file_ops fb_file_ops;

/** Attach the open description `e` and make the calling process the
 * display's owner, at mode 13h, the console unblanked first; 0, -ENOMEM, or -EBUSY for a caller
 * outside the current owner's descendants. Same-owner opens share its ownership. */
int32_t fb_take(struct fd_entry *e);

/**
 * The size in bytes of the mode in force, which an lseek(2) of /fb with
 * SEEK_END counts from and past which a write is refused: 64000 in mode 13h,
 * the pitch times the height at the native mode, and 0 in text mode.
 */
uint32_t fb_size(void);

/** Release the stack entry for thread group `pid`, active or suspended,
 * before its process record can be reaped. Process context, big lock held. */
void fb_release_if_owner(uint32_t pid);

#endif /* _FB_H_ */
