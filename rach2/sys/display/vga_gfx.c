/*
 * vga_gfx.c - VGA Graphics Mode driver (Mode 13h) implementation
 */

#include "vga_gfx.h"
#include "vga_text.h"
#include "../kio.h"
#include "../serial.h"
#include "../lib/kstring.h"

bool vga_graphics_mode = false;

/*
 * Global framebuffer state (from fb.h)
 * Updated when entering/exiting graphics mode
 */
gfx_info_t gfx_current = {
    .mode = GFX_MODE_NONE,
    .width = 0,
    .height = 0,
    .bpp = 0,
    .pitch = 0,
    .framebuffer = NULL,
    .size = 0
};

/*
 * Saved VGA font data (8x16 font, 256 characters = 4KB)
 * Must be saved before entering Mode 13h and restored on exit
 */
#define VGA_FONT_SIZE (256 * 16)
static uint8_t saved_font[VGA_FONT_SIZE];

/*
 * VGA Mode 13h register values (320x200, 256 colors, linear framebuffer)
 * These are the standard values for BIOS mode 0x13
 */

/* Miscellaneous Output Register */
static const uint8_t mode13h_misc = 0x63;

/* Sequencer registers (index 0-4) */
static const uint8_t mode13h_seq[] = {
    0x03,  /* Reset */
    0x01,  /* Clocking Mode */
    0x0F,  /* Map Mask */
    0x00,  /* Character Map Select */
    0x0E   /* Memory Mode */
};

/* CRTC registers (index 0-24) */
static const uint8_t mode13h_crtc[] = {
    0x5F,  /* Horizontal Total */
    0x4F,  /* Horizontal Display End */
    0x50,  /* Start Horizontal Blanking */
    0x82,  /* End Horizontal Blanking */
    0x54,  /* Start Horizontal Retrace */
    0x80,  /* End Horizontal Retrace */
    0xBF,  /* Vertical Total */
    0x1F,  /* Overflow */
    0x00,  /* Preset Row Scan */
    0x41,  /* Maximum Scan Line */
    0x00,  /* Cursor Start */
    0x00,  /* Cursor End */
    0x00,  /* Start Address High */
    0x00,  /* Start Address Low */
    0x00,  /* Cursor Location High */
    0x00,  /* Cursor Location Low */
    0x9C,  /* Vertical Retrace Start */
    0x0E,  /* Vertical Retrace End */
    0x8F,  /* Vertical Display End */
    0x28,  /* Offset */
    0x40,  /* Underline Location */
    0x96,  /* Start Vertical Blanking */
    0xB9,  /* End Vertical Blanking */
    0xA3,  /* CRTC Mode Control */
    0xFF   /* Line Compare */
};

/* Graphics Controller registers (index 0-8) */
static const uint8_t mode13h_gc[] = {
    0x00,  /* Set/Reset */
    0x00,  /* Enable Set/Reset */
    0x00,  /* Color Compare */
    0x00,  /* Data Rotate */
    0x00,  /* Read Map Select */
    0x40,  /* Graphics Mode */
    0x05,  /* Miscellaneous */
    0x0F,  /* Color Don't Care */
    0xFF   /* Bit Mask */
};

/* Attribute Controller registers (index 0-20) */
static const uint8_t mode13h_ac[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41,  /* Attribute Mode Control */
    0x00,  /* Overscan Color */
    0x0F,  /* Color Plane Enable */
    0x00,  /* Horizontal Pixel Panning */
    0x00   /* Color Select */
};

/*
 * VGA Text Mode 3 register values (80x25, 16 colors)
 */

static const uint8_t mode3_misc = 0x67;

static const uint8_t mode3_seq[] = {
    0x03, 0x00, 0x03, 0x00, 0x02
};

static const uint8_t mode3_crtc[] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF
};

static const uint8_t mode3_gc[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
};

static const uint8_t mode3_ac[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};

/*
 * Write a value to a VGA sequencer register
 */
static void vga_seq_write(uint8_t index, uint8_t value)
{
    outb(VGA_SEQ_INDEX, index);
    outb(VGA_SEQ_DATA, value);
}

