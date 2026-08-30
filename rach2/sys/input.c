/*
 * input.c: one keystroke path for two keyboards, and one mouse queue for two
 * mice.
 *
 * This was inside the PS/2 interrupt handler until a second keyboard existed.
 * Nothing about deciding where a keystroke goes is particular to the 8042: the
 * modifier state, the Ctrl+C interception, the scrollback keys and the choice
 * between the line discipline and the debug shell are the same whether the
 * code came off a port or out of a USB report. What is particular to the 8042
 * stayed there: reading the port.
 */

#include "input.h"

#include "display/vga_text.h"
#include "kbd.h"
#include "mouse.h"
#include "kio.h"
#include "klog.h"
#include "proc/wait_queue.h"
#include "tty/tty.h"

/*
 * The tables are `kbd.c`'s, indexed by set-1 scancode, and a zero means this
 * kernel has no character for that key. They are shared rather than copied,
 * because two tables of the same keyboard would be two things to keep in step.
 */

/* Which modifiers are held, and whether the last code was the extended
 * prefix. A USB keyboard's modifiers arrive as their own scancodes, so this
 * state is shared and correct for either keyboard. */
static int shift_held;
static int ctrl_held;
static int saw_e0;

/* Whether a mouse of any kind was found. */
static int mouse_present;

int input_mouse_present(void)
{
    return mouse_present;
}

void input_mouse_found(void)
{
    mouse_present = 1;
}

void input_mouse(int dx, int dy, int buttons)
{
    mouse_push_packet((uint8_t)buttons, dx, dy);
}

void input_key(uint8_t scancode)
{
    char c;
    int was_e0 = saw_e0;

    /* Every code, including the ones consumed below, goes to the ring a
     * graphics-mode reader drains: a game wants the modifiers and the releases
     * too, and it wants them from whichever keyboard the person is using. */
    kbd_push_scancode(scancode);

    saw_e0 = 0;

    if (scancode == 0xE0) {
        saw_e0 = 1;
        return;
    }

    /* Left and right shift. */
    if (scancode == 0x2A || scancode == 0x36) {
        shift_held = 1;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_held = 0;
        return;
    }

    /* Left and right control both report 0x1D pressed and 0x9D released. */
    if (scancode == 0x1D) {
        ctrl_held = 1;
        return;
    }
    if (scancode == 0x9D) {
        ctrl_held = 0;
        return;
    }

    /* The high bit marks a release, and nothing below cares about one. */
    if (scancode & 0x80) {
        return;
    }

    /*
     * 'C' is scancode 0x2E. When a session owns the console, Ctrl+C goes
     * through the line discipline like every other key: its ISIG path posts
     * SIGINT to the foreground group and flushes the pending input, which is
     * what a serial Ctrl+C does, so the two agree. Only where the debug shell
     * is the supervisor, reading through its own buffer rather than the
     * discipline, is the signal delivered out of band.
     */
    if (ctrl_held && scancode == 0x2E &&
        !(wq_active(&console_tty.input_wq) || console_tty.sid != 0)) {
        shell_interrupt();
        return;
    }

    if (was_e0 && shell_reads_input()) {
        if (scancode == 0x49) {
            k_scrollback_page_up();
            return;
        }
        if (scancode == 0x51) {
            k_scrollback_page_down();
            return;
        }
        if (scancode == 0x47) {
            k_scrollback_home();
            return;
        }
        if (scancode == 0x4F) {
            k_scrollback_end();
            return;
        }
    }

    c = (char)(shift_held ? EN_us_shift[scancode & 0x7F] : EN_us[scancode & 0x7F]);

    if (wq_active(&console_tty.input_wq) || console_tty.sid != 0) {
        /*
         * A session owns the console: give the keystroke to the line
         * discipline whether or not a reader is parked. With nobody parked the
         * wake is a no-op and the byte waits as type-ahead, the way the serial
         * drain queues input that arrives un-parked. A keyboard sends '\b' for
         * Backspace; the discipline's erase is DEL. Ctrl and a letter is that
         * letter's control byte, so line editing and end-of-file reach the
         * discipline as they would from a terminal.
         */
        if (c != 0) {
            unsigned char tc = (unsigned char)c;

            if (ctrl_held && c >= 'a' && c <= 'z') {
                tc = (unsigned char)(c - 'a' + 1);
            } else if (ctrl_held && c >= 'A' && c <= 'Z') {
                tc = (unsigned char)(c - 'A' + 1);
            } else if (c == '\b') {
                tc = 0x7F;
            }
            tty_input_char(&console_tty, tc);
        }
    } else if (c != 0 && shell_reads_input()) {
        shell_input_char(c);
    }
}
