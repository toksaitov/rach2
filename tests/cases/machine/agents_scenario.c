/* agents_scenario.c: agent sessions and fake APIs on private GNU roots.
 * Etude owns each machine, forwarded port and scripted server. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "netcase.h"
#include "agent_native.h"
#include "agent_accept.h"

/* The state shared by the SSH workload's actions. */
struct agent_case {
    struct net_case u;
    struct net_boot b;
    struct etude_case_service *server;
    char work[4096];
    char verdict[4096]; /* the server exit status written before it ends */
    char root_key[4096];
    char test_key[4096];
    char row[100];
    unsigned starts;
};

/** Run an SSH workload action and report its assertion, if one is given.
 * The scripts' output stays in the evidence and is printed. */
static void action(struct agent_case *s, const char *verb, const char *assertion)
{
    int ok = 0;
    if (net_ok(&s->u)) {
        if (!strncmp(etude_case_id(s->u.c), "machine.agentsaccept.", 21))
            ok = agent_accept_action(&s->u, s->work, s->row, verb);
        else
            ok = agent_native_action(&s->u, &s->b, s->work, s->row, verb,
                                     !strncmp(etude_case_id(s->u.c), "machine.fakeapi.", 16));
    }
    if (!ok)
        net_fail(&s->u, "%s failed; see its output", verb);
    if (assertion != NULL)
        net_assert(&s->u, assertion, ok, "%s failed; see its output", verb);
}

/** Start a script on the case's allocated API port and wait for its
 * listening record. The script and certificates belong to this case. */
static void serve(struct agent_case *s, const char *script, const char *record)
{
    char path[4096], log[4096], ca[4096], port[16];
    struct etude_termination end;
    char *args[] = {"serve", path, log, ca, port, NULL};

    if (snprintf(path, sizeof(path), "%s/%s.json", s->work, script) >= (int)sizeof(path) ||
        snprintf(log, sizeof(log), "%s/%s", s->work, record) >= (int)sizeof(log) ||
        snprintf(ca, sizeof(ca), "%s/ca", s->work) >= (int)sizeof(ca) ||
        snprintf(s->verdict, sizeof(s->verdict), "%s/%s.status", s->work, record) >=
            (int)sizeof(s->verdict)) {
        net_fail(&s->u, "the service path is too long");
        return;
    }
    snprintf(port, sizeof(port), "%u", etude_port(s->u.c, 1));
    if (net_ok(&s->u) && etude_service(s->u.c, "fakeapi", args, &s->server, &end) != ETUDE_OK)
        net_fail(&s->u, "the fake API service did not become ready");
    s->starts++;
}

/** Stop the server and judge its final verdict. A stop may report the
 * supervisor's stopped end, so its own result line and exit status are
 * read from the evidence the wrapper writes. */
static void unserve(struct agent_case *s, const char *assertion, int refused)
{
    struct etude_termination end;
    char name[80], *text = NULL, *status = NULL;
    int ok = 0;

    if (net_ok(&s->u) && etude_service_stop(s->server, &end) == ETUDE_OK) {
        snprintf(name, sizeof(name), "fakeapi.%u.out", s->starts);
        text = net_file(etude_evidence(s->u.c, name));
        fputs(text, stdout);
        status = net_file(s->verdict);
        ok = strstr(text,
                    refused
                        ? "fakeapi: result: fail: 1 failed, the first request 5: GET /v1/nothing: "
                        : "fakeapi: result: pass:") != NULL;
    }
    ok = ok && status != NULL && strcmp(status, refused ? "1\n" : "0\n") == 0;
    if (!ok)
        net_fail(&s->u, "the server's verdict did not match the exchange");
    net_assert(&s->u, assertion, ok, "the server's verdict did not match the exchange");
    free(status);
    free(text);
}

/** Boot the same private disk, and witness its root and init's server. */
static void boot(struct agent_case *s, unsigned number)
{
    net_boot(&s->b, &s->u, "gnu", number, NULL);
    net_wait(&s->b, "ext2: root: /dev/disk0p1");
    net_wait(&s->b, "init: sshd: started as pid");
    if (net_ok(&s->u)) {
        char port[16];
        struct net_run r;
        char *args[] = {"sshpeer", "wait", port, NULL};
        snprintf(port, sizeof(port), "%u", etude_port(s->u.c, 0));
        if (net_host(&s->u, args, "ssh-ready", &r) == ETUDE_OK) {
            if (!net_exited0(&r))
                net_fail(&s->u, "the guest SSH server did not answer");
            net_run_free(&r);
        }
    }
}

