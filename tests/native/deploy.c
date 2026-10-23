/* Native owner-side deployment. This program does not judge case output. */
#define _POSIX_C_SOURCE 200809L
#include "deploy.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
static void interrupted(int signal)
{
    nd_interrupted = signal;
}
static int nonce(char text[65])
{
    static const char digits[] = "0123456789abcdef";
    unsigned char bytes[32];
    size_t at = 0, i;
    int file = open("/dev/urandom", O_RDONLY);
    ssize_t count;
    if (file < 0)
        return 0;
    while (at < sizeof(bytes)) {
        count = read(file, bytes + at, sizeof(bytes) - at);
        if (count <= 0) {
            close(file);
            return 0;
        }
        at += (size_t)count;
    }
    close(file);
    for (i = 0; i < sizeof(bytes); i++) {
        text[2 * i] = digits[bytes[i] >> 4];
        text[2 * i + 1] = digits[bytes[i] & 15];
    }
    text[64] = '\0';
    return 1;
}
static int prepare(struct nd_deploy *d)
{
    struct sigaction action;
    struct utsname host;
    char path[4096];
    FILE *file;
    memset(&action, 0, sizeof(action));
    action.sa_handler = interrupted;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    if (uname(&host) || strcmp(host.sysname, "Darwin")) {
        fputs("native-deploy: deployment runs from the owner's Mac\n", stderr);
        return 0;
    }
    if (!nonce(d->c.nonce) || mkdir(d->c.logs, 0700) ||
        !nd_format(path, sizeof(path), "%s/target", d->c.logs))
        return 0;
    file = fopen(path, "w");
    if (file == NULL)
        return 0;
    fprintf(file,
            "host %s mac %s slot %s kernel %s sha256 %s target %s run %s owner %s archive %s\n",
            d->c.host, d->c.mac, d->c.slot, d->c.kernel, d->c.kernel_sha, d->c.target, d->c.run,
            d->c.nonce, d->c.archive_sha);
    if (fclose(file))
        return 0;
    d->executor = etude_exec_new();
    return d->executor != NULL;
}
int main(int argc, char **argv)
{
    struct nd_deploy deployment, *d = &deployment;
    int status = 1, cleanup = 1;
    memset(d, 0, sizeof(*d));
    if (!nd_options(&d->c, argc, argv)) {
        fputs("usage: native-deploy stage|run|proof PAYLOAD HOST SLOT KERNEL KEY TARGET RUN LOGS "
              "[--peer IPV4 --echo-port PORT --hostpeer FILE --network ADDRESS/PREFIX "
              "--gateway IPV4 --far IPV4 --empty IPV4 --silent IPV4] [-- CASE...]\n",
              stderr);
        return 2;
    }
    if (!nd_inputs(&d->c) || !prepare(d)) {
        fprintf(stderr,
                "native-deploy: invalid preparation, identity, key permissions or logs directory; "
                "evidence %s\n",
                d->c.logs);
        return nd_interrupted ? 128 + nd_interrupted : 2;
    }
    if (!strcmp(d->c.mode, "stage")) {
        status = nd_stage(d) ? 0 : 1;
        cleanup = status == 0;
    } else if (nd_peers(d)) {
        status = nd_run(d);
        for (unsigned n = 0; n < 2; n++)
            if (etude_proc_result(d->peers[n]->process)->finished_us)
                status = 1;
        d->cleaning = 1;
        cleanup = nd_remote_cleanup(d);
        if (cleanup)
            cleanup = nd_fetch(d);
    }
    if (d->remote_started && !d->cleaning) {
        d->cleaning = 1;
        cleanup = nd_remote_cleanup(d);
    }
    cleanup &= nd_cleanup(d);
    printf("native-deploy: result status %d cleanup %s; evidence %s\n", status,
           cleanup ? "confirmed" : "unconfirmed", d->c.logs);
    return nd_interrupted ? 128 + nd_interrupted : status == 0 && cleanup ? 0 : 1;
}
