/*
 * sys/time.h - System time functions for Doom on Rach2
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

int gettimeofday(struct timeval *tv, void *tz);

#endif /* _SYS_TIME_H */
