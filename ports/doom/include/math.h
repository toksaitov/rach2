/*
 * math.h - Math functions for Doom on Rach2
 * Note: Doom uses fixed-point arithmetic, so most of these are stubs
 */
#ifndef _MATH_H
#define _MATH_H

/* Doom primarily uses fixed-point math, but include some basic stubs */

static inline double fabs(double x) {
    return x < 0 ? -x : x;
}

static inline float fabsf(float x) {
    return x < 0 ? -x : x;
}

static inline double floor(double x) {
    return (double)(int)x - (x < 0 && x != (int)x);
}

static inline double ceil(double x) {
    return (double)(int)x + (x > 0 && x != (int)x);
}

/* These are rarely used by Doom - just return approximations */
static inline double sqrt(double x) {
    /* Newton-Raphson approximation */
    if (x <= 0) return 0;
    double guess = x / 2;
    for (int i = 0; i < 10; i++) {
        guess = (guess + x / guess) / 2;
    }
    return guess;
}

static inline double sin(double x) {
    /* Taylor series approximation - enough for Doom's needs */
    double x3 = x * x * x;
    double x5 = x3 * x * x;
    return x - x3 / 6 + x5 / 120;
}

static inline double cos(double x) {
    double x2 = x * x;
    double x4 = x2 * x2;
    return 1 - x2 / 2 + x4 / 24;
}

static inline double atan2(double y, double x) {
    (void)y; (void)x;
    return 0; /* Stub - Doom uses lookup tables */
}

static inline double pow(double x, double y) {
    (void)x; (void)y;
    return 1; /* Stub */
}

static inline double log(double x) {
    (void)x;
    return 0; /* Stub */
}

static inline double exp(double x) {
    (void)x;
    return 1; /* Stub */
}

#define M_PI 3.14159265358979323846

#endif /* _MATH_H */
