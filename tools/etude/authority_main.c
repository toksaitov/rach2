/* Fixed installed-policy entry and one capability-bound endpoint phase. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "authority_internal.h"
#include "authority_run.h"
#include "routine_proof.h"
#include "measurement_context.h"
#ifdef _WIN32
#include "win/files.h"
#endif

static int self(struct etude_pid *identity)
{
#ifdef _WIN32
    long pid = GetCurrentProcessId();
#else
    long pid = getpid();
#endif
    return etude_process_state(pid, identity) == 1 && identity->start_known ? 0 : -1;
}

static int capability(struct etude_authority_phase *p, const char *run, const char *commit)
{
    memset(p, 0, sizeof *p);
    p->run_id = run;
    p->commit = commit;
    p->nonce = getenv("ETUDE_AUTHORITY_NONCE");
    static char input_nonce[65];
    if (!p->nonce) {
        for (size_t i = 0; i < sizeof input_nonce; i++) {
            char byte;
            if (read(STDIN_FILENO, &byte, 1) != 1 ||
                (i < 64 ? !byte || !strchr("0123456789abcdef", byte) : byte != '\n'))
                return -1;
            input_nonce[i] = i == 64 ? 0 : byte;
        }
        p->nonce = input_nonce;
        if (setenv("ETUDE_AUTHORITY_NONCE", p->nonce, 1))
            return -1;
    }
    const char *owner = getenv("ETUDE_AUTHORITY_OWNER");
    if (!owner)
        return self(&p->owner);
    char *end;
    errno = 0;
    p->owner.pid = strtol(owner, &end, 10);
    if (errno || end == owner || *end != ':' || p->owner.pid <= 1)
        return -1;
    owner = end + 1;
    p->owner.start = strtoull(owner, &end, 10);
    p->owner.start_known = 1;
    return errno || end == owner || *end || !p->owner.start || !authority_owner_live(&p->owner) ? -1
                                                                                                : 0;
}

static int command_phase(int argc, char **argv)
{
    /* The nonce is inherited from the protected owner, never on argv. */
    if (argc < 7 || (strcmp(argv[4], "producer") && strcmp(argv[4], "consumer")))
        return 2;
    struct etude_authority_phase phase;
    struct etude_authority *authority = NULL;
    if (capability(&phase, argv[2], argv[3]))
        return 125;
    phase.phase = argv[4];
    if (etude_authority_reopen(&phase, &authority) || etude_authority_allow(authority, &phase))
        goto failed;
    /* A child acquires its own shared gate before its real ledger claim.
     * The phase owner blocks the endpoint again only after child cleanup. */
    etude_authority_close(authority);
    authority = NULL;
    if (setenv("ETUDE_AUTHORITY_RUN", phase.run_id, 1) ||
        setenv("ETUDE_AUTHORITY_PHASE", phase.phase, 1))
        return 125;
    struct authority_run run = {.phase = phase.phase,
                                .receipt = argv[5],
                                .argv = argv + 6,
                                .watch_input = getenv("ETUDE_AUTHORITY_INPUT") != NULL};
    struct etude_exec_result result = {0};
    int status = authority_run_command(&run, &result);
    if (result.cleanup != ETUDE_CLEANUP_CONFIRMED || etude_authority_reopen(&phase, &authority) ||
        etude_authority_block(authority))
        goto failed;
    etude_authority_close(authority);
    return status;
failed:
    fprintf(stderr, "etude authority: endpoint remains blocked: %s\n", strerror(errno));
    etude_authority_close(authority);
    return 125;
}

static int command_switch(int argc, char **argv)
{
    if (argc != 4 && !(argc == 5 && (!strcmp(argv[1], "permit") || !strcmp(argv[1], "physical"))))
        return 2;
    struct etude_authority_phase phase;
    struct etude_authority *authority = NULL;
    if (capability(&phase, argv[2], argv[3]))
        return 125;
    int status;
    if (!strcmp(argv[1], "physical")) {
        if (argc != 5)
            return 2;
        struct etude_reading grant;
        struct etude_context_clock clock;
        status = etude_context_clock(&clock) || etude_authority_physical(&phase, 0, &grant);
        if (!status) {
            struct jbuf b = {0};
            jbuf_puts(&b, "{\"machine_id\":");
            routine_quote(&b, clock.machine_id);
            jbuf_puts(&b, ",\"boot_id\":");
            routine_quote(&b, clock.boot_id);
            jbuf_puts(&b, ",\"reading\":");
            routine_reading(&b, &grant);
            jbuf_puts(&b, "}\n");
            status = routine_create(argv[4], &b);
            jbuf_free(&b);
        }
    } else if (!strcmp(argv[1], "freeze"))
        status = etude_authority_freeze(&phase, &authority);
    else {
        status = etude_authority_reopen(&phase, &authority);
        if (!status && !strcmp(argv[1], "permit")) {
            phase.phase = argv[4];
            status = etude_authority_allow(authority, &phase);
        } else if (!status && !strcmp(argv[1], "block"))
            status = etude_authority_block(authority);
        else if (!status)
            status = etude_authority_restore(authority);
    }
    if (status)
        fprintf(stderr, "etude authority: %s refused: %s\n", argv[1], strerror(errno));
    etude_authority_close(authority);
    return status ? 125 : 0;
}

