/*
 * signal.h: the signal numbers, with Linux's values, for Doom on Rach2.
 */
#ifndef _SIGNAL_H
#define _SIGNAL_H

#define SIGINT  2
#define SIGTERM 15
#define SIGSEGV 11

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

/** A stub: installs nothing and returns SIG_DFL. */
static inline sighandler_t signal(int signum, sighandler_t handler) {
    (void)signum; (void)handler;
    return SIG_DFL;
}

#endif /* _SIGNAL_H */
