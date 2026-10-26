/*
 * netcase.c: the operations the network scenarios of tests/cases/machine
 * share, as netcase.h declares them.
 */
#include "netcase.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* The longest assertion's detail. */
#define NET_DETAIL_MAX 160

/** Give the detail of status `st`. */
static const char *status_text(enum etude_status st)
{
    switch (st) {
    case ETUDE_OK:
        return "done";
    case ETUDE_FAILED:
        return "failed";
    case ETUDE_LOST:
        return "the machine or its transport was lost";
    case ETUDE_BOUND:
        return "the step's bound expired";
    case ETUDE_STOPPED:
        return "the case was stopped";
    case ETUDE_REFUSED:
        return "refused: the manifest does not declare it";
    }
    return "unknown";
}

/** Make `format` and `ap` the detail `buf` of `size` bytes, in printable
 * ASCII on one line. */
static void detail(char *buf, size_t size, const char *format, va_list ap)
{
    size_t i;

    vsnprintf(buf, size, format, ap);
    for (i = 0; buf[i] != '\0'; i++) {
        if (buf[i] < ' ' || buf[i] > '~')
            buf[i] = ' ';
    }
}

/** Record that an operation of `u` ended with `st` when it is the first to
 * fail, for the reason `format` makes; give `st`. */
static enum etude_status failed(struct net_case *u, enum etude_status st, const char *format, ...)
{
    va_list ap;

    if (st == ETUDE_OK || u->st != ETUDE_OK)
        return st;
    u->st = st;
    va_start(ap, format);
    detail(u->why, sizeof(u->why), format, ap);
    va_end(ap);
    return st;
}

void net_case(struct net_case *u, struct etude_case *c)
{
    memset(u, 0, sizeof(*u));
    u->c = c;
    u->st = ETUDE_OK;
    if (etude_has_machine(c, "channel-client")) {
        struct etude_termination end;
        u->st = etude_step(c, "channel-client");
        if (u->st == ETUDE_OK)
            u->st = etude_boot(c, "channel-client", NULL, &u->client, &end);
        if (u->st != ETUDE_OK)
            snprintf(u->why, sizeof(u->why), "the declared Rach2 SSH client did not start");
    }
}

int net_ok(const struct net_case *u)
{
    return u->st == ETUDE_OK;
}

void net_fail(struct net_case *u, const char *format, ...)
{
    va_list ap;

    if (u->st != ETUDE_OK)
        return;
    u->st = ETUDE_FAILED;
    va_start(ap, format);
    detail(u->why, sizeof(u->why), format, ap);
    va_end(ap);
}

enum etude_status net_step(struct net_case *u, const char *step)
{
    enum etude_status st;

    if (u->st != ETUDE_OK)
        return u->st;
    st = etude_step(u->c, step);
    return failed(u, st, "step %s: %s", step, status_text(st));
}

void net_assert(struct net_case *u, const char *id, int held, const char *format, ...)
{
    char why[NET_DETAIL_MAX];
    va_list ap;

    if (u->st != ETUDE_OK) {
        if (u->told) {
            snprintf(why, sizeof(why), "not reached: %.140s", u->why);
            etude_notrun(u->c, id, why);
        } else {
            u->told = 1;
            etude_assert(u->c, id, 0, u->why);
        }
        return;
    }
    va_start(ap, format);
    detail(why, sizeof(why), format, ap);
    va_end(ap);
    if (!held)
        u->failures++;
    etude_assert(u->c, id, held, why);
}

int net_end(struct net_case *u)
{
    struct net_made *m;
    int passed = u->st == ETUDE_OK && u->failures == 0;

    while ((m = u->made) != NULL) {
        u->made = m->next;
        if (passed && unlink(m->path) != 0 && errno != ENOENT)
            fprintf(stderr, "netcase: %s: not removed: %s\n", m->path, strerror(errno));
        free(m);
    }
    if (u->st != ETUDE_OK && !u->told)
        fprintf(stderr, "netcase: failed: %s\n", u->why);
    return u->st == ETUDE_OK ? 0 : 1;
}

const char *net_path(struct net_case *u, const char *name, char *buf, size_t size)
{
    const char *path;
    struct net_made *m;

    if (u->st != ETUDE_OK)
        return NULL;
    path = etude_evidence(u->c, name);
    if (path == NULL || strlen(path) >= size || strlen(path) >= sizeof(m->path)) {
        failed(u, ETUDE_FAILED, "evidence %s: no path", name);
        return NULL;
    }
    m = malloc(sizeof(*m));
    if (m == NULL) {
        failed(u, ETUDE_FAILED, "evidence %s: out of memory", name);
        return NULL;
    }
    strcpy(m->path, path);
    m->next = u->made;
    u->made = m;
    strcpy(buf, path);
    return buf;
}

