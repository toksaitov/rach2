/* Owner-side deployment of a prepared local collection. */
#ifndef NATIVE_DEPLOY_H
#define NATIVE_DEPLOY_H
#include "etude/exec.h"
#include <signal.h>
#define ND_PROCESSES 24
struct nd_config {
    const char *mode, *payload, *host, *mac, *slot, *kernel, *key, *target, *run, *logs;
    const char *ssh, *hostpeer, *peer, *network, *interface, *gateway, *far, *empty, *silent;
    const char *architecture;
    char **cases;
    int ncases;
    unsigned echo_port, datagram_port, uid;
    char nonce[65], account[64], archive[4096], archive_sha[65], expected_archive[65],
        kernel_sha[65];
    uint64_t kernel_size;
};
struct nd_process {
    struct etude_proc *process;
    char id[96], out[4096], err[4096];
};
struct nd_deploy {
    struct nd_config c;
    struct etude_exec *executor;
    struct nd_process processes[ND_PROCESSES];
    size_t count;
    struct nd_process *peers[2];
    int cleaning, peer_failed, remote_started;
};
extern volatile sig_atomic_t nd_interrupted;
int nd_options(struct nd_config *c, int argc, char **argv);
int nd_inputs(struct nd_config *c);
int nd_safe(const char *text, const char *allowed);
int nd_format(char *out, size_t size, const char *format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
struct nd_process *nd_start(struct nd_deploy *d, const char *name, char **argv, char **env,
                            const char *input);
int nd_wait(struct nd_deploy *d, struct nd_process *process);
int nd_cleanup(struct nd_deploy *d);
int nd_ssh(struct nd_deploy *d, const char *name, const char *command, const char *input,
           struct nd_process **process);
int nd_identity(struct nd_deploy *d, char *prefix, size_t size);
int nd_stage(struct nd_deploy *d);
int nd_run(struct nd_deploy *d);
int nd_peers(struct nd_deploy *d);
int nd_remote_cleanup(struct nd_deploy *d);
int nd_fetch(struct nd_deploy *d);
#endif
