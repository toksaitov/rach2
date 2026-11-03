/*
 * bell.c: term's audible bell, a soft chime computed at start and played
 * through /dev/dsp, so that it rings at the mixer's level over what other
 * programs play.
 */

/* POSIX's calls, which -std=c99 hides. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "term.h"

/* OSS's requests, as <sys/soundcard.h> has them; the host has no header. */
#define TB_SNDCTL_DSP_SPEED       0xC0045002u
#define TB_SNDCTL_DSP_SETFMT      0xC0045005u
#define TB_SNDCTL_DSP_CHANNELS    0xC0045006u
#define TB_SNDCTL_DSP_SETFRAGMENT 0xC004500Au
#define TB_SNDCTL_DSP_GETODELAY   0x80045017u
#define TB_AFMT_S16_LE            0x00000010

/* 32 periods of 2048 bytes, 64 KiB: room for the whole chime, which
 * /dev/dsp's mix cuts a 16-bit mono period at 48000 Hz to. */
#define TB_FRAGMENT ((32u << 16) | 11u)

#define TB_PI 3.14159265358979323846

/** Milliseconds of the monotonic clock. */
static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/** exp(x) by its series, for the small |x| of one frame's decay. */
static double series_exp(double x)
{
    double sum = 1.0, term = 1.0;
    int n;

    for (n = 1; n < 20; n++) {
        term *= x / n;
        sum += term;
    }
    return sum;
}

/** sin(x) and cos(x) by their series, for |x| below 1. */
static void series_sincos(double x, double *s, double *c)
{
    double ts = x, tc = 1.0;
    int n;

    *s = x;
    *c = 1.0;
    for (n = 1; n < 15; n++) {
        ts *= -x * x / ((2 * n) * (2 * n + 1));
        tc *= -x * x / ((2 * n - 1) * (2 * n));
        *s += ts;
        *c += tc;
    }
}

void term_bell_chime(int16_t *out)
{
    static const double hz[3] = {880.0, 1760.0, 2640.0};
    static const double weight[3] = {1.0, 0.35, 0.12};
    static const double decay[3] = {6.0, 9.0, 14.0};
    double re[3], im[3], step_re[3], step_im[3];
    double v, d, s, c, t;
    unsigned i, n;

    /* Each partial is the imaginary part of z^n, where z turns by the
       partial's angle in a frame and shrinks by its decay, so that no
       library's sin or exp is needed. */
    for (i = 0; i < 3; i++) {
        d = series_exp(-decay[i] / TERM_CHIME_RATE);
        series_sincos(2.0 * TB_PI * hz[i] / TERM_CHIME_RATE, &s, &c);
        step_re[i] = d * c;
        step_im[i] = d * s;
        re[i] = 1.0;
        im[i] = 0.0;
    }
    for (n = 0; n < TERM_CHIME_FRAMES; n++) {
        v = 0.0;
        for (i = 0; i < 3; i++) {
            v += weight[i] * im[i];
            t = re[i] * step_re[i] - im[i] * step_im[i];
            im[i] = re[i] * step_im[i] + im[i] * step_re[i];
            re[i] = t;
        }
        v *= 0.35 / 1.47 * 32767.0;
        /* The attack is 4 ms, 192 frames. */
        if (n < TERM_CHIME_RATE / 250)
            v *= (double)n / (TERM_CHIME_RATE / 250);
        out[n] = (int16_t)(v < 0 ? -(long)(-v + 0.5) : (long)(v + 0.5));
    }
}

void term_bell_init(struct term_bell *b)
{
    b->fd = -1;
    b->sent = 0;
    b->start = 0;
    b->check = 0;
    b->pcm = malloc(TERM_CHIME_FRAMES * sizeof(int16_t));
    if (b->pcm)
        term_bell_chime(b->pcm);
}

/** Close /dev/dsp; the open is nonblocking, so what has not played is
 * dropped and the close does not wait. */
static void bell_stop(struct term_bell *b)
{
    if (b->fd >= 0)
        close(b->fd);
    b->fd = -1;
    b->check = 0;
}

/** Set an OSS parameter and check that it was granted: 0, or -EINVAL. */
static int bell_set(int fd, unsigned long request, int value)
{
    int granted = value;

    if (ioctl(fd, request, &granted) < 0)
        return -errno;
    return granted == value ? 0 : -EINVAL;
}

int term_bell_ring(struct term_bell *b)
{
    int fragment = (int)TB_FRAGMENT;
    int fd;
    int r;

    if (!b->pcm)
        return -ENOMEM;
    if (b->fd >= 0)
        return 0;
    fd = open(TERM_DSP_PATH, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    /* A smaller buffer than asked is fed as it empties. */
    (void)ioctl(fd, TB_SNDCTL_DSP_SETFRAGMENT, &fragment);
    r = bell_set(fd, TB_SNDCTL_DSP_SETFMT, TB_AFMT_S16_LE);
    if (r == 0)
        r = bell_set(fd, TB_SNDCTL_DSP_CHANNELS, 1);
    if (r == 0)
        r = bell_set(fd, TB_SNDCTL_DSP_SPEED, TERM_CHIME_RATE);
    if (r < 0) {
        close(fd);
        return r;
    }
    b->fd = fd;
    b->sent = 0;
    b->start = now_ms();
    b->check = b->start + TERM_CHIME_LIMIT_MS;
    term_bell_service(b);
    return 0;
}

short term_bell_events(const struct term_bell *b)
{
    return b->fd >= 0 && b->sent < TERM_CHIME_FRAMES * sizeof(int16_t) ? POLLOUT : 0;
}

int term_bell_timeout(const struct term_bell *b)
{
    uint64_t now;

    if (b->fd < 0)
        return -1;
    now = now_ms();
    return b->check <= now ? 0 : (int)(b->check - now);
}

void term_bell_service(struct term_bell *b)
{
    size_t bytes = TERM_CHIME_FRAMES * sizeof(int16_t);
    uint64_t now, limit;
    ssize_t n;
    int delay;

    if (b->fd < 0)
        return;
    while (b->sent < bytes) {
        n = write(b->fd, (const char *)b->pcm + b->sent, bytes - b->sent);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            break;
        if (n <= 0) {
            bell_stop(b);
            return;
        }
        b->sent += (size_t)n;
    }
    now = now_ms();
    limit = b->start + TERM_CHIME_LIMIT_MS;
    if (now >= limit) {
        bell_stop(b);
        return;
    }
    if (b->sent < bytes) {
        b->check = limit;
        return;
    }
    /* GETODELAY counts the bytes queued and those the device holds. */
    if (ioctl(b->fd, TB_SNDCTL_DSP_GETODELAY, &delay) < 0 || delay <= 0) {
        bell_stop(b);
        return;
    }
    b->check = now + (uint64_t)delay * 1000u / (TERM_CHIME_RATE * sizeof(int16_t)) + 1;
    if (b->check > limit)
        b->check = limit;
}

void term_bell_close(struct term_bell *b)
{
    bell_stop(b);
    free(b->pcm);
    b->pcm = NULL;
}