/*
 * Write a value to a VGA CRTC register
 */
static void vga_crtc_write(uint8_t index, uint8_t value)
{
    outb(VGA_CRTC_INDEX, index);
    outb(VGA_CRTC_DATA, value);
}

/*
 * Write a value to a VGA Graphics Controller register
 */
static void vga_gc_write(uint8_t index, uint8_t value)
{
    outb(VGA_GC_INDEX, index);
    outb(VGA_GC_DATA, value);
}

/*
 * Write a value to a VGA Attribute Controller register
 */
static void vga_ac_write(uint8_t index, uint8_t value)
{
    /* Read input status to reset AC flip-flop */
    (void)inb(VGA_INSTAT_READ);
    /* Write index (with bit 5 set to keep video enabled) */
    outb(VGA_AC_INDEX, index);
    /* Write value */
    outb(VGA_AC_WRITE, value);
}

/*
 * Unlock CRTC registers by clearing the protect bit in register 0x11
 */
static void vga_crtc_unlock(void)
{
    uint8_t val;
    outb(VGA_CRTC_INDEX, 0x11);
    val = inb(VGA_CRTC_DATA);
    outb(VGA_CRTC_DATA, val & 0x7F);  /* Clear protect bit */
}

/*
 * Apply a set of VGA register values
 */
static void vga_set_mode(uint8_t misc,
                         const uint8_t *seq, size_t seq_count,
                         const uint8_t *crtc, size_t crtc_count,
                         const uint8_t *gc, size_t gc_count,
                         const uint8_t *ac, size_t ac_count)
{
    size_t i;

    /* Write Miscellaneous Output Register */
    outb(VGA_MISC_WRITE, misc);

    /* Write Sequencer registers */
    for (i = 0; i < seq_count; i++) {
        vga_seq_write(i, seq[i]);
    }

    /* Unlock CRTC registers */
    vga_crtc_unlock();

    /* Write CRTC registers */
    for (i = 0; i < crtc_count; i++) {
        vga_crtc_write(i, crtc[i]);
    }

    /* Write Graphics Controller registers */
    for (i = 0; i < gc_count; i++) {
        vga_gc_write(i, gc[i]);
    }

    /* Write Attribute Controller registers */
    for (i = 0; i < ac_count; i++) {
        vga_ac_write(i, ac[i]);
    }

    /* Re-enable video output */
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x20);
}

/*
 * Save VGA font from plane 2 (character generator RAM)
 */
static void vga_save_font(void)
{
    volatile uint8_t *font_mem = (volatile uint8_t *)0xA0000;
    int i;

    /* Set sequencer to access plane 2 */
    outb(VGA_SEQ_INDEX, 0x02);  /* Map Mask */
    outb(VGA_SEQ_DATA, 0x04);   /* Enable plane 2 only */

    outb(VGA_SEQ_INDEX, 0x04);  /* Memory Mode */
    outb(VGA_SEQ_DATA, 0x06);   /* Sequential, Extended memory, Odd/Even disabled */

    /* Set graphics controller for plane 2 read */
    outb(VGA_GC_INDEX, 0x04);   /* Read Map Select */
    outb(VGA_GC_DATA, 0x02);    /* Read from plane 2 */

    outb(VGA_GC_INDEX, 0x05);   /* Graphics Mode */
    outb(VGA_GC_DATA, 0x00);    /* Read mode 0, write mode 0 */

    outb(VGA_GC_INDEX, 0x06);   /* Miscellaneous */
    outb(VGA_GC_DATA, 0x04);    /* Map at A0000-AFFFF (64K region) */

    /* Copy font data (256 chars * 32 bytes per char, but only first 16 used) */
    for (i = 0; i < 256; i++) {
        k_memcpy(&saved_font[i * 16], (void *)(font_mem + i * 32), 16);
    }
}

/*
 * Restore VGA font to plane 2
 */