enum etude_status net_boot(struct net_boot *b, struct net_case *u, const char *machine, unsigned n,
                           const char *const disks[])
{
    struct etude_termination end;
    enum etude_status st;
    char name[200];
    const char *path;

    memset(b, 0, sizeof(*b));
    b->u = u;
    if (u->st != ETUDE_OK)
        return u->st;
    snprintf(name, sizeof(name), "%s.%u.console", machine, n);
    path = etude_evidence(u->c, name);
    if (path == NULL || strlen(path) >= sizeof(b->console))
        return failed(u, ETUDE_FAILED, "boot %s: no evidence path %s", machine, name);
    strcpy(b->console, path);
    st = etude_boot(u->c, machine, disks, &b->m, &end);
    return failed(u, st, "boot %s: %s", machine, status_text(st));
}

/** Wait for `text` on `b`'s console after `from`, giving the offset past it
 * in `at`. */
static enum etude_status wait_from(struct net_boot *b, const char *text, uint64_t from,
                                   uint64_t *at)
{
    enum etude_status st;

    if (b->u->st != ETUDE_OK)
        return b->u->st;
    st = etude_console_wait(b->m, text, from, at);
    return failed(b->u, st, "waiting for \"%s\": %s", text, status_text(st));
}

enum etude_status net_wait(struct net_boot *b, const char *text)
{
    return wait_from(b, text, b->at, &b->at);
}

enum etude_status net_seen(struct net_boot *b, const char *text)
{
    uint64_t at;

    return wait_from(b, text, 0, &at);
}

enum etude_status net_times(struct net_boot *b, const char *text, unsigned n, uint64_t *at)
{
    unsigned i;

    *at = 0;
    for (i = 0; i < n; i++) {
        if (wait_from(b, text, *at, at) != ETUDE_OK)
            return b->u->st;
    }
    return b->u->st;
}

/** Type `line` and a carriage return, naming it in a failure as `shown`. */
static enum etude_status type_line(struct net_boot *b, const char *line, const char *shown)
{
    enum etude_status st;
    char text[512];

    if (b->u->st != ETUDE_OK)
        return b->u->st;
    if (strlen(line) + 2 > sizeof(text))
        return failed(b->u, ETUDE_FAILED, "typing \"%s\": the line is too long", shown);
    snprintf(text, sizeof(text), "%s\r", line);
    st = etude_type(b->m, text);
    return failed(b->u, st, "typing \"%s\": %s", shown, status_text(st));
}

enum etude_status net_line(struct net_boot *b, const char *line)
{
    return type_line(b, line, line);
}

enum etude_status net_login(struct net_boot *b, const char *user, const char *password,
                            const char *prompt)
{
    if (net_wait(b, "login:") != ETUDE_OK || net_line(b, user) != ETUDE_OK ||
        net_wait(b, "Password:") != ETUDE_OK || type_line(b, password, "the password") != ETUDE_OK)
        return b->u->st;
    return net_wait(b, prompt);
}

enum etude_status net_command(struct net_boot *b, const char *command, const char *marker,
                              char **text)
{
    uint64_t from;

    *text = NULL;
    if (net_line(b, command) != ETUDE_OK)
        return b->u->st;
    from = b->at;
    if (net_wait(b, marker) != ETUDE_OK)
        return b->u->st;
    *text = net_text(b, from, b->at);
    return ETUDE_OK;
}

enum etude_status net_host(struct net_case *u, char *const argv[], const char *output,
                           struct net_run *r)
{
    enum etude_status st;
    char name[200];
    const char *path;

    memset(r, 0, sizeof(*r));
    r->end.end = ETUDE_END_LOST;
    r->end.value = -1;
    if (u->st != ETUDE_OK)
        return u->st;
    if (strcmp(argv[0], "ssh") == 0 || strcmp(argv[0], "scp") == 0 ||
        strcmp(argv[0], "sftp") == 0 || strcmp(argv[0], "ptydrive") == 0 ||
        strcmp(argv[0], "ssh-keygen") == 0)
        return net_ssh_input(u, argv, NULL, output, r);
    st = etude_host(u->c, argv, output, &r->end);
    if (st != ETUDE_OK)
        return failed(u, st, "%s: %s", argv[0], status_text(st));
    path = etude_evidence(u->c, output);
    r->out = net_file(path != NULL ? path : "");
    snprintf(name, sizeof(name), "%s.err", output);
    path = etude_evidence(u->c, name);
    r->err = net_file(path != NULL ? path : "");
    return ETUDE_OK;
}

/* The most argument words accepted by the guest's invocation. */
#define CLIENT_WORDS 128

/* A translated OpenSSH invocation and an optional sftp return file. */
struct client_command {
    const char *argv[CLIENT_WORDS];
    char *owned[CLIENT_WORDS];
    size_t count;
    char returned[160];
    char evidence[256];
    char key_guest[160], key_host[4096];
};

/** Free the translated words after the channel has copied its invocation. */
static void client_free(struct client_command *a)
{
    size_t i;

    for (i = 0; i < a->count; i++)
        free(a->owned[i]);
}

/** Copy one translated word, keeping it until the invocation is stored. */
static const char *client_word(struct client_command *a, const char *word)
{
    size_t n = strlen(word) + 1;
    char *p = malloc(n);

    if (p == NULL || a->count + 1 >= CLIENT_WORDS) {
        free(p);
        return NULL;
    }
    memcpy(p, word, n);
    a->owned[a->count] = p;
    a->argv[a->count++] = p;
    a->argv[a->count] = NULL;
    return p;
}

