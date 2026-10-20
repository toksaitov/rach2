/*
 * noqemu.c: the QEMU launcher of etude/qemu.h on Windows, where etude has
 * none yet. Its launcher has no machine and refuses to start one, so that
 * the supervisor, which keeps a launcher beside its executor, runs host and
 * remote attempts on Windows. It defines the functions the supervisor calls.
 */
#include <errno.h>
#include <stdlib.h>

#include "etude/qemu.h"

struct etude_qemu {
    int machines; /* always 0 */
};

struct etude_qemu *etude_qemu_new(void)
{
    return calloc(1, sizeof(struct etude_qemu));
}

void etude_qemu_free(struct etude_qemu *q)
{
    free(q);
}

/** Refuse the machine of `spec`: Windows has no launcher yet. */
int etude_qemu_start(struct etude_qemu *q, const struct etude_machine_spec *spec,
                     struct etude_machine **m)
{
    (void)q;
    (void)spec;
    *m = NULL;
    errno = ENOSYS;
    return -1;
}

/** Return at once, since no machine runs, as the POSIX launcher does. */
struct etude_machine *etude_qemu_wait(struct etude_qemu *q, int timeout_ms)
{
    (void)q;
    (void)timeout_ms;
    return NULL;
}

void etude_machine_cancel(struct etude_machine *m)
{
    (void)m;
}

long etude_machine_qmp(struct etude_machine *m, const char *execute, const char *arguments)
{
    (void)m;
    (void)execute;
    (void)arguments;
    errno = EPIPE;
    return -1;
}

int etude_machine_reply(const struct etude_machine *m, long id, const char **reply)
{
    (void)m;
    (void)id;
    (void)reply;
    return -1;
}

const struct etude_machine_result *etude_machine_result(const struct etude_machine *m)
{
    (void)m;
    return NULL;
}

void etude_machine_free(struct etude_machine *m)
{
    (void)m;
}
