/*
 * ttymusl.c - Phase 16 musl integration: termios through the real ported libc.
 *
 * The freestanding ttyprobe drives the kernel termios ioctls with hand-built
 * structs; this proves the SAME kernel layer satisfies musl's own termios API,
 * whose `struct termios` is wider (NCCS=32 + speeds) than the 36-byte prefix the
 * kernel exchanges. If the prefix layout (flags, c_line, the c_cc indices) were
 * off, cfmakeraw's effect would be misread here.
 *
 *   - isatty(0)          - the console is a terminal (musl probes TIOCGWINSZ);
 *   - tcgetattr          - the inherited mode is cooked with echo (ICANON|ECHO);
 *   - cfmakeraw+tcsetattr- raw mode takes, confirmed by reading it back;
 *   - one raw getch      - read() returns a single byte with no line/echo (the
 *                          driver sends "Z" right after the exec line);
 *   - restore            - the original mode is put back.
 *
 * Prints "MTTY OK" / "MTTY FAIL <n>". Installed as "mtty" by build_test_ramdisk.sh.
 */

#include <termios.h>
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    struct termios orig;
    struct termios raw;
    struct termios back;
    unsigned char c = 0;
    int fails = 0;
    ssize_t n;

    if (!isatty(STDIN_FILENO)) {
        printf("isatty(0) false\n");
        fails++;
    }

    if (tcgetattr(STDIN_FILENO, &orig) != 0) {
        printf("tcgetattr failed\n");
        printf("MTTY FAIL %d\n", fails + 1);
        return 1;
    }
    if (!(orig.c_lflag & ICANON)) {
        printf("inherited mode not ICANON\n");
        fails++;
    }
    if (!(orig.c_lflag & ECHO)) {
        printf("inherited mode not ECHO\n");
        fails++;
    }

    /* Raw mode through musl's cfmakeraw, then confirm the kernel applied it. */
    raw = orig;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        printf("tcsetattr(raw) failed\n");
        fails++;
    }
    if (tcgetattr(STDIN_FILENO, &back) != 0 || (back.c_lflag & ICANON) ||
        (back.c_lflag & ECHO)) {
        printf("raw mode did not take (ICANON/ECHO still set)\n");
        fails++;
    }

    /* One byte, raw: no newline needed, nothing echoed. */
    n = read(STDIN_FILENO, &c, 1);
    if (n != 1 || c != 'Z') {
        printf("raw read got n=%d c=%d\n", (int)n, (int)c);
        fails++;
    }

    /* Put the terminal back the way we found it. */
    if (tcsetattr(STDIN_FILENO, TCSANOW, &orig) != 0) {
        printf("tcsetattr(restore) failed\n");
        fails++;
    }

    if (fails == 0) {
        printf("MTTY OK\n");
    } else {
        printf("MTTY FAIL %d\n", fails);
    }
    return fails;
}
