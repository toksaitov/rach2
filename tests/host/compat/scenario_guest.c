/* A framed stand-in for the scenario test's Windows serial peer. The real
 * guest row still executes the Rach2 responder and /hello under QEMU. */
#ifdef _WIN32
#include "win/files.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scenario_host.h"
#include "scenario_guest.h"
#include "history_json.h"
#include "guest/wire.h"
#include "etude/frame.h"
#include "sha256.h"

struct actor {
    host_socket socket;
    char session[17], boot[17], pid[24], start[24];
    uint64_t sent, received;
    char name[256], digest[65];
    unsigned char document[1u << 20];
    size_t size;
    uint64_t expected;
};

static int send_bytes(host_socket socket, const void *data, size_t size)
{
    const char *at = data;
    int n;
    while (size > 0) {
        n = host_socket_write(socket, at, size);
        if (n <= 0)
            return -1;
        at += n;
        size -= (size_t)n;
    }
    return 0;
}

/* Use the production codec and frame bound, including acknowledgments. */
static int event(struct actor *a, const char *type, int count, const char *const *fields)
{
    char line[ETUDE_FRAME_SERIAL_MAX + 1];
    size_t n = wire_encode(line, sizeof(line), a->session, a->boot, a->sent, a->received != 0,
                           a->received - 1, type, count, fields);
    if (n == 0 || send_bytes(a->socket, line, n) != 0)
        return -1;
    if (strcmp(type, ETUDE_CMD_ACK) != 0 && strcmp(type, ETUDE_EV_ALIVE) != 0)
        a->sent++;
    return 0;
}

/* Hash bytes independently before declaring the stream's size and digest. */
static void digest(const void *bytes, size_t size, char hex[65])
{
    struct sha256 hash;
    unsigned char raw[32];
    sha256_init(&hash);
    sha256_update(&hash, bytes, size);
    sha256_final(&hash, raw);
    sha256_hex(raw, hex);
}

static int stream(struct actor *a, const char *cmd, const char *attempt, const char *fd,
                  const unsigned char *data, size_t size)
{
    char hex[65], length[24];
    const char *fields[] = {cmd, attempt, fd, length, length, hex};
    digest(data, size, hex);
    snprintf(length, sizeof(length), "%llu", (unsigned long long)size);
    return event(a, ETUDE_EV_STREAM, 6, fields);
}

static int output(struct actor *a, const struct wire_frame *frame, const char *fd,
                  const unsigned char *bytes, size_t size, size_t *offset)
{
    char base64[65], position[24];
    const char *fields[] = {frame->field[0], frame->field[1], fd, position, base64};
    size_t count;
    while (size > 0) {
        count = size > 36 ? 36 : size;
        snprintf(position, sizeof(position), "%llu", (unsigned long long)*offset);
        wire_base64(bytes, count, base64);
        if (event(a, ETUDE_EV_OUT, 5, fields) != 0)
            return -1;
        *offset += count;
        bytes += count;
        size -= count;
    }
    return 0;
}

struct recording {
    unsigned char out[65536], err[65536];
    size_t out_size, err_size;
};

static int error_output(struct actor *a, const struct wire_frame *frame, const char *path,
                        struct recording *record)
{
    FILE *file = fopen(path, "rb");
    size_t offset = 0;
    int failed;
    if (file == NULL)
        return -1;
    record->err_size = fread(record->err, 1, sizeof(record->err), file);
    failed = ferror(file) || fgetc(file) != EOF;
    fclose(file);
    return failed ? -1 : output(a, frame, "2", record->err, record->err_size, &offset);
}

static int finish_run(struct actor *a, const struct wire_frame *frame, int status,
                      const struct recording *record)
{
    char value[24];
    const char *fields[] = {frame->field[0], frame->field[1], "exited", value, "-", "-"};
    snprintf(value, sizeof(value), "%d", status);
    if (event(a, ETUDE_EV_EXIT, 4, fields) != 0 ||
        stream(a, frame->field[0], frame->field[1], "1", record->out, record->out_size) != 0 ||
        stream(a, frame->field[0], frame->field[1], "2", record->err, record->err_size) != 0)
        return -1;
    fields[2] = fields[3] = "-";
    if (event(a, ETUDE_EV_USAGE, 6, fields) != 0)
        return -1;
    fields[2] = "confirmed";
    return event(a, ETUDE_EV_CLEANED, 3, fields);
}

/* The actor executes the actual native report or killer program. No verdict
 * is synthesized from the requested invocation or from a cached pass. */
