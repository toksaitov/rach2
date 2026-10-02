/*
 * input.c: one keystroke path for two keyboards, and one mouse queue for two
 * mice.
 *
 * Nothing about deciding where a keystroke goes is particular to the 8042: the
 * modifier state, the Ctrl+C interception, the scrollback keys and the choice
 * between the line discipline and the debug shell are the same whether the
 * code came off a port or out of a USB report. What is particular to the 8042,
 * reading the port, is in its interrupt handler.
 */

#include "proc/suspend.h"
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
static int alt_held;
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

/**
 * Send the escape sequence of the extended key `scancode`, the code after an
 * E0 prefix, to the console's line discipline, as the `rach2` terminfo entry
 * lists it: the arrows, Home and End as xterm sends them, and Insert, Delete,
 * Page Up and Page Down as VT220 editing keys.
 *
 * @return 1 when `scancode` is one of those keys, 0 otherwise.
 */
static int extended_key(uint8_t scancode)
{
    const char *seq;

    switch (scancode) {
    case 0x48:
        seq = "\033[A";
        break;
    case 0x50:
        seq = "\033[B";
        break;
    case 0x4D:
        seq = "\033[C";
        break;
    case 0x4B:
        seq = "\033[D";
        break;
    case 0x47:
        seq = "\033[H";
        break;
    case 0x4F:
        seq = "\033[F";
        break;
    case 0x52:
        seq = "\033[2~";
        break;
    case 0x53:
        seq = "\033[3~";
        break;
    case 0x49:
        seq = "\033[5~";
        break;
    case 0x51:
        seq = "\033[6~";
        break;
    default:
        return 0;
    }
    while (*seq) {
        tty_input_char(&console_tty, (unsigned char)*seq++);
    }
    return 1;
}

/**
 * Give the key of `scancode`, `c` in the keymap and extended when `was_e0`,
 * to the console's line discipline, for a reader parked on it or the session
 * that owns it. Shift with Page Up or Page Down scrolls the screen back
 * instead, and any other key first returns it to the live screen.
 */
static void session_key(uint8_t scancode, char c, int was_e0)
{
    unsigned char tc = (unsigned char)c;

    if (was_e0 && shift_held && scancode == 0x49) {
        k_scrollback_page_up();
        return;
    }
    if (was_e0 && shift_held && scancode == 0x51) {
        k_scrollback_page_down();
        return;
    }
    if (k_scrollback_is_active()) {
        k_scrollback_end();
    }
    if ((was_e0 && extended_key(scancode)) || c == 0) {
        return;
    }
    if (ctrl_held && c >= 'a' && c <= 'z') {
        tc = (unsigned char)(c - 'a' + 1);
    } else if (ctrl_held && c >= 'A' && c <= 'Z') {
        tc = (unsigned char)(c - 'A' + 1);
    } else if (c == '\b') {
        tc = 0x7F;
    }
    if (alt_held) {
        tty_input_char(&console_tty, 0x1B);
    }
    tty_input_char(&console_tty, tc);
}

/**
 * Route one set-1 scancode.
 *
 * Every code, including the ones consumed here, goes first to the ring a
 * graphics-mode reader drains, since a game needs the modifiers and the
 * releases too. The extended Page Up, Page Down, Home and End scroll the
 * screen whenever the debug shell reads input, and Shift with Page Up or Page
 * Down scrolls it in a session, where any other key returns it to the live
 * screen before it is sent, as on Linux's console. When a reader is parked on
 * the console or a session owns it, Ctrl+C goes through the line discipline
 * like every other key: its ISIG path posts SIGINT to the foreground group and
 * flushes the pending input, as a serial Ctrl+C does. Otherwise Ctrl+C is
 * `shell_interrupt`, out of band, and other keys go to the debug shell's own
 * buffer when it reads.
 *
 * For the discipline, Backspace, '\b' in the keymap, becomes DEL, its erase
 * character, Ctrl with a letter becomes that letter's control byte, Alt puts
 * ESC before a key's character, as Linux's console sends Meta, and the
 * arrows and editing keys become their escape sequences, so line editing, end
 * of file and cursor keys arrive as they would from a terminal. A key nobody
 * is parked for waits in the discipline as type-ahead.
 */
void input_key(uint8_t scancode)
{
    char c;
    int was_e0 = saw_e0;

    suspend_wake();
    kbd_push_scancode(scancode);

    saw_e0 = 0;

    if (scancode == 0xE0) {
        saw_e0 = 1;
        return;
    }

    /* Left and right shift, pressed or let go. After E0 they are the shifts
       a PS/2 keyboard sends around a cursor key for Num Lock, not a key. */
    if ((scancode & 0x7F) == 0x2A || (scancode & 0x7F) == 0x36) {
        if (!was_e0) {
            shift_held = !(scancode & 0x80);
        }
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

    /* Left and right Alt both report 0x38 pressed and 0xB8 released. */
    if ((scancode & 0x7F) == 0x38) {
        alt_held = !(scancode & 0x80);
        return;
    }

    /* The high bit marks a release, and nothing below cares about one. */
    if (scancode & 0x80) {
        return;
    }

    /* 'C' is scancode 0x2E. */
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
        session_key(scancode, c, was_e0);
    } else if (c != 0 && shell_reads_input()) {
        shell_input_char(c);
    }
}
