/*
 * time.h - Time functions for Doom on Rach2
 */
#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>

typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 100

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

static inline time_t time(time_t *t) {
    struct timeval {
        long tv_sec;
        long tv_usec;
    };
    extern int gettimeofday(struct timeval *tv, void *tz);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (t) *t = tv.tv_sec;
    return tv.tv_sec;
}

static inline clock_t clock(void) {
    struct timeval {
        long tv_sec;
        long tv_usec;
    };
    extern int gettimeofday(struct timeval *tv, void *tz);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * CLOCKS_PER_SEC + tv.tv_usec / 10000;
}

static inline struct tm *localtime(const time_t *timep) {
    static struct tm tm;
    (void)timep;
    return &tm;
}

static inline char *ctime(const time_t *timep) {
    (void)timep;
    return "Unknown time\n";
}

#endif /* _TIME_H */
