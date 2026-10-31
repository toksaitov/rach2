/*
 * keymap_keytable_hosttest.c: the key table, tests/host/keytable.txt, run
 * through the console's translator, rach2/sys/keymap.c, with its built-in
 * US keymap.
 *
 * host-deps: rach2/sys/keymap.c tests/host/keytable.c
 * host-inputs: tests/host/keytable.txt
 *
 * Each case of the table starts from a fresh translator state, with Num Lock
 * and Caps Lock on as its modes say, and gives its events to `keymap_key`;
 * every result that sends something is encoded by `keymap_encode` under the
 * case's cursor and keypad modes, and the bytes of all of them together must
 * be the table's. A case the table calls local must send nothing and give
 * one of the console's scrollback actions. A case of the tty modes passes
 * the terminal's bytes through a model of the line discipline with Linux's
 * default control characters: ICRNL turns CR into NL, ISIG turns VINTR,
 * VQUIT and VSUSP into their signals, and ICANON keeps only whole lines,
 * with VERASE erasing a byte, as the table writes them.
 */

#include <stdio.h>
#include <string.h>

#include "keymap.h"
#include "keytable.h"

static int failures;

/* The failing cases printed in full; the rest are only counted. */
#define SHOWN 20

/* Linux's default control characters, and the signals they raise. */
#define VINTR_CHAR  0x03
#define VQUIT_CHAR  0x1C
#define VSUSP_CHAR  0x1A
#define VERASE_CHAR 0x7F
#define SIGINT_NR   2
#define SIGQUIT_NR  3
#define SIGTSTP_NR  20

/* What one case gave. */
struct outcome {
    enum keytable_kind kind;
    int sig;
    int local; /* a scrollback action was given */
    int nbytes;
    unsigned char bytes[KEYTABLE_MAX_BYTES * 4];
};

/**
 * The translator's results for the events of `c`, as the terminal's bytes
 * in `out`, with `out->local` set when a result is a scrollback action.
 */
static void translate(const struct keytable_case *c, struct outcome *out)
{
    struct keymap map;
    struct keymap_state st;
    struct keymap_result res;
    unsigned int leds = 0, modes = 0, n;
    int i;

    map.entry = keymap_default_entry;
    map.ctx = keymap_default_tables;
    map.func = keymap_default_func;
    map.diacr = keymap_default_diacr;
    map.ndiacr = keymap_default_ndiacr;
    if (c->modes & KEYTABLE_NUM) {
        leds |= KM_LED_NUM;
    }
    if (c->modes & KEYTABLE_CAPS) {
        leds |= KM_LED_CAPS;
    }
    if (c->modes & KEYTABLE_CKM) {
        modes |= KM_MODE_CURSOR;
    }
    if (c->modes & KEYTABLE_KPAM) {
        modes |= KM_MODE_KEYPAD;
    }
    keymap_state_init(&st, leds);
    out->kind = KEYTABLE_BYTES;
    out->sig = 0;
    out->local = 0;
    out->nbytes = 0;
    for (i = 0; i < c->nevents; i++) {
        keymap_key(&st, &map, c->events[i].code, c->events[i].press, &res);
        if (res.kind == KM_RESULT_SPECIAL &&
            (res.key == KM_SCROLLBACK || res.key == KM_SCROLLFORW)) {
            out->local = 1;
            continue;
        }
        if (res.kind == KM_RESULT_NONE || res.kind == KM_RESULT_LOCKS) {
            continue;
        }
        n = keymap_encode(&map, &res, modes, (char *)out->bytes + out->nbytes,
                          (unsigned int)(sizeof(out->bytes) - (size_t)out->nbytes));
        out->nbytes += (int)n;
    }
}

/**
 * Give `out`'s bytes to a model of the line discipline under the tty flags
 * of `modes`, KEYTABLE_ICANON, KEYTABLE_ICRNL and KEYTABLE_ISIG, as a reader
 * of the tty would receive them: a signal character ends the case with its
 * signal, and in canonical mode only the lines ended by NL are read.
 */
static void discipline(unsigned modes, struct outcome *out)
{
    unsigned char line[sizeof(out->bytes)];
    int i, n = 0, done = 0;
    unsigned char b;

    for (i = 0; i < out->nbytes; i++) {
        b = out->bytes[i];
        if ((modes & KEYTABLE_ICRNL) && b == '\r') {
            b = '\n';
        }
        if (modes & KEYTABLE_ISIG) {
            int sig = b == VINTR_CHAR   ? SIGINT_NR
                      : b == VQUIT_CHAR ? SIGQUIT_NR
                      : b == VSUSP_CHAR ? SIGTSTP_NR
                                        : 0;

            if (sig != 0) {
                out->kind = KEYTABLE_SIGNAL;
                out->sig = sig;
                out->nbytes = 0;
                return;
            }
        }
        if ((modes & KEYTABLE_ICANON) && b == VERASE_CHAR) {
            if (n > done) {
                n--;
            }
            continue;
        }
        line[n++] = b;
        if (b == '\n') {
            done = n;
        }
    }
    if (modes & KEYTABLE_ICANON) {
        n = done;
    }
    memcpy(out->bytes, line, (size_t)n);
    out->nbytes = n;
}

/** Whether `got` is what case `c` wants. */
static int agrees(const struct keytable_case *c, const struct outcome *got)
{
    switch (c->kind) {
    case KEYTABLE_LOCAL:
        return got->kind == KEYTABLE_BYTES && got->local && got->nbytes == 0;
    case KEYTABLE_SIGNAL:
        return got->kind == KEYTABLE_SIGNAL && got->sig == c->sig;
    case KEYTABLE_BYTES:
    default:
        return got->kind == KEYTABLE_BYTES && !got->local && got->nbytes == c->nbytes &&
               memcmp(got->bytes, c->bytes, (size_t)c->nbytes) == 0;
    }
}

int main(void)
{
    struct keytable t;
    struct keytable_case c;
    struct outcome got;
    char what[512], bytes[256];
    int r, cases = 0;

    if (keytable_open(&t, KEYTABLE_PATH) != 0) {
        fprintf(stderr, "keymap_keytable_hosttest: fail: %s\n", t.error);
        return 1;
    }
    while ((r = keytable_next(&t, &c)) == 1) {
        cases++;
        translate(&c, &got);
        if (c.modes & KEYTABLE_TTY) {
            discipline(c.modes, &got);
        }
        if (agrees(&c, &got)) {
            continue;
        }
        if (++failures <= SHOWN) {
            keytable_describe(&c, what, sizeof(what));
            keytable_quote(got.bytes, got.nbytes, bytes, sizeof(bytes));
            fprintf(stderr, "keymap_keytable_hosttest: fail: %s: the translator gives %s%s\n", what,
                    got.kind == KEYTABLE_SIGNAL ? "a signal" : bytes, got.local ? ", local" : "");
        }
    }
    if (r < 0) {
        fprintf(stderr, "keymap_keytable_hosttest: fail: %s\n", t.error);
        failures++;
    }
    keytable_close(&t);
    if (cases == 0) {
        fprintf(stderr, "keymap_keytable_hosttest: fail: the table gave no case\n");
        failures++;
    }
    if (failures) {
        fprintf(stderr, "keymap_keytable_hosttest: %d of %d cases fail\n", failures, cases);
        return 1;
    }
    printf("keymap_keytable_hosttest: ok: %d cases\n", cases);
    return 0;
}
