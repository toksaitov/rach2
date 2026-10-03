/**
 * VGA mode transitions with a pending keyboard interrupt at each port access,
 * and the console's font loaded into the adapter's character memory: each
 * glyph at its character's 32 bytes, read back as written, and refused in a
 * graphics mode.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#include "display/vga_gfx.h"

static uint8_t aperture[VGA_GFX_SIZE];
#undef VGA_GFX_FRAMEBUFFER
#define VGA_GFX_FRAMEBUFFER aperture

#include "display/vga_gfx.c"
#include "display/vga_font.c"

extern int printf(const char *format, ...);
static int failures;
static uint32_t masked;
static uint8_t crtc[256];
static uint8_t index_reg;
static unsigned int accesses;
static unsigned int inject_at;
static unsigned int echoes;
static unsigned int clears;
static int pending;
static int expected_mode;
static int font_row; /* the font's own load, which no mode change is part of */
static unsigned int unmasked_accesses;

#define INJECT_ENTRY 0xFFFFFFFFu
#define INJECT_EXIT  0xFFFFFFFEu

#define CHECK(c, s)                                \
    do {                                           \
        if (!(c)) {                                \
            printf("vga-gfx-test: fail: %s\n", s); \
            failures++;                            \
        }                                          \
    } while (0)

/** Echo updates both cursor registers and leaves cursor-low selected. */
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

/** Save the caller's interrupt state. */
uint32_t kirq_save(void)
{
    uint32_t old = masked;
    masked = 1;
    return old;
}

/** Require complete publication and font copying before interrupts resume. */
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

/** The transition must include clearing the text display. */
void k_cls(void)
{
    CHECK(masked, "text clear excludes interrupts");
    clears++;
}

/** Run entry and exit, with a repeated call to each, from either flag state. */
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

/** Try every port boundary, including font mapping and palette restoration. */
int main(void)
{
    unsigned int point, total;

    row_font();

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
