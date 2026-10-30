/**
 * blankprobe: the console's blanking, driven from user space as util-linux's
 * setterm drives Linux's.
 *
 *   blankprobe blank     TIOCLINUX 14 blanks the console, and 15 then says 1;
 *   blankprobe unblank   TIOCLINUX 4 unblanks it, and 15 then says 0;
 *   blankprobe state     prints what TIOCLINUX 15 says;
 *   blankprobe time N    writes `ESC [ 9 ; N ]`, the blank time in minutes.
 *
 * Each prints `BLANKPROBE BLANKED N`, N what TIOCLINUX 15 returned, or for
 * `time` `BLANKPROBE TIME N`, and exits 0; a request the kernel refused
 * prints a line that begins `  FAIL: ` and exits 1. Run at the kernel's
 * debug shell by `tests/run screen`, whose scenario judges the screen.
 */

#include "probe.h"

#define TIOCLINUX           0x541C
#define TIOCL_UNBLANKSCREEN 4
#define TIOCL_BLANKSCREEN   14
#define TIOCL_BLANKEDSCREEN 15

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The length of the string `s`. */
static int len(const char *s)
{
    int n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/** Whether the strings `a` and `b` are the same. */
static int same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** TIOCLINUX with subcode `sub` on standard output: its result, or a
 * negated errno. */
static long tioclinux(unsigned char sub)
{
    return syscall3(SYS_IOCTL, 1, TIOCLINUX, (long)&sub);
}

/** Print `BLANKPROBE BLANKED N` from TIOCLINUX 15; 0, or 1 after a FAIL
 * line when the request failed or `want` is not -1 and differs. */
static int report(long want)
{
    long r = tioclinux(TIOCL_BLANKEDSCREEN);
    char digit[2];

    if (sys_failed(r)) {
        probe_line("  FAIL: TIOCLINUX 15 was refused", 0, 0);
        return 1;
    }
    digit[0] = (char)('0' + (r & 7));
    digit[1] = '\0';
    probe_line("BLANKPROBE BLANKED ", digit, 0);
    if (want != -1 && r != want) {
        probe_line("  FAIL: TIOCLINUX 15 did not say ", want ? "1" : "0", 0);
        return 1;
    }
    return 0;
}

/** The request of the opening comment named by argv[1]. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    const char *what = argc >= 2 ? argv[1] : "";
    char seq[16];
    int n, i, bad = 0;

    if (same(what, "blank") || same(what, "unblank")) {
        int blank = same(what, "blank");

        if (sys_failed(tioclinux(blank ? TIOCL_BLANKSCREEN : TIOCL_UNBLANKSCREEN))) {
            probe_line("  FAIL: TIOCLINUX ", blank ? "14" : "4", " was refused");
            bad = 1;
        }
        bad |= report(blank);
    } else if (same(what, "state")) {
        bad = report(-1);
    } else if (same(what, "time") && argc >= 3 && (n = len(argv[2])) > 0 && n <= 4) {
        seq[0] = '\033';
        seq[1] = '[';
        seq[2] = '9';
        seq[3] = ';';
        for (i = 0; i < n; i++) {
            seq[4 + i] = argv[2][i];
        }
        seq[4 + n] = ']';
        syscall3(SYS_WRITE, 1, (long)seq, 5 + n);
        probe_line("BLANKPROBE TIME ", argv[2], 0);
    } else {
        probe_line("  FAIL: usage: blankprobe blank|unblank|state|time N", 0, 0);
        bad = 1;
    }
    syscall1(SYS_EXIT, bad);
    __builtin_unreachable();
}