/** Put a local input on the Rach2 client and return its temporary path. */
static int client_file(struct net_case *u, const char *from, char path[160])
{
    snprintf(path, 160, "/tmp/etude-client-%u", ++u->client_files);
    if (etude_guest_put(u->client, from, path, "0600") == ETUDE_OK)
        return 1;
    net_fail(u, "the SSH client's input could not be transferred");
    return 0;
}

/** Replace the one get of a scenario's sftp batch with a client-local
 * destination. The finished file is fetched through the framed channel. */
static int client_batch(struct net_case *u, struct client_command *a, const char *from,
                        char path[160])
{
    char *text = net_file(from), command[16], remote[4096], local[4096];
    char evidence[80], batch[4096];
    const char *base;
    FILE *f;
    int ok;

    ok = sscanf(text, "%15s %4095s %4095[^\n]", command, remote, local) == 3 &&
         strcmp(command, "get") == 0 && strchr(remote, '"') == NULL;
    free(text);
    if (!ok) {
        net_fail(u, "the SSH client's sftp batch is not one file retrieval");
        return 0;
    }
    base = strrchr(local, '/');
    base = base != NULL ? base + 1 : local;
    if (strlen(base) >= sizeof(a->evidence)) {
        net_fail(u, "the sftp return evidence name is too long");
        return 0;
    }
    strcpy(a->evidence, base);
    snprintf(a->returned, sizeof(a->returned), "/tmp/etude-return-%u", ++u->client_files);
    snprintf(evidence, sizeof(evidence), "client-batch-%u", u->client_files);
    if (net_path(u, evidence, batch, sizeof(batch)) == NULL)
        return 0;
    f = fopen(batch, "wb");
    if (f == NULL)
        return 0;
    ok = fprintf(f, "get \"%s\" \"%s\"\n", remote, a->returned) > 0;
    if (fclose(f) != 0)
        ok = 0;
    return ok && client_file(u, batch, path);
}

/** Keep a separate known-hosts file for each local path the scenario used.
 * It stays on the private client root, across commands of the attempt. */
static const char *client_hosts(const char *word, char out[160])
{
    struct sha256 h;
    unsigned char d[32];
    char hex[65];

    sha256_init(&h);
    sha256_update(&h, (const unsigned char *)word, strlen(word));
    sha256_final(&h, d);
    sha256_hex(d, hex);
    snprintf(out, 160, "UserKnownHostsFile=/tmp/etude-known-%s", hex);
    return out;
}

static const char *client_program(const char *name)
{
    static const char *const names[] = {"ptydrive", "scp", "sftp", "ssh-keygen"};
    static const char *const paths[] = {"/ptydrive", "/usr/bin/scp", "/usr/bin/sftp",
                                        "/usr/bin/ssh-keygen"};
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i]))
            return paths[i];
    return "/usr/bin/ssh";
}

/** Translate only the client invocation: local file arguments are copied,
 * and its destination reaches the target's host forward through QEMU's
 * gateway. The remote command string is left byte for byte. */
static int client_arguments(struct net_case *u, char *const argv[], struct client_command *a)
{
    struct stat st;
    char path[160], address[512];
    const char *word, *at;
    size_t i;

    memset(a, 0, sizeof(*a));
    if (u->client == NULL) {
        net_fail(u, "the manifest declares no Rach2 SSH client");
        return 0;
    }
    for (i = 0; argv[i] != NULL; i++) {
        word = argv[i];
        if (i == 0)
            word = client_program(word);
        else if (i > 0 && strcmp(argv[i - 1], "-b") == 0) {
            if (!client_batch(u, a, word, path))
                return 0;
            word = path;
        } else if (strncmp(word, "UserKnownHostsFile=", 19) == 0 &&
                   strcmp(word + 19, "/dev/null") != 0) {
            word = client_hosts(word, path);
        } else if ((strcmp(argv[i - 1], "-i") == 0 || strcmp(argv[i - 1], "-f") == 0 ||
                    strcmp(argv[0], "scp") == 0) &&
                   stat(word, &st) == 0 && S_ISREG(st.st_mode)) {
            if (!client_file(u, word, path))
                return 0;
            word = path;
        } else if (strcmp(argv[0], "ssh-keygen") == 0 && strcmp(argv[i - 1], "-f") == 0) {
            if (strlen(word) >= sizeof(a->key_host)) {
                net_fail(u, "the generated key's host path is too long");
                return 0;
            }
            strcpy(a->key_host, word);
            snprintf(a->key_guest, sizeof(a->key_guest), "/tmp/etude-key-%u", ++u->client_files);
            word = a->key_guest;
        } else if ((at = strstr(word, "@127.0.0.1")) != NULL && (at[10] == '\0' || at[10] == ':')) {
            snprintf(address, sizeof(address), "%.*s@10.0.2.2%s", (int)(at - word), word, at + 10);
            word = address;
        }
        if (client_word(a, word) == NULL) {
            net_fail(u, "the SSH client's invocation is too long");
            return 0;
        }
    }
    return 1;
}