/** Run one agent row and its host verifier with the API service beside it. */
static void row(struct agent_case *s, int fakeapi)
{
    char record[200];

    net_step(&s->u, "prepare");
    action(s, "prepare", "fixture");
    if (fakeapi)
        snprintf(record, sizeof(record), "record");
    else
        snprintf(record, sizeof(record), "row.%s.record", s->row);
    net_step(&s->u, "server");
    serve(s, "script", record);
    net_step(&s->u, "boot");
    boot(s, 1);
    net_assert(&s->u, "boot", 1, "");
    net_step(&s->u, "setup");
    action(s, "setup", "ssh");
    net_step(&s->u, "workload");
    action(s, fakeapi ? "client" : "row", "application");
    net_step(&s->u, "verdict");
    unserve(s, "server-verdict", fakeapi);
    net_step(&s->u, "verify");
    action(s, "verify", "record");
}

/** Exercise one interactive agent, then boot its disk again and check the
 * persisted credentials, versions and authenticated requests. */
static void acceptance(struct agent_case *s)
{
    struct net_run r;
    struct etude_termination end;
    char port[16];
    char *off[] = {NET_SSH, NET_SSH_KEY(s->root_key), "-p", port, "root@127.0.0.1", "reboot", NULL};

    net_step(&s->u, "prepare");
    action(s, "prepare", "fixture");
    net_step(&s->u, "boot");
    boot(s, 1);
    net_assert(&s->u, "boot", 1, "");
    net_step(&s->u, "setup");
    action(s, "setup", "credentials-before");
    net_step(&s->u, "session");
    serve(s, "session", "session.record");
    action(s, "session", "session");
    unserve(s, "session-verdict", 0);
    net_step(&s->u, "reset");
    snprintf(port, sizeof(port), "%u", etude_port(s->u.c, 0));
    if (net_host(&s->u, off, "poweroff", &r) == ETUDE_OK)
        net_run_free(&r);
    memset(&end, 0, sizeof(end));
    if (net_ok(&s->u) && etude_wait_end(s->b.m, &end) != ETUDE_OK)
        net_fail(&s->u, "the guest did not reboot");
    net_assert(&s->u, "reset", end.end == ETUDE_END_RESET,
               "the guest did not end with QEMU's reset event");
    net_step(&s->u, "reboot");
    boot(s, 2);
    net_assert(&s->u, "reboot", 1, "");
    net_step(&s->u, "credentials");
    action(s, "credentials", "credentials-after");
    net_step(&s->u, "after");
    serve(s, "after", "after.record");
    action(s, "after", "authenticated-after");
    unserve(s, "after-verdict", 0);
    net_step(&s->u, "gh");
    serve(s, "gh", "gh.record");
    action(s, "gh", "gh-after");
    unserve(s, "gh-verdict", 0);
    net_step(&s->u, "finish");
    action(s, "finish", "redaction");
}

/** Select the workload by its case identity and give it only private keys,
 * state and allocated ports. Stop a service left by a failed action before
 * returning, so its scripted end is observed. Etude cleans every owned
 * process on return. */
int etude_scenario(struct etude_case *c)
{
    struct agent_case s;
    const char *id = etude_case_id(c);
    int accept = strncmp(id, "machine.agentsaccept.", 21) == 0;
    int fake = strncmp(id, "machine.fakeapi.", 16) == 0;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    snprintf(s.row, sizeof(s.row), "%s", strrchr(id, '.') + 1);
    snprintf(s.work, sizeof(s.work), "%s", etude_evidence(c, "work"));
    if (mkdir(s.work, 0700) != 0)
        net_fail(&s.u, "could not make the workload directory");
    net_key(&s.u, "demo-key", s.root_key, sizeof(s.root_key));
    net_key(&s.u, "test-key", s.test_key, sizeof(s.test_key));
    if (accept) {
        acceptance(&s);
    } else {
        row(&s, fake);
    }
    if (!net_ok(&s.u) && s.server != NULL) {
        struct etude_termination end;
        etude_service_stop(s.server, &end);
    }
    if (net_ok(&s.u) && s.u.failures == 0) {
        char *clean[] = {"agent_server", "remove", s.work, NULL};
        struct net_run r;
        if (net_host(&s.u, clean, "remove-work", &r) == ETUDE_OK)
            net_run_free(&r);
    }
    return net_end(&s.u);
}
