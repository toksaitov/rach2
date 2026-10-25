/* One attempt's child subreaper; the program keeps its own PID and pipes. */
#ifndef ETUDE_GUEST_OWNER_H
#define ETUDE_GUEST_OWNER_H
#include <stddef.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/types.h>
struct guest_owner {
    pid_t pid;
    unsigned long long start;
    int enabled, control, events, reaped, ended, complete, clean, fault;
    int status;
    struct rusage usage;
    unsigned char message[sizeof(struct rusage) + 64];
    size_t used;
};
/* Like fork: zero in the real program, its PID in the responder, or -1. */
pid_t guest_owner_fork(struct guest_owner *owner, uint64_t grace_us, const char *directory);
/* Read complete owner events without blocking; 1 after the program's end. */
int guest_owner_read(struct guest_owner *owner);
void guest_owner_signal(struct guest_owner *owner, int signo);
void guest_owner_reaped(struct guest_owner *owner, int status);
int guest_owner_wait(struct guest_owner *owner, uint64_t bound_us);
/* A private durable proof is valid only for this exact reaped owner. */
int guest_owner_confirm(const char *directory, pid_t pid, unsigned long long start);
void guest_owner_close(struct guest_owner *owner);
#endif