/** Give the default environment and the password only to the Rach2
 * terminal driver, which redacts it before writing either transcript. */
static void client_environment(const char *env[5], char password[512])
{
    const char *value = getenv("RACH2_PASSWORD");

    env[0] = "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/";
    env[1] = "HOME=/root";
    env[2] = "TERM=vt100";
    env[3] = NULL;
    env[4] = NULL;
    if (value != NULL) {
        snprintf(password, 512, "RACH2_PASSWORD=%s", value);
        env[3] = password;
    }
}

/** Read a client's result evidence into the ordinary network result. */
static enum etude_status client_result(struct net_case *u, const char *output, enum etude_status st,
                                       const struct etude_guest_result *result, struct net_run *r)
{
    char name[256];
    const char *path;

    r->end = result->termination;
    if (st == ETUDE_OK && (!result->complete || result->cleanup != ETUDE_CLEANUP_CONFIRMED))
        st = ETUDE_LOST;
    if (st != ETUDE_OK)
        return failed(u, st, "Rach2 SSH client: %s", status_text(st));
    path = etude_evidence(u->c, output);
    r->out = net_file(path != NULL ? path : "");
    snprintf(name, sizeof(name), "%s.err", output);
    path = etude_evidence(u->c, name);
    r->err = net_file(path != NULL ? path : "");
    return ETUDE_OK;
}

enum etude_status net_guest(struct net_case *u, struct etude_case_machine *m,
                            const char *const argv[], const char *const env[], const char *input,
                            const char *output, struct net_run *r)
{
    struct etude_guest_result result;
    enum etude_status st;

    memset(r, 0, sizeof(*r));
    r->end.end = ETUDE_END_LOST;
    r->end.value = -1;
    if (!net_ok(u))
        return u->st;
    st = etude_guest_command(m, argv, env, input, output, &result);
    return client_result(u, output, st, &result, r);
}

/** Fetch a generated client key to the scenario's private file. */
static enum etude_status client_key_file(struct net_case *u, const char *guest, const char *host,
                                         const char *evidence)
{
    enum etude_status st = etude_guest_get(u->client, guest, evidence);
    const char *path;
    FILE *in, *out;
    unsigned char bytes[65536];
    size_t n;
    int ok;

    if (st != ETUDE_OK)
        return st;
    path = etude_evidence(u->c, evidence);
    in = path != NULL ? fopen(path, "rb") : NULL;
    out = fopen(host, "wb");
    ok = in != NULL && out != NULL;
    while (ok && (n = fread(bytes, 1, sizeof(bytes), in)) != 0)
        ok = fwrite(bytes, 1, n, out) == n;
    if (in != NULL) {
        if (ferror(in))
            ok = 0;
        fclose(in);
    }
    if (out != NULL) {
        if (fchmod(fileno(out), 0600) != 0)
            ok = 0;
        if (fclose(out) != 0)
            ok = 0;
    }
    return ok ? ETUDE_OK : ETUDE_FAILED;
}

/** Bring both key files back after ssh-keygen's successful exit. */
static enum etude_status client_keys(struct net_case *u, const struct client_command *a)
{
    char guest[180], host[4120], evidence[80];
    enum etude_status st;

    snprintf(evidence, sizeof(evidence), "client-key-%u", u->client_files);
    st = client_key_file(u, a->key_guest, a->key_host, evidence);
    if (st != ETUDE_OK)
        return st;
    snprintf(guest, sizeof(guest), "%s.pub", a->key_guest);
    snprintf(host, sizeof(host), "%s.pub", a->key_host);
    snprintf(evidence, sizeof(evidence), "client-key-%u.pub", u->client_files);
    return client_key_file(u, guest, host, evidence);
}

enum etude_status net_ssh_input(struct net_case *u, char *const argv[], const char *input,
                                const char *output, struct net_run *r)
{
    struct client_command a;
    struct etude_guest_result result;
    const char *env[5];
    char password[512];
    enum etude_status st;

    memset(r, 0, sizeof(*r));
    r->end.end = ETUDE_END_LOST;
    r->end.value = -1;
    if (!net_ok(u))
        return u->st;
    if (!client_arguments(u, argv, &a)) {
        client_free(&a);
        return u->st;
    }
    client_environment(env, password);
    if (input != NULL && strcmp(input, u->client_input) == 0)
        st = etude_guest_command_file(u->client, a.argv, env, "/dev/disk1", output, &result);
    else
        st = etude_guest_command(u->client, a.argv, env, input, output, &result);
    if (st == ETUDE_OK && a.returned[0] != '\0' && result.termination.end == ETUDE_END_EXITED &&
        result.termination.value == 0)
        st = etude_guest_get(u->client, a.returned, a.evidence);
    if (st == ETUDE_OK && a.key_host[0] != '\0' && result.termination.end == ETUDE_END_EXITED &&
        result.termination.value == 0)
        st = client_keys(u, &a);
    client_free(&a);
    return client_result(u, output, st, &result, r);
}

