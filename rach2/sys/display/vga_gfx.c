/*
 * VGA Graphics Mode driver (Mode 13h, 320x200, 256 colors).
 */

#include "vga_gfx.h"
#include "vga_text.h"
#include "../kio.h"
#include "../serial.h"
#include "../lib/kstring.h"

bool vga_graphics_mode = false;

/* Live framebuffer state, updated on entry to and exit from graphics mode. */
/* The gfx_info_t fields, in the order fb.h declares them: mode, width,
   height, bpp, pitch, framebuffer, size. */
gfx_info_t gfx_current = {GFX_MODE_NONE, 0, 0, 0, 0, NULL, 0};

/* 8x16 text font (256 chars * 16 bytes), saved before Mode 13h, restored on
 * exit. */
#define VGA_FONT_SIZE (256 * 16)
static uint8_t saved_font[VGA_FONT_SIZE];

/* Standard register values for BIOS Mode 13h: 320x200, 256 colors, linear
 * A0000. */

/* Miscellaneous Output Register */
static const uint8_t mode13h_misc = 0x63;

/* Sequencer registers (index 0-4) */
static const uint8_t mode13h_seq[] = {
    0x03, /* Reset */
    0x01, /* Clocking Mode */
    0x0F, /* Map Mask */
    0x00, /* Character Map Select */
    0x0E  /* Memory Mode */
};

/* CRTC registers (index 0-24) */
static const uint8_t mode13h_crtc[] = {
    0x5F, /* Horizontal Total */
    0x4F, /* Horizontal Display End */
    0x50, /* Start Horizontal Blanking */
    0x82, /* End Horizontal Blanking */
    0x54, /* Start Horizontal Retrace */
    0x80, /* End Horizontal Retrace */
    0xBF, /* Vertical Total */
    0x1F, /* Overflow */
    0x00, /* Preset Row Scan */
    0x41, /* Maximum Scan Line */
    0x00, /* Cursor Start */
    0x00, /* Cursor End */
    0x00, /* Start Address High */
    0x00, /* Start Address Low */
    0x00, /* Cursor Location High */
    0x00, /* Cursor Location Low */
    0x9C, /* Vertical Retrace Start */
    0x0E, /* Vertical Retrace End */
    0x8F, /* Vertical Display End */
    0x28, /* Offset */
    0x40, /* Underline Location */
    0x96, /* Start Vertical Blanking */
    0xB9, /* End Vertical Blanking */
    0xA3, /* CRTC Mode Control */
    0xFF  /* Line Compare */
};

/* Graphics Controller registers (index 0-8) */
static const uint8_t mode13h_gc[] = {
    0x00, /* Set/Reset */
    0x00, /* Enable Set/Reset */
    0x00, /* Color Compare */
    0x00, /* Data Rotate */
    0x00, /* Read Map Select */
    0x40, /* Graphics Mode */
    0x05, /* Miscellaneous */
    0x0F, /* Color Don't Care */
    0xFF  /* Bit Mask */
};

/* Attribute Controller registers (index 0-20) */
static const uint8_t mode13h_ac[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x41, /* Attribute Mode Control */
    0x00,                                           /* Overscan Color */
    0x0F,                                           /* Color Plane Enable */
    0x00,                                           /* Horizontal Pixel Panning */
    0x00                                            /* Color Select */
};

/* Standard register values for BIOS Mode 3: 80x25 text, 16 colors. */

static const uint8_t mode3_misc = 0x67;

static const uint8_t mode3_seq[] = {0x03, 0x00, 0x03, 0x00, 0x02};

static const uint8_t mode3_crtc[] = {0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00,
                                     0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50, 0x9C, 0x0E,
                                     0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF};

static const uint8_t mode3_gc[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF};

static const uint8_t mode3_ac[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A,
                                   0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0x00, 0x0F, 0x08, 0x00};

/** Write a Sequencer register: index to SEQ_INDEX, then value to SEQ_DATA. */
static void vga_seq_write(uint8_t index, uint8_t value)
{
    outb(VGA_SEQ_INDEX, index);
    outb(VGA_SEQ_DATA, value);
}