static void vga_restore_font(void)
{
    volatile uint8_t *font_mem = (volatile uint8_t *)0xA0000;
    int i;

    /* Set sequencer to write plane 2 */
    outb(VGA_SEQ_INDEX, 0x02);  /* Map Mask */
    outb(VGA_SEQ_DATA, 0x04);   /* Enable plane 2 only */

    outb(VGA_SEQ_INDEX, 0x04);  /* Memory Mode */
    outb(VGA_SEQ_DATA, 0x06);   /* Sequential, Extended memory, Odd/Even disabled */

    /* Set graphics controller for plane 2 write */
    outb(VGA_GC_INDEX, 0x04);   /* Read Map Select */
    outb(VGA_GC_DATA, 0x02);    /* Read from plane 2 */

    outb(VGA_GC_INDEX, 0x05);   /* Graphics Mode */
    outb(VGA_GC_DATA, 0x00);    /* Read mode 0, write mode 0 */

    outb(VGA_GC_INDEX, 0x06);   /* Miscellaneous */
    outb(VGA_GC_DATA, 0x04);    /* Map at A0000-AFFFF (64K region) */

    /* Write font data (each char takes 32 bytes in memory, use first 16) */
    for (i = 0; i < 256; i++) {
        k_memcpy((void *)(font_mem + i * 32), &saved_font[i * 16], 16);
    }

    /* Restore sequencer for text mode */
    outb(VGA_SEQ_INDEX, 0x02);
    outb(VGA_SEQ_DATA, 0x03);   /* Map Mask: planes 0 and 1 */

    outb(VGA_SEQ_INDEX, 0x04);
    outb(VGA_SEQ_DATA, 0x02);   /* Memory Mode: Odd/Even enabled */

    /* Restore graphics controller for text mode */
    outb(VGA_GC_INDEX, 0x04);
    outb(VGA_GC_DATA, 0x00);    /* Read from plane 0 */

    outb(VGA_GC_INDEX, 0x05);
    outb(VGA_GC_DATA, 0x10);    /* Odd/Even mode */

    outb(VGA_GC_INDEX, 0x06);
    outb(VGA_GC_DATA, 0x0E);    /* Map at B8000, text mode */
}

uint8_t *vga_mode13h_enter(void)
{
    if (vga_graphics_mode) {
        return (uint8_t *)VGA_GFX_FRAMEBUFFER;
    }

    /* Save font before switching to graphics mode */
    vga_save_font();

    /* Apply Mode 13h registers */
    vga_set_mode(mode13h_misc,
                 mode13h_seq, sizeof(mode13h_seq),
                 mode13h_crtc, sizeof(mode13h_crtc),
                 mode13h_gc, sizeof(mode13h_gc),
                 mode13h_ac, sizeof(mode13h_ac));

    /* Clear the framebuffer */
    k_memset((void *)VGA_GFX_FRAMEBUFFER, 0, VGA_GFX_SIZE);

    vga_graphics_mode = true;

    /* Update global framebuffer info */
    gfx_current.mode = GFX_MODE_VGA_13H;
    gfx_current.width = VGA_GFX_WIDTH;
    gfx_current.height = VGA_GFX_HEIGHT;
    gfx_current.bpp = VGA_GFX_BPP;
    gfx_current.pitch = VGA_GFX_WIDTH;
    gfx_current.framebuffer = (uint8_t *)VGA_GFX_FRAMEBUFFER;
    gfx_current.size = VGA_GFX_SIZE;

    return (uint8_t *)VGA_GFX_FRAMEBUFFER;
}

/*
 * Standard VGA text mode palette (first 16 colors)
 * These are the classic CGA/EGA colors used in text mode
 */