static int command_launch(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    struct authority_binding binding;
    if (authority_binding_read(&binding))
        return 125;
    struct etude_authority *authority = NULL;
    if (etude_authority_open(binding.ledger, &authority) || !authority ||
        etude_authority_claim(authority, "launch", NULL, 0)) {
        fprintf(stderr, "etude authority: normal endpoint admission is blocked\n");
        etude_authority_close(authority);
        return 125;
    }
    /* This is the installed immutable body, not the caller's checkout.
     * The shared gate spans its policy read and every owned descendant. */
#ifdef _WIN32
    const char *shell = etude_win_shell();
#else
    const char *shell = "/bin/sh";
#endif
    char **args = calloc((size_t)argc + 1, sizeof *args);
    if (!shell || !args) {
        free(args);
        etude_authority_close(authority);
        return 125;
    }
    args[0] = (char *)shell;
    args[1] = binding.launcher_body;
    memcpy(args + 2, argv + 2, (size_t)(argc - 2) * sizeof *args);
    struct authority_run run = {.phase = "launcher", .argv = args, .forward_input = 1};
    struct etude_exec_result result;
    int status = authority_run_command(&run, &result);
    free(args);
    etude_authority_close(authority);
    return status;
}

static int command_facts(int argc, char **argv)
{
    if (argc != 5 || !authority_absolute(argv[4]))
        return 2;
    struct etude_authority_phase phase;
    struct etude_authority *a = NULL;
    if (capability(&phase, argv[2], argv[3]) || etude_authority_reopen(&phase, &a))
        return 125;
    const char *names[] = {"binding", "state", "policy-before", "policy-disabled", "ledger-empty"};
    char claims[4096], targets[5][4096];
    int n = snprintf(claims, sizeof claims, "%s/claims", a->binding.ledger);
    int error = n < 0 || (size_t)n >= sizeof claims || strcmp(a->state.phase, "blocked");
    const char *sources[] = {authority_installed_binding(), a->binding.state, a->state.snapshot,
                             a->binding.site, claims};
    for (size_t i = 0; !error && i < 5; i++) {
        n = snprintf(targets[i], sizeof targets[i], "%s/%s", argv[4], names[i]);
        error = n < 0 || (size_t)n >= sizeof targets[i] || routine_copy(sources[i], targets[i]);
    }
    struct authority_file_id gate = {0};
    if (!error)
        error = authority_file_id(a->binding.gate, &gate);
    struct jbuf b = {0};
    jbuf_puts(&b, "{\"binding\":");
    if (!error)
        error = routine_ref(&b, targets[0]);
    const char *keys[] = {
        ",\"state\":", ",\"policy_before\":", ",\"policy_disabled\":", ",\"ledger_empty\":"};
    for (size_t i = 1; !error && i < 5; i++) {
        jbuf_puts(&b, keys[i - 1]);
        error = routine_ref(&b, targets[i]);
    }
    jbuf_puts(&b, ",\"snapshot\":");
    if (!error)
        error = routine_ref(&b, a->state.snapshot);
    jbuf_puts(&b, ",\"gate\":{\"path\":");
    routine_quote(&b, a->binding.gate);
    char ids[180];
    snprintf(ids, sizeof ids, ",\"identity\":{\"device\":\"%llu\",\"inode\":\"%llu\"}}}\n",
             (unsigned long long)gate.device, (unsigned long long)gate.inode);
    jbuf_puts(&b, ids);
    char output[4096];
    n = snprintf(output, sizeof output, "%s/facts.json", argv[4]);
    if (!error)
        error = n < 0 || (size_t)n >= sizeof output || routine_create(output, &b);
    if (!error && fwrite(b.data, 1, b.len, stdout) != b.len)
        error = 1;
    jbuf_free(&b);
    etude_authority_close(a);
    return error ? 125 : 0;
}

static int command_helper(int argc, char **argv)
{
    if (argc < 5)
        return 2;
    struct authority_binding binding;
    if (authority_binding_read(&binding) || !authority_operator(binding.operator_id))
        return 125;
    struct authority_run run = {.phase = "routine-helper",
                                .argv = argv + 2,
                                .forward_input = getenv("ETUDE_ROUTINE_INPUT") != NULL};
    struct etude_exec_result result = {0};
    return authority_run_command(&run, &result);
}

