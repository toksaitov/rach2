/*
 * socket_scenario.c: the scenario of the socket cases (../machine.socket.json),
 * udptest and tcptest, each in a boot of its own against hostpeer's echo
 * server, which the guest reaches at the user network's gateway.
 */
#include <stdio.h>
#include <string.h>

#include "console_report.h"

/* The debug shell's banner, after which it reads commands. */
#define SHELL_READY "Type 'help' for available commands."

/* What hostpeer says when a peer's FIN ends the stream it reads. */
#define PEER_CLOSED "hostpeer: tcp: the peer closed its end"

/**
 * Start the echo server on the case's port until it is stopped, boot the
 * test image, run the probe that the case's identity names at the debug
 * shell with the port, and take its report from the console. For tcptest,
 * require the server to have read the end of the stream that the probe's
 * close sent, which the probe cannot see from inside the guest.
 */
int etude_scenario(struct etude_case *c)
{
    const char *probe = strrchr(etude_case_id(c), '.');
    struct etude_case_service *echo;
    struct etude_case_machine *m;
    struct etude_termination end;
    char port[12], command[64];
    char *args[4];
    enum etude_status st;
    uint64_t at;

    if (probe == NULL || (strcmp(probe, ".udptest") != 0 && strcmp(probe, ".tcptest") != 0)) {
        fprintf(stderr, "socket_scenario: %s is no case of machine.socket\n", etude_case_id(c));
        return 1;
    }
    probe++;
    snprintf(port, sizeof(port), "%u", etude_port(c, 0));
    args[0] = "echo";
    args[1] = port;
    args[2] = "0";
    args[3] = NULL;
    if (etude_step(c, "serve") != ETUDE_OK ||
        etude_service(c, "echo", args, &echo, &end) != ETUDE_OK)
        return 1;
    if (etude_step(c, "boot") != ETUDE_OK || etude_boot(c, "guest", NULL, &m, &end) != ETUDE_OK ||
        etude_console_wait(m, SHELL_READY, 0, &at) != ETUDE_OK)
        return 1;
    snprintf(command, sizeof(command), "exec /%s %s\r", probe, port);
    if (etude_step(c, probe) != ETUDE_OK || etude_type(m, command) != ETUDE_OK ||
        console_report(c, m, "guest", 1, at, NULL) != ETUDE_OK)
        return 1;
    if (strcmp(probe, "tcptest") != 0)
        return 0;
    if (etude_step(c, "close") != ETUDE_OK)
        return 1;
    st = etude_service_wait(echo, PEER_CLOSED, 0, &at);
    if (st == ETUDE_STOPPED)
        return 1;
    etude_assert(c, "peer-closed", st == ETUDE_OK,
                 "the echo server never read the end of the stream");
    return etude_service_stop(echo, &end) == ETUDE_OK ? 0 : 1;
}
