/**
 * Exercises the kernel termios layer through musl's own termios API.
 *
 * The freestanding ttyprobe drives the kernel ioctls with hand-built structs;
 * this proves the same kernel layer satisfies musl's API, whose termios struct
 * is wider (NCCS=32 plus speeds) than the 36-byte prefix the kernel exchanges.
 * If the prefix layout (flags, c_line, the c_cc indices) were off, cfmakeraw's
 * effect would be misread here.
 *
 * The checks are: isatty(0) reports the console is a terminal (musl probes it
 * with TIOCGWINSZ); tcgetattr shows the inherited mode cooked with echo
 * (ICANON|ECHO); cfmakeraw plus tcsetattr puts the terminal in raw mode,
 * confirmed by reading it back; one read() returns a single byte with no line
 * editing or echo (the driver sends "Z" right after the exec line); and a final
 * tcsetattr restores the original mode.
 *
 * Prints "MTTY OK" or "MTTY FAIL <n>". Staged into test.krffs as /musl/ttymusl.
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
    if (tcgetattr(STDIN_FILENO, &back) != 0 || (back.c_lflag & ICANON) || (back.c_lflag & ECHO)) {
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