static void vga_restore_text_palette(void)
{
    /*
     * Text mode uses AC palette registers that map attributes to DAC indices:
     * AC[0-7] = 0x00-0x07 (low intensity colors at DAC 0-7)
     * AC[6] = 0x14 (brown is special, at DAC 20)
     * AC[8-15] = 0x38-0x3F (high intensity colors at DAC 56-63)
     */

    /* Low intensity colors at DAC indices 0-7 */
    static const uint8_t low_colors[8 * 3] = {
        0x00, 0x00, 0x00,  /* 0: Black */
        0x00, 0x00, 0x2A,  /* 1: Blue */
        0x00, 0x2A, 0x00,  /* 2: Green */
        0x00, 0x2A, 0x2A,  /* 3: Cyan */
        0x2A, 0x00, 0x00,  /* 4: Red */
        0x2A, 0x00, 0x2A,  /* 5: Magenta */
        0x2A, 0x15, 0x00,  /* 6: Brown (also at 0x14) */
        0x2A, 0x2A, 0x2A   /* 7: Light Gray */
    };

    /* High intensity colors at DAC indices 0x38-0x3F (56-63) */
    static const uint8_t high_colors[8 * 3] = {
        0x15, 0x15, 0x15,  /* 0x38: Dark Gray */
        0x15, 0x15, 0x3F,  /* 0x39: Light Blue */
        0x15, 0x3F, 0x15,  /* 0x3A: Light Green */
        0x15, 0x3F, 0x3F,  /* 0x3B: Light Cyan */
        0x3F, 0x15, 0x15,  /* 0x3C: Light Red */
        0x3F, 0x15, 0x3F,  /* 0x3D: Light Magenta */
        0x3F, 0x3F, 0x15,  /* 0x3E: Yellow */
        0x3F, 0x3F, 0x3F   /* 0x3F: White */
    };
    int i;

    /* Set DAC indices 0-7 (low intensity colors) */
    outb(VGA_DAC_WRITE_INDEX, 0);
    for (i = 0; i < 8 * 3; i++) {
        outb(VGA_DAC_DATA, low_colors[i]);
    }

    /* Set DAC index 0x14 (20) for brown - AC[6] maps here */
    outb(VGA_DAC_WRITE_INDEX, 0x14);
    outb(VGA_DAC_DATA, 0x2A);  /* R */
    outb(VGA_DAC_DATA, 0x15);  /* G */
    outb(VGA_DAC_DATA, 0x00);  /* B */

    /* Set DAC indices 0x38-0x3F (high intensity colors) */
    outb(VGA_DAC_WRITE_INDEX, 0x38);
    for (i = 0; i < 8 * 3; i++) {
        outb(VGA_DAC_DATA, high_colors[i]);
    }
}

void vga_mode13h_exit(void)
{
    if (!vga_graphics_mode) {
        return;
    }

    /* Apply Mode 3 (text mode) registers */
    vga_set_mode(mode3_misc,
                 mode3_seq, sizeof(mode3_seq),
                 mode3_crtc, sizeof(mode3_crtc),
                 mode3_gc, sizeof(mode3_gc),
                 mode3_ac, sizeof(mode3_ac));

    /* Restore standard text mode palette */
    vga_restore_text_palette();

    /* Restore font to character generator RAM */
    vga_restore_font();

    vga_graphics_mode = false;

    /* Clear global framebuffer info */
    gfx_current.mode = GFX_MODE_NONE;
    gfx_current.width = 0;
    gfx_current.height = 0;
    gfx_current.bpp = 0;
    gfx_current.pitch = 0;
    gfx_current.framebuffer = NULL;
    gfx_current.size = 0;

    /* Clear text mode screen */
    k_cls();
}

void vga_mode13h_set_palette(const uint8_t *data)
{
    int i;

    if (!data) {
        return;
    }

    /* Start writing at palette index 0 */
    outb(VGA_DAC_WRITE_INDEX, 0);

    /* Write 256 colors * 3 components (R, G, B) */
    /* VGA DAC expects 6-bit values (0-63), not 8-bit */
    for (i = 0; i < 768; i++) {
        outb(VGA_DAC_DATA, data[i] >> 2);  /* Convert 8-bit to 6-bit */
    }
}

uint8_t *vga_mode13h_get_framebuffer(void)
{
    return (uint8_t *)VGA_GFX_FRAMEBUFFER;
}

void vga_mode13h_blit(const uint8_t *src)
{
    if (src && vga_graphics_mode) {
        k_memcpy((void *)VGA_GFX_FRAMEBUFFER, src, VGA_GFX_SIZE);
    }
}
