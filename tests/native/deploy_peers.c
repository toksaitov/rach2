/* The two peers remain external to Rach2 and belong to this deployment. */
#define _POSIX_C_SOURCE 200809L
#include "deploy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern char **environ;
static char **peer_environment(const char *address)
{
    size_t n = 0, used = 0;
    char **result;
    while (environ[n] != NULL)
        n++;
    result = calloc(n + 2, sizeof(*result));
    if (result == NULL)
        return NULL;
    for (n = 0; environ[n] != NULL; n++)
        if (strncmp(environ[n], "HOSTPEER_ADDRESS=", 17))
            result[used++] = environ[n];
    result[used] = malloc(strlen(address) + 18);
    if (result[used] == NULL) {
        free(result);
        return NULL;
    }
    sprintf(result[used], "HOSTPEER_ADDRESS=%s", address);
    return result;
}
static int start_peer(struct nd_deploy *d, unsigned which)
{
    char text[16];
    char *argv[] = {(char *)d->c.hostpeer, which ? "send-repeat" : "echo", text,
                    which ? "outside" : "0", NULL};
    char **env = peer_environment(which ? d->c.host : d->c.peer);
    const char *ready = which ? "hostpeer: repeating " : "hostpeer: echoing ";
    size_t n;
    int ok;
    if (env == NULL)
        return 0;
    snprintf(text, sizeof(text), "%u", which ? d->c.datagram_port : d->c.echo_port);
    d->peers[which] = nd_start(d, which ? "datagram-peer" : "echo-peer", argv, env, NULL);
    ok = d->peers[which] != NULL;
    for (n = 0; env[n] != NULL; n++)
        if (!strncmp(env[n], "HOSTPEER_ADDRESS=", 17))
            free(env[n]);
    free(env);
    while (ok && !nd_interrupted) {
        const struct etude_exec_result *r = etude_proc_result(d->peers[which]->process);
        if (r->finished_us || d->peer_failed)
            return 0;
        if (strstr(r->out.data, ready) != NULL)
            return 1;
        if (etude_exec_wait(d->executor, 250) != NULL)
            return 0;
    }
    return 0;
}
int nd_peers(struct nd_deploy *d)
{
    return start_peer(d, 0) && start_peer(d, 1);
}
