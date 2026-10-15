/**
 * VGA mode transitions with a pending keyboard interrupt at each port access,
 * and the console's font loaded into the adapter's character memory: each
 * glyph at its character's 32 bytes, read back as written, and not loaded in
 * a graphics mode.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#include "display/vga_gfx.h"

static uint8_t aperture[VGA_GFX_SIZE];
#undef VGA_GFX_FRAMEBUFFER
#define VGA_GFX_FRAMEBUFFER aperture

#include "display/vga_gfx.c"
#include "display/vga_font.c"

/* The console on the adapter, not on a framebuffer. */
bool fbcon_active = false;

/* The framebuffer console's side of the picture, which the rows count. */
static int gfx_begins, gfx_ends;
static const uint8_t *gfx_palette_seen, *gfx_frame_seen;

/** Count the framebuffer console giving the screen to mode 13h. */
void fbcon_gfx_begin(void)
{
    gfx_begins++;
}

/** Count the framebuffer console taking the screen back. */
void fbcon_gfx_end(void)
{
    gfx_ends++;
}

/** Record the palette handed to the framebuffer console. */
void fbcon_gfx_palette(const uint8_t *rgb)
{
    gfx_palette_seen = rgb;
}

/** Record the frame handed to the framebuffer console. */
void fbcon_gfx_frame(const uint8_t *pixels)
{
    gfx_frame_seen = pixels;
}

extern int printf(const char *format, ...);
static int failures;
static uint32_t masked;   /* the modelled interrupt flag: 1 when masked */
static uint8_t crtc[256]; /* the CRTC's registers */
static uint8_t index_reg; /* the CRTC register selected */
static unsigned int accesses;
static unsigned int inject_at; /* the access that raises the interrupt */
static unsigned int echoes;    /* interrupts delivered */
static unsigned int clears;
static int pending;       /* an interrupt raised and not yet delivered */
static int expected_mode; /* the mode the transition under way publishes */
static int font_row;      /* the font's own load, which no mode change is part of */
static unsigned int unmasked_accesses;

/* Values of `inject_at` that raise the interrupt when register 9 is selected
 * on the way into mode 13h or on the way out. */
#define INJECT_ENTRY 0xFFFFFFFFu
#define INJECT_EXIT  0xFFFFFFFEu

#define CHECK(c, s)                                \
    do {                                           \
        if (!(c)) {                                \
            printf("vga-gfx-test: fail: %s\n", s); \
            failures++;                            \
        }                                          \
    } while (0)

/** Deliver a pending interrupt unless masked: its keyboard echo writes both
 * cursor registers and leaves cursor-low selected. */
static void deliver_echo(void)
{
    if (pending && !masked) {
        pending = 0;
        echoes++;
        crtc[0x0E] = 1;
        crtc[0x0F] = 0;
        index_reg = 0x0F;
    }
}

/** Queue an interrupt after the selected access; masking delays delivery. */
static void interrupt_point(void)
{
    accesses++;
    if (!masked)
        unmasked_accesses++;
    if (accesses == inject_at)
        pending = 1;
    deliver_echo();
}

/** Model the CRTC's shared register selector. */
void outb(uint16_t port, uint8_t value)
{
    if (port == VGA_CRTC_INDEX)
        index_reg = value;
    if (port == VGA_CRTC_INDEX && value == 9 &&
        inject_at == (expected_mode ? INJECT_ENTRY : INJECT_EXIT))
        pending = 1;
    if (port == VGA_CRTC_DATA)
        crtc[index_reg] = value;
    interrupt_point();
}

/** Return the selected CRTC register; other reads need no device data. */
uint8_t inb(uint16_t port)
{
    uint8_t value = port == VGA_CRTC_DATA ? crtc[index_reg] : 0;
    interrupt_point();
    return value;
}

/** Mask interrupts and return the mask the caller had. */
uint32_t kirq_save(void)
{
    uint32_t old = masked;
    masked = 1;
    return old;
}

/** Require complete publication and font copying before interrupts resume,
 * then restore the mask `flags` and deliver a pending interrupt. */
void kirq_restore(uint32_t flags)
{
    unsigned int i, j;
    if (font_row) {
        masked = flags;
        return;
    }
    CHECK(vga_graphics_mode == expected_mode, "mode published before restore");
    CHECK(gfx_current.mode == (expected_mode ? GFX_MODE_VGA_13H : GFX_MODE_NONE),
          "framebuffer state published before restore");
    if (!expected_mode) {
        CHECK(clears == 1, "text cleared before restore");
        for (i = 0; i < 256; i++)
            for (j = 0; j < 16; j++)
                CHECK(aperture[i * 32 + j] == (uint8_t)(i + j), "font restored before interrupts");
    } else {
        for (i = 0; i < 256; i++)
            for (j = 0; j < 16; j++)
                CHECK(saved_font[i * 16 + j] == (uint8_t)(i + j), "font saved before interrupts");
    }
    masked = flags;
    deliver_echo();
}