/** Write a CRTC register: index to CRTC_INDEX, then value to CRTC_DATA. */
static void vga_crtc_write(uint8_t index, uint8_t value)
{
    outb(VGA_CRTC_INDEX, index);
    outb(VGA_CRTC_DATA, value);
}

/** Write a Graphics Controller register: index to GC_INDEX, then value to
 * GC_DATA. */
static void vga_gc_write(uint8_t index, uint8_t value)
{
    outb(VGA_GC_INDEX, index);
    outb(VGA_GC_DATA, value);
}

/**
 * Write an Attribute Controller register.
 *
 * The AC index port is a shared index/data flip-flop. Reading INSTAT_READ
 * resets it to the index phase before the index write. Bit 5 of the index
 * (palette address source) stays clear during register programming, so video
 * is disabled; the mode sequence re-enables it afterward by writing index 0x20.
 */
static void vga_ac_write(uint8_t index, uint8_t value)
{
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, index);
    outb(VGA_AC_WRITE, value);
}

/** Clear the write-protect bit (bit 7) of CRTC register 0x11 so CRTC
 * 0x00-0x07 are writable. */
static void vga_crtc_unlock(void)
{
    uint8_t val;
    outb(VGA_CRTC_INDEX, 0x11);
    val = inb(VGA_CRTC_DATA);
    outb(VGA_CRTC_DATA, val & 0x7F);
}

/**
 * Program a full VGA mode from its register tables.
 *
 * The order is the required VGA programming sequence: Miscellaneous Output,
 * Sequencer, then CRTC (unlocked first by clearing the 0x11 protect bit),
 * Graphics Controller, and Attribute Controller. The final INSTAT_READ resets
 * the AC flip-flop and writing AC index 0x20 sets the palette-address-source
 * bit, which re-enables video output.
 */
static void vga_set_mode(uint8_t misc, const uint8_t *seq, size_t seq_count, const uint8_t *crtc,
                         size_t crtc_count, const uint8_t *gc, size_t gc_count, const uint8_t *ac,
                         size_t ac_count)
{
    size_t i;

    outb(VGA_MISC_WRITE, misc);

    for (i = 0; i < seq_count; i++) {
        vga_seq_write(i, seq[i]);
    }

    vga_crtc_unlock();

    for (i = 0; i < crtc_count; i++) {
        vga_crtc_write(i, crtc[i]);
    }

    for (i = 0; i < gc_count; i++) {
        vga_gc_write(i, gc[i]);
    }

    for (i = 0; i < ac_count; i++) {
        vga_ac_write(i, ac[i]);
    }

    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x20);
}

/**
 * Save the text-mode font from plane 2 (character generator RAM) into
 * saved_font.
 *
 * The font lives in plane 2 at A0000. To read it as linear memory the
 * Sequencer and Graphics Controller are switched out of the odd/even text
 * layout into sequential access to plane 2 alone. Each glyph occupies 32
 * bytes there but only the first 16 are used by the 8x16 font.
 */
static void vga_save_font(void)
{
    volatile uint8_t *font_mem = (volatile uint8_t *)0xA0000;
    int i, j;

    outb(VGA_SEQ_INDEX, 0x02); /* Map Mask */
    outb(VGA_SEQ_DATA, 0x04);  /* enable plane 2 only */

    outb(VGA_SEQ_INDEX, 0x04); /* Memory Mode */
    outb(VGA_SEQ_DATA, 0x06);  /* sequential, extended memory, odd/even off */

    outb(VGA_GC_INDEX, 0x04); /* Read Map Select */
    outb(VGA_GC_DATA, 0x02);  /* read from plane 2 */

    outb(VGA_GC_INDEX, 0x05); /* Graphics Mode */
    outb(VGA_GC_DATA, 0x00);  /* read mode 0, write mode 0 */

    outb(VGA_GC_INDEX, 0x06); /* Miscellaneous */
    outb(VGA_GC_DATA, 0x04);  /* map at A0000-AFFFF (64K) */

    for (i = 0; i < 256; i++) {
        for (j = 0; j < 16; j++) {
            saved_font[i * 16 + j] = font_mem[i * 32 + j];
        }
    }
}