/** Remove only this download's private file after a completed command.
 * Lost channels leave the file to the owned client machine's cleanup. */
static enum etude_status download_remove(struct net_case *u, const char *path, const char *output)
{
    const char *argv[] = {"/bin/rm", "-f", "--", path, NULL};
    struct etude_guest_result result;
    enum etude_status st = etude_guest_command(u->client, argv, NULL, NULL, output, &result);

    if (st == ETUDE_OK &&
        (!result.complete || result.cleanup != ETUDE_CLEANUP_CONFIRMED ||
         result.termination.end != ETUDE_END_EXITED || result.termination.value != 0))
        st = ETUDE_FAILED;
    return st;
}

enum etude_status net_ssh_download(struct net_case *u, char *const argv[], const char *output,
                                   struct net_run *r)
{
    struct client_command a;
    struct etude_guest_result result;
    const char *env[5], *words[CLIENT_WORDS + 6];
    char password[512], path[160], evidence[256], cleanup[256];
    enum etude_status st, removed;
    size_t i;

    memset(r, 0, sizeof(*r));
    r->end.end = ETUDE_END_LOST;
    r->end.value = -1;
    if (!net_ok(u))
        return u->st;
    if (strcmp(argv[0], "ssh") != 0 || strlen(output) > sizeof(evidence) - sizeof(".command.err"))
        return failed(u, ETUDE_REFUSED, "invalid SSH download invocation");
    if (!client_arguments(u, argv, &a)) {
        client_free(&a);
        return u->st;
    }
    if (a.count > CLIENT_WORDS - 6) {
        client_free(&a);
        return failed(u, ETUDE_REFUSED, "the SSH download has too many arguments");
    }
    snprintf(path, sizeof(path), "/tmp/etude-download-%u", ++u->client_files);
    snprintf(evidence, sizeof(evidence), "%s.command", output);
    snprintf(cleanup, sizeof(cleanup), "%s.cleanup", output);
    words[0] = "/bin/sh";
    words[1] = "-c";
    words[2] = "umask 077; set -C; file=$1; shift; exec \"$@\" > \"$file\"";
    words[3] = "etude-download";
    words[4] = path;
    for (i = 0; i < a.count; i++)
        words[i + 5] = a.argv[i];
    words[i + 5] = NULL;
    client_environment(env, password);
    st = etude_guest_command(u->client, words, env, NULL, evidence, &result);
    client_free(&a);
    st = client_result(u, evidence, st, &result, r);
    if (st != ETUDE_OK)
        return st;
    if (net_exited0(r))
        st = etude_guest_get(u->client, path, output);
    removed = download_remove(u, path, cleanup);
    if (st == ETUDE_OK)
        st = removed;
    return failed(u, st, "the SSH file download or its cleanup failed: %s", status_text(st));
}

/** End the owned client before rebinding or reading its raw transfer disk. */
static enum etude_status client_end(struct net_case *u)
{
    struct etude_termination end;
    char *reply = NULL;
    enum etude_status st = etude_qmp(u->client, "quit", NULL, &reply);
    free(reply);
    return st == ETUDE_OK ? etude_wait_end(u->client, &end) : st;
}

/** Check and flush all returned bytes, including zero-filled blocks. */
static int client_disk_stream(FILE *in, FILE *out, uint64_t size, const char *expected)
{
    struct sha256 digest;
    unsigned char block[65536], sum[32];
    char hash[65];
    uint64_t total = 0;
    size_t n;
    sha256_init(&digest);
    while ((n = fread(block, 1, sizeof(block), in)) != 0) {
        total += n;
        if (total > size || fwrite(block, 1, n, out) != n)
            return 0;
        sha256_update(&digest, block, n);
    }
    sha256_final(&digest, sum);
    sha256_hex(sum, hash);
    return !ferror(in) && total == size && !memcmp(hash, expected, 64) && !fflush(out) &&
           !fsync(fileno(out));
}

/** Copy a stopped client's complete disk, checking its declared file digest. */
static int client_disk_copy(const char *source, const char *target, uint64_t size,
                            const char *expected)
{
    struct stat before, after;
    FILE *in = NULL, *out = NULL;
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef _WIN32
    flags |= O_BINARY;
#endif
    if (!expected || strspn(expected, "0123456789abcdef") != 64 || expected[64] != ' ' ||
        lstat(source, &before) || !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
        before.st_size < 0 || (uint64_t)before.st_size != size || !(in = fopen(source, "rb")))
        return 0;
    int fd = open(target, flags, 0600);
    if (fd >= 0)
        out = fdopen(fd, "wb");
    if (!out && fd >= 0)
        close(fd);
    int ok = out && client_disk_stream(in, out, size, expected) && !fstat(fileno(in), &after) &&
             before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
             before.st_size == after.st_size;
    if (out && fclose(out))
        ok = 0;
    if (fclose(in))
        ok = 0;
    if (!ok && fd >= 0)
        unlink(target);
    return ok;
}