static int execute(struct actor *a, const struct wire_frame *f)
{
    struct eh_json *document = NULL;
    const struct eh_json *arguments, *item;
    struct sh_child child;
    char program[4096], error[4096], pid[24];
    char *argv[32];
    unsigned char bytes[8192];
    struct recording record;
    const char *fields[] = {f->field[0], f->field[1], pid, "-", "-", "-"};
    size_t n = 1, offset = 0;
    int got, value;
    memset(&record, 0, sizeof(record));
    a->document[a->size] = '\0';
    document = eh_json_parse((const char *)a->document);
    if (document == NULL)
        return -1;
    item = eh_json_get(document, "program");
    if (item == NULL || item->type != 's' || strchr(item->string, '/') != NULL ||
        strchr(item->string, '\\') != NULL) {
        eh_json_free(document);
        return -1;
    }
    snprintf(program, sizeof(program), "%s/%s", getenv("ETUDE_FAKE_STAGE"), item->string);
    snprintf(error, sizeof(error), "%s.actor.err", getenv("ETUDE_FAKE_GUEST_PID"));
    argv[0] = program;
    arguments = eh_json_get(document, "arguments");
    for (item = arguments != NULL ? arguments->child : NULL; item != NULL; item = item->next) {
        if (item->type != 's' || n + 1 >= sizeof(argv) / sizeof(argv[0])) {
            eh_json_free(document);
            return -1;
        }
        argv[n++] = item->string;
    }
    argv[n] = NULL;
    if (sh_start(&child, program, argv, NULL, error, 1) != 0) {
        eh_json_free(document);
        return -1;
    }
    snprintf(pid, sizeof(pid), "%ld", child.pid);
    value = event(a, ETUDE_EV_STARTED, 4, fields);
    while (value == 0 && (got = sh_read(&child, bytes, sizeof(bytes))) >= 0) {
        if (got == 0) {
            host_sleep_ms(1);
            continue;
        }
        if (record.out_size + (size_t)got > sizeof(record.out)) {
            value = -1;
            break;
        }
        memcpy(record.out + record.out_size, bytes, (size_t)got);
        record.out_size += (size_t)got;
        value = output(a, f, "1", bytes, (size_t)got, &offset);
    }
    got = sh_wait(&child);
    if (value == 0)
        value = error_output(a, f, error, &record);
    if (value == 0)
        value = finish_run(a, f, got, &record);
    eh_json_free(document);
    return value;
}

/* A staged executable is verified rather than transferred over serial. */
static int staged_digest(const struct actor *a, char hex[65])
{
    char path[4096];
    FILE *file;
    struct sha256 hash;
    unsigned char raw[32], data[8192];
    size_t count;
    uint64_t total = 0;
    int failed;
    if (strchr(a->name, '/') != NULL || strchr(a->name, '\\') != NULL)
        return -1;
    snprintf(path, sizeof(path), "%s/%s", getenv("ETUDE_FAKE_STAGE"), a->name);
    file = fopen(path, "rb");
    if (file == NULL)
        return -1;
    sha256_init(&hash);
    while ((count = fread(data, 1, sizeof(data), file)) > 0) {
        sha256_update(&hash, data, count);
        total += count;
    }
    failed = ferror(file) || total != a->expected;
    fclose(file);
    sha256_final(&hash, raw);
    sha256_hex(raw, hex);
    return failed ? -1 : 0;
}

static int seal(struct actor *a, const char *cmd)
{
    char hex[65];
    const char *fields[] = {cmd, a->name, a->digest};
    if (a->size == 0 && a->expected > 0) {
        if (staged_digest(a, hex) != 0)
            return -1;
    } else {
        digest(a->document, a->size, hex);
        if (a->size != a->expected)
            return -1;
    }
    return strcmp(hex, a->digest) == 0 ? event(a, ETUDE_EV_STORED, 3, fields) : -1;
}

static int command(struct actor *a, const struct wire_frame *f)
{
    const char *fields[5] = {f->field[0], NULL, NULL, NULL, NULL};
    unsigned char bytes[256];
    uint64_t size, offset;
    long n;
    if (strcmp(f->type, ETUDE_CMD_HELLO) == 0 && f->nfields == 3) {
        memcpy(a->session, f->session, sizeof(a->session));
        fields[1] = "1";
        fields[2] = "255";
        fields[3] = a->pid;
        fields[4] = a->start;
        return event(a, ETUDE_EV_READY, 5, fields);
    }
    if (strcmp(f->type, ETUDE_CMD_PUT) == 0 && f->nfields == 5 && wire_number(f->field[2], &size) &&
        size <= (UINT64_C(1) << 30)) {
        snprintf(a->name, sizeof(a->name), "%s", f->field[1]);
        snprintf(a->digest, sizeof(a->digest), "%s", f->field[3]);
        a->size = 0;
        a->expected = size;
        return event(a, ETUDE_CMD_ACK, 0, NULL);
    }
    if (strcmp(f->type, ETUDE_CMD_DATA) == 0 && f->nfields == 3 &&
        wire_number(f->field[1], &offset) && offset == a->size) {
        n = strlen(f->field[2]) < 340 ? wire_unbase64(f->field[2], bytes) : -1;
        if (n < 0 ||
            (a->size + (size_t)n > a->expected || a->size + (size_t)n >= sizeof(a->document)))
            return -1;
        memcpy(a->document + a->size, bytes, (size_t)n);
        a->size += (size_t)n;
        return event(a, ETUDE_CMD_ACK, 0, NULL);
    }
    if (strcmp(f->type, ETUDE_CMD_SEAL) == 0 && f->nfields == 1)
        return seal(a, f->field[0]);
    if (strcmp(f->type, ETUDE_CMD_RUN) == 0 && f->nfields == 3 &&
        strcmp(f->field[2], a->digest) == 0)
        return execute(a, f);
    if (strcmp(f->type, ETUDE_CMD_BYE) == 0)
        return event(a, ETUDE_CMD_BYE, 1, fields) == 0 ? 1 : -1;
    return -1;
}

