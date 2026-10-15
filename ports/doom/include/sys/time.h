/*
 * sys/time.h: struct timeval and gettimeofday for Doom on Rach2.
 */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

/** The system call, in crt0.c. Returns 0 or a negative errno, and does not
 * set errno. */
int gettimeofday(struct timeval *tv, void *tz);

#endif /* _SYS_TIME_H */