/** Return a known-size binary file through the client's existing raw disk. */
enum etude_status net_ssh_download_disk(struct net_case *u, char *const argv[], const char *output,
                                        struct net_run *r)
{
    struct client_command a;
    struct etude_guest_result result;
    struct etude_termination end;
    struct stat disk;
    const char *env[5], *words[CLIENT_WORDS + 8], *target;
    char password[512], path[160], evidence[256], bytes[32];
    enum etude_status st;
    memset(r, 0, sizeof(*r));
    r->end.end = ETUDE_END_LOST;
    r->end.value = -1;
    if (!net_ok(u))
        return u->st;
    if (!u->client || !u->client_input[0] || strcmp(argv[0], "ssh") ||
        stat(u->client_input, &disk) || disk.st_size <= 0 ||
        strlen(output) > sizeof(evidence) - sizeof(".command.err"))
        return failed(u, ETUDE_REFUSED, "the raw SSH download has no declared input disk");
    if (!client_arguments(u, argv, &a)) {
        client_free(&a);
        return u->st;
    }
    snprintf(path, sizeof(path), "/tmp/etude-download-%u", ++u->client_files);
    snprintf(evidence, sizeof(evidence), "%s.command", output);
    snprintf(bytes, sizeof(bytes), "%llu", (unsigned long long)disk.st_size);
    words[0] = "/bin/sh";
    words[1] = "-c";
    words[2] = "umask 077; set -C; file=$1; bytes=$2; shift 2; "
               "trap 'rm -f \"$file\"' EXIT; \"$@\" > \"$file\" || exit; "
               "[ \"$(wc -c < \"$file\")\" = \"$bytes\" ] && "
               "dd if=\"$file\" of=/dev/disk1 bs=65536 conv=notrunc >/dev/null 2>&1 && "
               "sync && sha256sum \"$file\"";
    words[3] = "etude-raw-download";
    words[4] = path;
    words[5] = bytes;
    for (size_t i = 0; i <= a.count; i++)
        words[i + 6] = a.argv[i];
    client_environment(env, password);
    st = etude_guest_command(u->client, words, env, NULL, evidence, &result);
    client_free(&a);
    st = client_result(u, evidence, st, &result, r);
    if (st != ETUDE_OK || !net_exited0(r))
        return st;
    st = client_end(u);
    target = st == ETUDE_OK ? etude_evidence(u->c, output) : NULL;
    if (st == ETUDE_OK &&
        (!target || !client_disk_copy(u->client_input, target, (uint64_t)disk.st_size, r->out)))
        st = ETUDE_FAILED;
    if (st == ETUDE_OK)
        st = etude_boot(u->c, "channel-client", NULL, &u->client, &end);
    return failed(u, st, "the raw SSH return or client restart failed: %s", status_text(st));
}

enum etude_status net_ssh_attach(struct net_case *u, const char *input)
{
    struct etude_termination end;
    enum etude_status st;

    if (!net_ok(u))
        return u->st;
    if (u->client == NULL || input == NULL || strlen(input) >= sizeof(u->client_input))
        return failed(u, ETUDE_REFUSED, "the SSH client has no raw input attachment");
    st = client_end(u);
    if (st == ETUDE_OK)
        st = etude_bind_disk(u->c, "channel-client", "channel-input", input);
    if (st == ETUDE_OK)
        st = etude_boot(u->c, "channel-client", NULL, &u->client, &end);
    if (st == ETUDE_OK)
        strcpy(u->client_input, input);
    return failed(u, st, "the SSH client's raw input disk could not be attached");
}

enum etude_status net_ssh_start(struct net_case *u, char *const argv[], const char *ready)
{
    struct client_command a;
    const char *env[5];
    char password[512];
    enum etude_status st;

    if (!net_ok(u))
        return u->st;
    if (!client_arguments(u, argv, &a)) {
        client_free(&a);
        return u->st;
    }
    client_environment(env, password);
    st = etude_guest_start(u->client, a.argv, env, NULL);
    client_free(&a);
    if (st == ETUDE_OK)
        st = etude_guest_output_wait(u->client, ready);
    return failed(u, st, "the Rach2 SSH workload did not start: %s", status_text(st));
}

enum etude_status net_ssh_finish(struct net_case *u, const char *output, struct net_run *r)
{
    struct etude_guest_result result;
    enum etude_status st;

    memset(r, 0, sizeof(*r));
    st = etude_guest_finish(u->client, 1, output, &result);
    return client_result(u, output, st, &result, r);
}

/** Quote one word for the guest shell, preserving every byte. */
static char *client_quote(const char *word)
{
    size_t n = strlen(word), at = 0, i;
    char *quoted;

    if (n > (SIZE_MAX - 3) / 4 || (quoted = malloc(4 * n + 3)) == NULL)
        return NULL;
    quoted[at++] = '\'';
    for (i = 0; i < n; i++) {
        if (word[i] == '\'') {
            memcpy(quoted + at, "'\\''", 4);
            at += 4;
        } else
            quoted[at++] = word[i];
    }
    quoted[at++] = '\'';
    quoted[at] = '\0';
    return quoted;
}

