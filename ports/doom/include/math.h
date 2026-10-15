/*
 * math.h: the floating-point functions of Doom on Rach2, inline. The engine
 * computes in fixed point. fabs and fabsf are exact, floor and ceil are
 * exact for values within the range of int, and the rest are approximations
 * or stubs, as each says.
 */
#ifndef _MATH_H
#define _MATH_H

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

/** Ten steps of Newton's method from x / 2; 0 for an `x` of 0 or less. */
static inline double sqrt(double x) {
    if (x <= 0) return 0;
    double guess = x / 2;
    for (int i = 0; i < 10; i++) {
        guess = (guess + x / guess) / 2;
    }
    return guess;
}

/** The Taylor polynomial of sine to x^5, which is close only near 0. */
static inline double sin(double x) {
    double x3 = x * x * x;
    double x5 = x3 * x * x;
    return x - x3 / 6 + x5 / 120;
}

/** The Taylor polynomial of cosine to x^4, which is close only near 0. */
static inline double cos(double x) {
    double x2 = x * x;
    double x4 = x2 * x2;
    return 1 - x2 / 2 + x4 / 24;
}

/** A stub that returns 0. */
static inline double atan2(double y, double x) {
    (void)y; (void)x;
    return 0;
}

/** A stub that returns 1. */
static inline double pow(double x, double y) {
    (void)x; (void)y;
    return 1;
}

/** A stub that returns 0. */
static inline double log(double x) {
    (void)x;
    return 0;
}

/** A stub that returns 1. */
static inline double exp(double x) {
    (void)x;
    return 1;
}

#define M_PI 3.14159265358979323846

#endif /* _MATH_H */