static int receive(struct actor *a)
{
    char line[ETUDE_FRAME_SERIAL_MAX + 1], byte;
    struct wire_frame frame;
    size_t at = 0;
    int result;
    while (host_socket_read(a->socket, &byte, 1) == 1) {
        if (byte != '\n') {
            if (at >= sizeof(line) - 1)
                return -1;
            line[at++] = byte;
            continue;
        }
        if (wire_parse(line, at, &frame) != 0)
            return -1;
        at = 0;
        if (strcmp(frame.type, ETUDE_CMD_ACK) == 0)
            continue;
        if (frame.seq != a->received) {
            if (event(a, ETUDE_CMD_ACK, 0, NULL) != 0)
                return -1;
            continue;
        }
        a->received++;
        result = command(a, &frame);
        if (result != 0)
            return result > 0 ? 0 : -1;
    }
    return -1;
}

int scenario_guest_actor(const char *port)
{
    struct actor *a = calloc(1, sizeof(*a));
    host_socket connection = host_qemu_connect(port);
    const char *fields[3];
    int result = -1;
    if (a == NULL || connection < 0)
        goto done;
    a->socket = sh_receive_socket(connection);
    if (a->socket < 0)
        goto done;
    snprintf(a->session, sizeof(a->session), "%s", ETUDE_FRAME_NO_SESSION);
    snprintf(a->boot, sizeof(a->boot), "%016llx", (unsigned long long)host_clock_us());
    snprintf(a->pid, sizeof(a->pid), "%lu", GetCurrentProcessId());
    snprintf(a->start, sizeof(a->start), "%llu", (unsigned long long)host_clock_us());
    fields[0] = "1";
    fields[1] = a->pid;
    fields[2] = a->start;
    if (event(a, ETUDE_EV_ALIVE, 3, fields) == 0)
        result = receive(a);
    host_socket_close(a->socket);
done:
    if (connection >= 0)
        host_socket_close(connection);
    free(a);
    return result == 0 ? 0 : 1;
}

long scenario_guest_start(const char *self, host_socket console)
{
    struct sh_child child;
    unsigned short port = 0;
    host_socket listener = (host_socket)etude_win_listen(&port), connection;
    char text[16];
    char *argv[] = {(char *)self, "--scenario-guest", text, getenv("ETUDE_FAKE_STAGE"), NULL};
    long pid = -1;
    snprintf(text, sizeof(text), "%u", port);
    if (listener >= 0 && sh_start(&child, self, argv, NULL, NULL, 0) == 0) {
        connection = (host_socket)etude_win_accept((SOCKET)listener, INFINITE);
        if (connection >= 0 && sh_send_socket(connection, console) == 0)
            pid = child.pid;
        else
            sh_signal(child.pid, SIGKILL);
        if (connection >= 0)
            host_socket_close(connection);
        CloseHandle(child.output);
        CloseHandle(child.process);
    }
    if (listener >= 0)
        host_socket_close(listener);
    return pid;
}

/* QEMU's command relay uses pipes on Windows, so its input and its socket
 * are read on separate threads. The two directions keep their half-closes. */
static DWORD WINAPI relay_input(void *argument)
{
    host_socket socket = *(host_socket *)argument;
    char bytes[4096];
    int n;
    while ((n = _read(0, bytes, sizeof(bytes))) > 0)
        if (send_bytes(socket, bytes, (size_t)n) != 0)
            break;
    shutdown((SOCKET)socket, SD_SEND);
    return 0;
}

int scenario_relay(const char *port)
{
    host_socket socket = host_qemu_connect(port);
    HANDLE thread;
    char bytes[4096];
    int n, at, count, result = 0;
    if (socket < 0)
        return 1;
    thread = CreateThread(NULL, 0, relay_input, &socket, 0, NULL);
    if (thread == NULL) {
        host_socket_close(socket);
        return 1;
    }
    while ((n = host_socket_read(socket, bytes, sizeof(bytes))) > 0)
        for (at = 0; at < n; at += count) {
            count = _write(1, bytes + at, n - at);
            if (count <= 0) {
                result = 1;
                goto done;
            }
        }
done:
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    host_socket_close(socket);
    return result;
}
#endif