/** Build su's command and its separate output/status markers. */
static char *su_command(const char *command, const char *token)
{
    char *word = client_quote(command), *body, *quoted, *remote;
    size_t size;

    if (word == NULL)
        return NULL;
    size = strlen(word) + 2 * strlen(token) + 256;
    body = malloc(size);
    if (body == NULL) {
        free(word);
        return NULL;
    }
    snprintf(body, size,
             "{ printf '\\n%%s\\n' '%s-begin'; /bin/sh -c %s 2>/dev/null; "
             "accept_status=$?; printf '\\n%%s %%s\\n' '%s-end' \"$accept_status\"; } | cat",
             token, word, token);
    free(word);
    quoted = client_quote(body);
    free(body);
    if (quoted == NULL)
        return NULL;
    size = strlen(quoted) + 8;
    remote = malloc(size);
    if (remote != NULL)
        snprintf(remote, size, "su -c %s", quoted);
    free(quoted);
    return remote;
}

/** Extract the root command's bytes and status from the terminal driver. */
static int su_result(struct net_run *r, const char *token)
{
    char begin[96], end[96], *first, *last, *digits, *text;
    unsigned long status;
    size_t n;

    if (!net_exited0(r) || net_count(r->out, "PROMPTED=1") != 1 ||
        net_count(r->out, "ANSWERED=1") != 1)
        return 0;
    snprintf(begin, sizeof(begin), "\n%s-begin\n", token);
    snprintf(end, sizeof(end), "\n%s-end ", token);
    first = strstr(r->out, begin);
    first = first != NULL ? first + strlen(begin) : NULL;
    last = first != NULL ? strstr(first, end) : NULL;
    if (last == NULL)
        return 0;
    status = strtoul(last + strlen(end), &digits, 10);
    if (status > 255 || digits == last + strlen(end) || *digits != '\n')
        return 0;
    n = (size_t)(last - first);
    text = malloc(n + 1);
    if (text == NULL)
        return 0;
    memcpy(text, first, n);
    text[n] = '\0';
    free(r->out);
    r->out = text;
    r->end.end = ETUDE_END_EXITED;
    r->end.value = (int)status;
    return 1;
}

enum etude_status net_su(struct net_case *u, const struct net_login *login, const char *command,
                         const char *output, struct net_run *r)
{
    struct client_command a;
    struct etude_guest_result result;
    const char *env[5];
    char password[512], who[128], token[64];
    char *remote;
    enum etude_status st;
    unsigned tries;
    char *argv[] = {"ptydrive", "--rows",
                    "40",       "--cols",
                    "120",      "--",
                    "ssh",      "-tt",
                    "-i",       (char *)login->key,
                    "-p",       (char *)login->port,
                    "-o",       "StrictHostKeyChecking=no",
                    "-o",       "UserKnownHostsFile=/dev/null",
                    "-o",       "BatchMode=yes",
                    who,        NULL,
                    NULL};

    memset(r, 0, sizeof(*r));
    r->end.end = ETUDE_END_LOST;
    r->end.value = -1;
    if (!net_ok(u))
        return u->st;
    snprintf(token, sizeof(token), "RACH2_SU_%u", ++u->client_files);
    remote = su_command(command, token);
    if (remote == NULL)
        return failed(u, ETUDE_FAILED, "the root command could not be quoted");
    argv[19] = remote;
    snprintf(who, sizeof(who), "%s@127.0.0.1", login->user);
    if (!client_arguments(u, argv, &a)) {
        free(remote);
        client_free(&a);
        return u->st;
    }
    free(remote);
    client_environment(env, password);
    snprintf(password, sizeof(password), "RACH2_PASSWORD=%s", login->password);
    env[3] = password;
    for (tries = 0;; tries++) {
        st = etude_guest_command(u->client, a.argv, env, NULL, output, &result);
        st = client_result(u, output, st, &result, r);
        if (st != ETUDE_OK || tries == 3 || net_count(r->out, "PROMPTED=0") != 1 ||
            (strstr(r->out, "kex_exchange_identification") == NULL &&
             strstr(r->out, "ssh_exchange_identification") == NULL &&
             strstr(r->out, "Connection reset by") == NULL &&
             strstr(r->out, "Connection refused") == NULL &&
             strstr(r->out, "Connection timed out") == NULL))
            break;
        net_run_free(r);
        {
            const char *pause[] = {"/bin/sleep", "1", NULL};
            struct etude_guest_result waited;
            st = etude_guest_command(u->client, pause, NULL, NULL, "su-retry", &waited);
            if (st != ETUDE_OK)
                break;
        }
    }
    client_free(&a);
    if (st == ETUDE_OK && !su_result(r, token))
        return failed(u, ETUDE_FAILED,
                      "su did not answer one prompt and report its command's status");
    return st;
}

void net_run_free(struct net_run *r)
{
    free(r->out);
    free(r->err);
    r->out = NULL;
    r->err = NULL;
}

int net_exited0(const struct net_run *r)
{
    return r->end.end == ETUDE_END_EXITED && r->end.value == 0;
}