/**
 * Restore saved_font to plane 2 and return the Sequencer and Graphics
 * Controller to the text-mode odd/even configuration.
 *
 * The first block mirrors vga_save_font's plane-2 sequential access to write
 * the glyphs back; the second restores the text layout: Map Mask to planes 0
 * and 1, odd/even memory mode, plane 0 reads, and the B8000 text window.
 */
static void vga_restore_font(void)
{
    volatile uint8_t *font_mem = (volatile uint8_t *)0xA0000;
    int i, j;

    outb(VGA_SEQ_INDEX, 0x02); /* Map Mask */
    outb(VGA_SEQ_DATA, 0x04);  /* enable plane 2 only */

    outb(VGA_SEQ_INDEX, 0x04); /* Memory Mode */
    outb(VGA_SEQ_DATA, 0x06);  /* sequential, extended memory, odd/even off */

    outb(VGA_GC_INDEX, 0x04); /* Read Map Select */
    outb(VGA_GC_DATA, 0x02);  /* read from plane 2 */

    outb(VGA_GC_INDEX, 0x05); /* Graphics Mode */
    outb(VGA_GC_DATA, 0x00);  /* read mode 0, write mode 0 */

    outb(VGA_GC_INDEX, 0x06); /* Miscellaneous */
    outb(VGA_GC_DATA, 0x04);  /* map at A0000-AFFFF (64K) */

    for (i = 0; i < 256; i++) {
        for (j = 0; j < 16; j++) {
            font_mem[i * 32 + j] = saved_font[i * 16 + j];
        }
    }

    outb(VGA_SEQ_INDEX, 0x02);
    outb(VGA_SEQ_DATA, 0x03); /* Map Mask: planes 0 and 1 */

    outb(VGA_SEQ_INDEX, 0x04);
    outb(VGA_SEQ_DATA, 0x02); /* Memory Mode: odd/even enabled */

    outb(VGA_GC_INDEX, 0x04);
    outb(VGA_GC_DATA, 0x00); /* read from plane 0 */

    outb(VGA_GC_INDEX, 0x05);
    outb(VGA_GC_DATA, 0x10); /* odd/even mode */

    outb(VGA_GC_INDEX, 0x06);
    outb(VGA_GC_DATA, 0x0E); /* map at B8000, text mode */
}

/**
 * Switch to Mode 13h and return the linear framebuffer at A0000.
 *
 * Saves the text font first so vga_mode13h_exit can restore it. A no-op that
 * returns the framebuffer if already in graphics mode. Publishes gfx_current.
 *
 * @return Pointer to the 320x200 8bpp framebuffer.
 */
uint8_t *vga_mode13h_enter(void)
{
    if (vga_graphics_mode) {
        return (uint8_t *)VGA_GFX_FRAMEBUFFER;
    }

    vga_save_font();

    vga_set_mode(mode13h_misc, mode13h_seq, sizeof(mode13h_seq), mode13h_crtc, sizeof(mode13h_crtc),
                 mode13h_gc, sizeof(mode13h_gc), mode13h_ac, sizeof(mode13h_ac));

    k_memset((void *)VGA_GFX_FRAMEBUFFER, 0, VGA_GFX_SIZE);

    vga_graphics_mode = true;

    gfx_current.mode = GFX_MODE_VGA_13H;
    gfx_current.width = VGA_GFX_WIDTH;
    gfx_current.height = VGA_GFX_HEIGHT;
    gfx_current.bpp = VGA_GFX_BPP;
    gfx_current.pitch = VGA_GFX_WIDTH;
    gfx_current.framebuffer = (uint8_t *)VGA_GFX_FRAMEBUFFER;
    gfx_current.size = VGA_GFX_SIZE;

    return (uint8_t *)VGA_GFX_FRAMEBUFFER;
}

/**
 * Load the classic 16-color CGA/EGA palette into the DAC for text mode.
 *
 * The Mode 3 Attribute Controller maps the 16 text attributes to scattered DAC
 * indices, so the DAC entries are written where those AC registers point:
 * AC[0-7] -> DAC 0x00-0x07 (low intensity), AC[6] -> DAC 0x14 (brown is the
 * odd one out), AC[8-15] -> DAC 0x38-0x3F (high intensity). DAC values are
 * 6-bit (0-63).
 */
