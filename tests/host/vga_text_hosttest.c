/**
 * Count what the text console driver writes to the adapter.
 *
 * host-deps:
 *
 * The adapter's memory is an array here and its ports are recorded, so the
 * rows count what each operation costs. A byte is one cell. A scroll is the
 * new line's eighty cells and the start address, with the rows above left in
 * the cells they were in. The copy to the front of the memory comes only
 * when the window reaches its end, once in 179 lines. A scroll back and a
 * clear redraw the screen. A held cursor is placed once, at the release. And
 * the twenty-five rows the start address selects are the history's live
 * lines throughout, including across the copy. The driver is included whole
 * to reach its state.
 */
#include "lib/kint.h"

static uint16_t vram[16384];
#define VGA_TEXT_BUFFER vram
#include "display/vga_text.c"

extern int printf(const char *format, ...);

static unsigned int port_writes;
static uint8_t crtc_index;
static uint16_t start_address;
static uint16_t cursor_address;
static int failures;

#define CHECK(condition, detail)                         \
    do {                                                 \
        if (!(condition)) {                              \
            printf("vga-text-test: fail: %s\n", detail); \
            failures++;                                  \
        }                                                \
    } while (0)

/* The CRTC as the driver sees it: an index port and a data port, of which
   the start address and the cursor location are kept. */
void outb(uint16_t port, uint8_t val)
{
    port_writes++;
    if (port == 0x3D4) {
        crtc_index = val;
    } else if (port == 0x3D5) {
        switch (crtc_index) {
        case 0x0C:
            start_address = (uint16_t)((start_address & 0x00FFu) | ((uint16_t)val << 8));
            break;
        case 0x0D:
            start_address = (uint16_t)((start_address & 0xFF00u) | val);
            break;
        case 0x0E:
            cursor_address = (uint16_t)((cursor_address & 0x00FFu) | ((uint16_t)val << 8));
            break;
        case 0x0F:
            cursor_address = (uint16_t)((cursor_address & 0xFF00u) | val);
            break;
        default:
            break;
        }
    }
}

static uint16_t before[16384];

static void snapshot(void)
{
    unsigned int i;

    for (i = 0; i < 16384; i++) {
        before[i] = vram[i];
    }
}

/** The cells that differ from the snapshot. */
static unsigned int changed(void)
{
    unsigned int i, n = 0;

    for (i = 0; i < 16384; i++) {
        if (vram[i] != before[i]) {
            n++;
        }
    }
    return n;
}

/**
 * Whether the window the start address selects shows the viewport's lines,
 * cell for cell, from the history ring.
 */
static int window_shows_viewport(void)
{
    unsigned int row, col;

    if (start_address != _hw_top) {
        return 0;
    }
    for (row = 0; row < VGA_TEXT_ROWS; row++) {
        uint32_t line = _viewport_top_line + row;
        uint16_t *cells = vga_text_line_ptr(line);

        for (col = 0; col < VGA_TEXT_COLS; col++) {
            uint16_t want = cells != 0 ? cells[col] : vga_text_blank_cell();

            if (vram[_hw_top + row * VGA_TEXT_COLS + col] != want) {
                return 0;
            }
        }
    }
    return 1;
}

int main(void)
{
    unsigned int n, ports;

    /* A clear draws the screen once and points the display at it. */
    k_cls();
    CHECK(_hw_top == 0 && start_address == 0,
          "a clear starts the display at the front of the memory");
    CHECK(window_shows_viewport(), "a clear shows the blank history");

    /* A byte is one cell and, placed, four port writes. */
    x = 1;
    y = 0;
    snapshot();
    ports = port_writes;
    vga_text_write_cell(0, 0, 'A', 0x07);
    CHECK(changed() == 1, "a byte changes one cell");
    CHECK(vram[0] == vga_text_make_cell('A', 0x07), "the byte's cell holds the byte");
    CHECK(port_writes == ports, "a byte alone touches no port");
    vga_text_place_cursor();
    CHECK(port_writes == ports + 4, "placing the cursor is four port writes");
    CHECK(cursor_address == 1, "the cursor is at the byte after the one written");

    /* Held, the cursor is placed once at the release, wherever it went. */
    ports = port_writes;
    vga_text_hold();
    vga_text_hold();
    x = 5;
    vga_text_place_cursor();
    x = 7;
    vga_text_place_cursor();
    vga_text_release();
    CHECK(port_writes == ports, "a nested hold keeps the cursor where it was");
    vga_text_release();
    CHECK(port_writes == ports + 4, "the last release places the cursor once");
    CHECK(cursor_address == 7, "and places it where it last went");
    ports = port_writes;
    vga_text_hold();
    vga_text_release();
    CHECK(port_writes == ports, "a hold in which the cursor did not move places nothing");

    /* 179 scrolls move the window down the memory, a line and the start
       address each, and the rows above stay in their cells. Each line gets
       a mark before the scroll, so the copy later is checked by content. */
    for (n = 0; n < 179; n++) {
        vga_text_write_cell(0, VGA_TEXT_ROWS - 1, (char)('a' + (n % 26)), 0x07);
        snapshot();
        ports = port_writes;
        k_scroll_down();
        CHECK(changed() == VGA_TEXT_COLS, "a scroll changes the new line's cells and no other");
        CHECK(port_writes == ports + 4, "a scroll writes the start address and nothing else");
        CHECK(_hw_top == (n + 1) * VGA_TEXT_COLS, "a scroll moves the window down a line");
        CHECK(window_shows_viewport(), "the window shows the live lines after a scroll");
        if (failures) {
            printf("vga-text-test: at scroll %u\n", n + 1);
            return 1;
        }
    }
    CHECK(_hw_top == 179 * VGA_TEXT_COLS, "179 scrolls reach the end of the memory");

    /* The 180th would leave the memory: the rows that stay are copied to
       the front, and the window shows the same lines from there. */
    vga_text_write_cell(0, VGA_TEXT_ROWS - 1, 'Z', 0x07);
    snapshot();
    ports = port_writes;
    k_scroll_down();
    CHECK(_hw_top == 0 && start_address == 0,
          "the scroll at the end returns the window to the front");
    CHECK(changed() <= VGA_TEXT_SCREEN_CELLS, "the copy writes at most one screen");
    CHECK(port_writes == ports + 4, "the copy writes the start address once");
    CHECK(window_shows_viewport(), "the window shows the live lines after the copy");
    CHECK(vram[(VGA_TEXT_ROWS - 2) * VGA_TEXT_COLS] == vga_text_make_cell('Z', 0x07),
          "the copied rows carry their content");

    /* Scrolled back, the screen is redrawn from history, live output does
       not touch the adapter, and the return redraws the live lines. */
    k_scrollback_page_up();
    CHECK(!_follow_live, "page up leaves the live lines");
    CHECK(window_shows_viewport(), "the window shows the history page");
    snapshot();
    vga_text_write_cell(0, 0, 'Q', 0x07);
    CHECK(changed() == 0, "live output while scrolled back leaves the adapter alone");
    k_scroll_down();
    CHECK(changed() == 0, "a scroll while scrolled back leaves the adapter alone");
    k_scrollback_end();
    CHECK(_follow_live, "end returns to the live lines");
    CHECK(window_shows_viewport(), "the return shows the live lines, the byte among them");

    if (failures) {
        printf("vga-text-test: %d failure(s)\n", failures);
        return 1;
    }
    printf("vga-text-test: OK\n");
    return 0;
}