const char *net_secret(struct net_case *u, const char *name, char *buf, size_t size)
{
    const char *path;
    char *text;
    size_t n;

    buf[0] = '\0';
    if (u->st != ETUDE_OK)
        return buf;
    path = etude_artifact(u->c, name);
    if (path == NULL) {
        failed(u, ETUDE_REFUSED, "artifact %s: %s", name, status_text(ETUDE_REFUSED));
        return buf;
    }
    text = net_file(path);
    n = strcspn(text, "\r\n");
    if (n == 0 || n >= size) {
        failed(u, ETUDE_FAILED, "artifact %s: no password on its first line", name);
    } else {
        memcpy(buf, text, n);
        buf[n] = '\0';
    }
    free(text);
    return buf;
}

const char *net_key(struct net_case *u, const char *name, char *buf, size_t size)
{
    const char *from;
    char *text;
    size_t n;
    int fd;

    if (u->st != ETUDE_OK)
        return NULL;
    from = etude_artifact(u->c, name);
    if (from == NULL) {
        failed(u, ETUDE_REFUSED, "artifact %s: %s", name, status_text(ETUDE_REFUSED));
        return NULL;
    }
    if (net_path(u, name, buf, size) == NULL)
        return NULL;
    text = net_file(from);
    fd = open(buf, O_WRONLY | O_CREAT | O_EXCL, 0600);
    n = strlen(text);
    if (fd < 0 || n == 0 || write(fd, text, n) != (ssize_t)n) {
        failed(u, ETUDE_FAILED, "artifact %s: no private copy: %s", name,
               fd < 0 ? strerror(errno) : "a short write");
        buf = NULL;
    }
    if (fd >= 0)
        close(fd);
    free(text);
    return buf;
}

/** Read at most `max` bytes of the file at `path` from `from`, into a text
 * from malloc. */
static char *read_part(const char *path, uint64_t from, uint64_t max)
{
    FILE *f = fopen(path, "rb");
    char *text;
    size_t n = 0, got;

    text = malloc((size_t)max + 1);
    if (text == NULL)
        abort();
    if (f != NULL && fseek(f, (long)from, SEEK_SET) == 0) {
        while (n < max && (got = fread(text + n, 1, (size_t)max - n, f)) > 0)
            n += got;
    }
    if (f != NULL)
        fclose(f);
    text[n] = '\0';
    return text;
}

char *net_text(const struct net_boot *b, uint64_t from, uint64_t to)
{
    return read_part(b->console, from, to > from ? to - from : 0);
}

char *net_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size = -1;

    if (f != NULL && fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (f != NULL)
        fclose(f);
    return read_part(path, 0, size > 0 ? (uint64_t)size : 0);
}

/** Say whether `pattern` matches the whole of `s` of `n` bytes, from its
 * start. */
static int match_here(const char *s, size_t n, const char *pattern)
{
    size_t i;

    if (*pattern == '\0')
        return 1;
    if (*pattern == '*') {
        for (i = 0; i <= n; i++) {
            if (match_here(s + i, n - i, pattern + 1))
                return 1;
        }
        return 0;
    }
    if (*pattern == '#') {
        for (i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++) {
            if (match_here(s + i + 1, n - i - 1, pattern + 1))
                return 1;
        }
        return 0;
    }
    return n > 0 && *s == *pattern && match_here(s + 1, n - 1, pattern + 1);
}

/** Say whether line `line` of `len` bytes has `pattern` in it. */
static int match(const char *line, size_t len, const char *pattern)
{
    size_t i;

    for (i = 0; i <= len; i++) {
        if (match_here(line + i, len - i, pattern))
            return 1;
    }
    return 0;
}

const char *net_find(const char *text, const char *pattern)
{
    const char *p;
    size_t len;

    if (text == NULL)
        return NULL;
    for (p = text; *p != '\0'; p += strspn(p, "\r\n")) {
        len = strcspn(p, "\r\n");
        if (match(p, len, pattern))
            return p;
        p += len;
    }
    return NULL;
}

unsigned net_count(const char *text, const char *pattern)
{
    unsigned n = 0;
    const char *p;
    size_t len;

    if (text == NULL)
        return 0;
    for (p = text; *p != '\0'; p += strspn(p, "\r\n")) {
        len = strcspn(p, "\r\n");
        if (match(p, len, pattern))
            n++;
        p += len;
    }
    return n;
}

const char *net_copy_line(const char *line, char *buf, size_t size)
{
    size_t n = 0;

    if (size == 0)
        return buf;
    while (line != NULL && line[n] != '\0' && line[n] != '\r' && line[n] != '\n' && n + 1 < size) {
        buf[n] = line[n];
        n++;
    }
    buf[n] = '\0';
    return buf;
}

long net_number_after(const char *text, const char *prefix)
{
    const char *p, *last = NULL;

    if (text == NULL)
        return -1;
    for (p = strstr(text, prefix); p != NULL; p = strstr(p + 1, prefix))
        last = p;
    if (last == NULL)
        return -1;
    last += strlen(prefix);
    if (*last < '0' || *last > '9')
        return -1;
    return strtol(last, NULL, 10);
}