static void vga_restore_text_palette(void)
{
    /* Low intensity colors at DAC indices 0-7 */
    static const uint8_t low_colors[8 * 3] = {
        0x00, 0x00, 0x00, /* 0: Black */
        0x00, 0x00, 0x2A, /* 1: Blue */
        0x00, 0x2A, 0x00, /* 2: Green */
        0x00, 0x2A, 0x2A, /* 3: Cyan */
        0x2A, 0x00, 0x00, /* 4: Red */
        0x2A, 0x00, 0x2A, /* 5: Magenta */
        0x2A, 0x15, 0x00, /* 6: Brown (also at 0x14) */
        0x2A, 0x2A, 0x2A  /* 7: Light Gray */
    };

    /* High intensity colors at DAC indices 0x38-0x3F (56-63) */
    static const uint8_t high_colors[8 * 3] = {
        0x15, 0x15, 0x15, /* 0x38: Dark Gray */
        0x15, 0x15, 0x3F, /* 0x39: Light Blue */
        0x15, 0x3F, 0x15, /* 0x3A: Light Green */
        0x15, 0x3F, 0x3F, /* 0x3B: Light Cyan */
        0x3F, 0x15, 0x15, /* 0x3C: Light Red */
        0x3F, 0x15, 0x3F, /* 0x3D: Light Magenta */
        0x3F, 0x3F, 0x15, /* 0x3E: Yellow */
        0x3F, 0x3F, 0x3F  /* 0x3F: White */
    };
    int i;

    outb(VGA_DAC_WRITE_INDEX, 0);
    for (i = 0; i < 8 * 3; i++) {
        outb(VGA_DAC_DATA, low_colors[i]);
    }

    /* DAC 0x14 is where AC[6] (brown) points. */
    outb(VGA_DAC_WRITE_INDEX, 0x14);
    outb(VGA_DAC_DATA, 0x2A); /* R */
    outb(VGA_DAC_DATA, 0x15); /* G */
    outb(VGA_DAC_DATA, 0x00); /* B */

    outb(VGA_DAC_WRITE_INDEX, 0x38);
    for (i = 0; i < 8 * 3; i++) {
        outb(VGA_DAC_DATA, high_colors[i]);
    }
}

/**
 * Leave graphics mode and return the VGA to its 80x25 text state.
 *
 * Reprograms Mode 3, reloads the text palette and font, clears gfx_current, and
 * clears the text screen. A no-op if not currently in graphics mode.
 */
void vga_mode13h_exit(void)
{
    if (!vga_graphics_mode) {
        return;
    }

    vga_set_mode(mode3_misc, mode3_seq, sizeof(mode3_seq), mode3_crtc, sizeof(mode3_crtc), mode3_gc,
                 sizeof(mode3_gc), mode3_ac, sizeof(mode3_ac));

    vga_restore_text_palette();
    vga_restore_font();

    vga_graphics_mode = false;

    gfx_current.mode = GFX_MODE_NONE;
    gfx_current.width = 0;
    gfx_current.height = 0;
    gfx_current.bpp = 0;
    gfx_current.pitch = 0;
    gfx_current.framebuffer = NULL;
    gfx_current.size = 0;

    k_cls();
}

/**
 * Load a 256-entry RGB palette into the DAC.
 *
 * @param data 768 bytes, 256 entries of 8-bit R, G, B. Each component is
 *   shifted right by 2 because the VGA DAC has 6-bit values (0-63).
 *   Ignored if NULL.
 */
void vga_mode13h_set_palette(const uint8_t *data)
{
    int i;

    if (!data) {
        return;
    }

    outb(VGA_DAC_WRITE_INDEX, 0);

    for (i = 0; i < 768; i++) {
        outb(VGA_DAC_DATA, data[i] >> 2);
    }
}

uint8_t *vga_mode13h_get_framebuffer(void)
{
    return (uint8_t *)VGA_GFX_FRAMEBUFFER;
}

/** Copy a full VGA_GFX_SIZE frame to the framebuffer; ignored unless in
 * graphics mode and src is non-NULL. */
void vga_mode13h_blit(const uint8_t *src)
{
    if (src && vga_graphics_mode) {
        k_memcpy((void *)VGA_GFX_FRAMEBUFFER, src, VGA_GFX_SIZE);
    }
}