/** Count a clear of the text display, which must run with interrupts
 * masked. */
void k_cls(void)
{
    CHECK(masked, "text clear excludes interrupts");
    clears++;
}

/**
 * Run entry and exit, with a repeated call to each, from the interrupt mask
 * `initial_mask`, raising the interrupt at access `point` (0 for none, or
 * INJECT_ENTRY or INJECT_EXIT). Returns the port accesses of one entry and
 * one exit.
 */
static unsigned int row_transition(unsigned int point, uint32_t initial_mask)
{
    unsigned int i, j, total;

    for (i = 0; i < 256; i++)
        for (j = 0; j < 16; j++)
            aperture[i * 32 + j] = (uint8_t)(i + j);
    vga_graphics_mode = false;
    crtc[9] = 0x4F;
    masked = initial_mask;
    accesses = echoes = clears = 0;
    unmasked_accesses = 0;
    pending = 0;
    inject_at = point;
    expected_mode = 1;
    CHECK(fb_enter() == aperture, "entry returns aperture");
    CHECK(crtc[9] == 0x41, "entry programs maximum scan line");
    CHECK(masked == initial_mask, "entry restores caller flags");
    total = accesses;
    fb_enter();
    CHECK(accesses == total && masked == initial_mask, "repeated entry restores flags");
    expected_mode = 0;
    fb_exit();
    CHECK(crtc[9] == 0x4F, "exit programs maximum scan line");
    CHECK(masked == initial_mask, "exit restores caller flags");
    total = accesses;
    fb_exit();
    CHECK(accesses == total && masked == initial_mask, "repeated exit restores flags");
    CHECK(unmasked_accesses == 0, "font and mode ports exclude interrupts");
    if (point && point <= total)
        CHECK(initial_mask ? pending && !echoes : echoes == 1,
              "pending interrupt obeys caller flags");
    return total;
}

/** The font lands at each glyph's character and reads back. */
static void row_font(void)
{
    const struct vga_glyph *g = &vga_font_glyphs[VGA_FONT_GLYPHS - 1];

    font_row = 1;
    k_memset(aperture, 0, sizeof(aperture));
    CHECK(vga_font_load() == 1, "the font reads back as written");
    CHECK(aperture[g->slot * 32 + 7] == g->rows[7] && aperture[g->slot * 32 + 16] == 0,
          "a glyph's rows are the first 16 of its character's 32 bytes");
    vga_graphics_mode = true;
    CHECK(vga_font_load() == 0, "a graphics mode refuses the font");
    vga_graphics_mode = false;
    font_row = 0;
}

/** On a framebuffer console mode 13h programs no port and its picture is
 * memory, not the aperture, which may alias the screen the console draws;
 * the console takes the screen at the entry, the palette whole, a frame at
 * each present, and gives the screen back at the exit. */
static void row_fbcon(void)
{
    static const uint8_t palette[768] = {1, 2, 3};
    uint8_t *pixels;

    font_row = 1; /* no font is saved or restored here */
    fbcon_active = true;
    vga_graphics_mode = false;
    accesses = 0;
    k_memset(aperture, 0x5A, sizeof(aperture));
    expected_mode = 1;
    pixels = fb_enter();
    CHECK(pixels != 0 && pixels != aperture && gfx_current.framebuffer == pixels &&
              gfx_current.size == VGA_GFX_SIZE,
          "the picture on a framebuffer console is memory of its own");
    pixels[0] = 0xEE;
    pixels[VGA_GFX_SIZE - 1] = 0xEE;
    fb_set_palette(palette);
    fb_present();
    CHECK(accesses == 0 && aperture[0] == 0x5A, "and no port nor the aperture is touched");
    CHECK(gfx_begins == 1 && gfx_palette_seen == palette && gfx_frame_seen == pixels,
          "the console takes the screen, the palette whole and the frame");
    expected_mode = 0;
    fb_exit();
    CHECK(accesses == 0 && gfx_current.framebuffer == 0, "nor by the way back");
    CHECK(gfx_ends == 1, "and gives the screen back");
    fbcon_active = false;
    font_row = 0;
}

/** Try every port boundary, including font mapping and palette restoration:
 * 0 when every check passes, 1 otherwise. */
int main(void)
{
    unsigned int point, total;

    row_font();
    row_fbcon();

    row_transition(INJECT_ENTRY, 0);
    row_transition(INJECT_EXIT, 0);
    total = row_transition(0, 0);
    for (point = 1; point <= total; point++) {
        row_transition(point, 0);
        row_transition(point, 1);
        if (failures)
            break;
    }
    printf("vga-gfx-test: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}