static int command_proxy(int argc, char **argv)
{
#ifdef _WIN32
    (void)argc;
    (void)argv;
    return 2;
#else
    if (argc < 4 || setenv("ETUDE_ROUTINE_INPUT", "pipe", 1))
        return 2;
    struct authority_run run = {
        .phase = "physical-owner", .argv = argv + 2, .dedicated_input = 1, .clear_parent = 1};
    struct etude_exec_result result = {0};
    return authority_run_command(&run, &result);
#endif
}

static int command_clock(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    struct etude_context_clock clock;
    struct etude_reading now;
    if (etude_context_clock(&clock) || etude_history_now(&now))
        return 125;
    struct jbuf b = {0};
    jbuf_puts(&b, "{\"machine_id\":");
    routine_quote(&b, clock.machine_id);
    jbuf_puts(&b, ",\"boot_id\":");
    routine_quote(&b, clock.boot_id);
    jbuf_puts(&b, ",\"reading\":");
    routine_reading(&b, &now);
    jbuf_puts(&b, "}\n");
    int error = routine_create(argv[2], &b);
    jbuf_free(&b);
    return error ? 125 : 0;
}

static int policy_field(const char *bytes, const char *name, const char *value)
{
    char prefix[80];
    int n = snprintf(prefix, sizeof prefix, "\n%s ", name);
    if (n < 0 || (size_t)n >= sizeof prefix)
        return 0;
    const char *at = strstr(bytes, prefix);
    if (!at || strstr(at + 1, prefix))
        return 0;
    at += n;
    const char *end = strchr(at, '\n');
    return end && (size_t)(end - at) == strlen(value) && !memcmp(at, value, strlen(value));
}

static int transport_policy(struct etude_authority *a, char **argv)
{
    struct authority_file_id expected, supplied;
    if (authority_file_id(a->binding.site, &expected) || authority_file_id(argv[2], &supplied) ||
        expected.device != supplied.device || expected.inode != supplied.inode)
        return -1;
    char *bytes = NULL;
    size_t size;
    uint64_t capacity[3];
    int enabled;
    if (authority_policy(&a->binding, a->binding.site, &bytes, &size, capacity, &enabled))
        return -1;
    int error = !enabled || !policy_field(bytes, "ledger", argv[3]) ||
                !policy_field(bytes, "locks", argv[4]) || !policy_field(bytes, "capacity", argv[5]);
    const char *seconds = strstr(bytes, "\nseconds ");
    char *end;
    unsigned long bound = strtoul(argv[6], &end, 10);
    error |= !bound || *end || !seconds || bound > 86400;
    if (!error) {
        unsigned long actual = strtoul(seconds + 9, &end, 10);
        error = *end != '\n' || !actual || actual > 86400 || bound > actual;
    }
    free(bytes);
    return error ? -1 : 0;
}

static int command_transport(int argc, char **argv)
{
    struct etude_authority *authority = NULL;
    if (argc < 8 || etude_authority_open(argv[3], &authority) || !authority ||
        etude_authority_claim(authority, "producer-transport", NULL, 0) ||
        transport_policy(authority, argv)) {
        etude_authority_close(authority);
        return 125;
    }
    struct authority_run run = {
        .phase = "producer-transport", .argv = argv + 7, .forward_input = 1};
    struct etude_exec_result result = {0};
    int status = authority_run_command(&run, &result);
    etude_authority_close(authority);
    return status;
}

static int command_route(int argc, char **argv)
{
    struct authority_binding binding;
    if (argc != 3 || authority_route(argv[2], getenv("ETUDE_ROUTINE_ROUTE"), &binding))
        return 125;
    return 0;
}

int etude_authority_main(int argc, char **argv)
{
    if (argc > 1) {
        if (!strcmp(argv[1], "transport"))
            return command_transport(argc, argv);
        if (!strcmp(argv[1], "route"))
            return command_route(argc, argv);
        if (!strcmp(argv[1], "clock"))
            return command_clock(argc, argv);
        if (!strcmp(argv[1], "proxy"))
            return command_proxy(argc, argv);
        if (!strcmp(argv[1], "helper"))
            return command_helper(argc, argv);
        if (!strcmp(argv[1], "facts"))
            return command_facts(argc, argv);
        if (!strcmp(argv[1], "launch"))
            return command_launch(argc, argv);
        if (!strcmp(argv[1], "phase"))
            return command_phase(argc, argv);
        if (!strcmp(argv[1], "freeze") || !strcmp(argv[1], "restore") ||
            !strcmp(argv[1], "physical") || !strcmp(argv[1], "permit") || !strcmp(argv[1], "block"))
            return command_switch(argc, argv);
    }
    fprintf(stderr, "usage: etude authority freeze|restore RUN COMMIT\n"
                    "       etude authority phase RUN COMMIT producer|consumer RECEIPT COMMAND...\n"
                    "       etude authority launch LAUNCH_ARGUMENTS...\n");
    return 2;
}
