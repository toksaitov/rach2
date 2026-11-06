/*
 * qemu.c: the QEMU launcher on POSIX systems. Each machine has a private
 * directory with two listening Unix sockets, to which QEMU connects as a
 * client: its serial console and its QMP monitor. QEMU runs as an attempt of
 * the launcher's host executor, stopped until QMP has confirmed the
 * accelerator, and the end of the machine is told from QMP's events and the
 * end of QEMU's process.
 */
#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#endif
#include <unistd.h>

#ifdef _WIN32
#include "../win/files.h"
#include "../win/sockets.h"
#else
typedef int etude_socket;
/* A POSIX socket keeps what has arrived until it is read, and needs no
 * receive of the launcher's own. */
struct socket_in {
    int fd; /* the connection it reads, or -1 */
};
#define socket_read(in, fd, buf, n) read(fd, buf, n)
#define socket_write                write
#define socket_close                close

static void socket_in_start(struct socket_in *in, int fd)
{
    in->fd = fd;
}

static void socket_in_stop(struct socket_in *in, int fd)
{
    (void)fd;
    in->fd = -1;
}

static void socket_in_pump(struct socket_in *in, int fd)
{
    (void)in;
    (void)fd;
}
#endif

#include "../capture.h"
#include "../history_json.h"
#include "etude/qemu.h"
#include "posix.h"

/* The longest QMP line kept, such as the registers of 64 processors; a
 * longer one is counted and skipped. The buffer grows from QMP_LINE_FIRST. */
#define QMP_LINE_MAX   (1u << 20)
#define QMP_LINE_FIRST 4096

/* The bytes kept of QEMU's own standard output and error. */
#define QEMU_OUTPUT_MAX 65536

/* The most console bytes one pass of the loop reads of a machine. */
#define CONSOLE_PASS_MAX 65536
#define CONSOLE_PUMP_MAX (64u * 1024 * 1024) /* a pump past this stops reading */

/* The bytes kept for QEMU's command line. */
#define QEMU_LINE_BYTES 16384

/* Where QMP is in the launcher's start of the machine. */
enum qmp_state {
    QMP_GREETING, /* waiting for QEMU's greeting */
    QMP_CAPS,     /* qmp_capabilities sent */
    QMP_ACCEL,    /* query-accelerators sent */
    QMP_KVM,      /* query-kvm sent, for a QEMU without query-accelerators */
    QMP_CONT,     /* cont sent */
    QMP_READY,    /* the machine runs */
    QMP_QUIT,     /* quit sent */
    QMP_GONE      /* the connection is closed or unusable */
};

/* The ids of the launcher's QMP commands, which their replies repeat. */
enum { ID_CAPS = 1, ID_ACCEL, ID_KVM, ID_CONT, ID_QUIT };

/* A command of the caller's, and its reply once it has come. */
struct qmp_call {
    struct qmp_call *next;
    long id;
    char *reply; /* the whole line, or NULL */
};

struct etude_machine {
    struct etude_qemu *q;
    struct etude_machine *next;
    char *attempt_id;
    enum etude_accel accel;
#ifdef _WIN32
    char dir[4096];
#else
    char dir[sizeof(((struct sockaddr_un *)0)->sun_path) - 2];
#endif /* with room \
                                                                  for "/c" */
    char console_path[128];
    char qmp_path[128];
    etude_socket console_listen, qmp_listen; /* -1 once QEMU has connected */
    etude_socket console, qmp;               /* QEMU's connections; -1 once closed */
    struct socket_in console_in, qmp_in;     /* their reading sides */
    int console_file;
#ifndef _WIN32
    struct console_pump *pump; /* keeps the console socket read; NULL before */
#endif
    char *audio_path;
    int audio_created, audio_temporary;
    struct etude_proc *proc;
    int proc_done;        /* the executor has finished QEMU's attempt */
    uint64_t drain_until; /* the existing grace bounds final socket delivery */
    enum qmp_state qmp_state;
    char *line; /* a QMP line being read */
    size_t line_len, line_cap;
    int line_long;
    char *input; /* console input not yet written */
    size_t input_len;
    size_t console_max;
    uint64_t grace_us;
    uint64_t kill_at;         /* when the executor cancels QEMU; 0 for not due */
    uint64_t quit_at;         /* when the launcher asks QEMU to quit; 0 for not due */
    int boundary, total_sent; /* restricted lazy boot and its one probe */
    int cancelled;            /* the caller stopped the machine before its end */
    int accel_wrong;          /* QMP reported another accelerator in use */
    int qmp_lost;             /* QMP answered what the launcher cannot use */
    int shutdown;             /* QMP reported SHUTDOWN */
    int changed;              /* new console bytes or a new state since the last wait */
    int finished, returned;
    struct qmp_call *calls; /* the caller's commands, newest first */
    long next_id;
    char accel_seen[16];
    struct etude_machine_result r;
};

struct etude_qemu {
    struct etude_exec *ex;
    struct etude_machine *machines;
};

struct etude_qemu *etude_qemu_new(void)
{
    struct etude_qemu *q = calloc(1, sizeof(*q));

    if (q == NULL)
        return NULL;
    q->ex = etude_exec_new();
    if (q->ex == NULL) {
        free(q);
        return NULL;
    }
    return q;
}

/** Close descriptor `*fd` if it is open, and mark it closed. */
static void close_fd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

#ifndef _WIN32
/** Make descriptor `fd` nonblocking and closed on exec. */
static void fd_flags(int fd)
{
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/** Listen on a new Unix socket at `path`, for one connection. */
static int listen_at(const char *path)
{
    struct sockaddr_un a;
    int fd, e;

    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(a.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(a.sun_path, path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fd_flags(fd);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
        e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

#else
/** Make a socket nonblocking and noninheritable. */
static void fd_flags(etude_socket fd)
{
    u_long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
    SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0);
}

/** Listen exclusively on a dynamically assigned loopback port. */
static etude_socket listen_at(char *path)
{
    unsigned short port;
    SOCKET s;
    if (etude_win_net_start() != 0) {
        errno = EIO;
        return -1;
    }
    s = etude_win_listen(&port);
    if (s == INVALID_SOCKET) {
        errno = EIO;
        return -1;
    }
    fd_flags((etude_socket)s);
    snprintf(path, 128, "%u", port);
    return (etude_socket)s;
}
#endif

/** Close a socket and mark it closed. */
static void close_socket_fd(etude_socket *fd)
{
    if (*fd >= 0)
        socket_close(*fd);
    *fd = -1;
}

/** Stop reading connection `*fd` through `in`, close it, and mark it closed. */
static void close_channel(etude_socket *fd, struct socket_in *in)
{
    if (*fd >= 0)
        socket_in_stop(in, *fd);
    close_socket_fd(fd);
}

#ifndef _WIN32
/*
 * A thread that keeps a console socket read while the loop is held. The loop
 * writes each console byte to a file, and a loaded file system holds that write
 * for seconds. The socket fills after a few hundred unread bytes, as QEMU sends
 * one byte at a time, and QEMU's UART then stops the guest's kernel, which
 * waits for it with nothing else running. The bytes wait here instead. Both
 * sides read under the lock, so the bytes stay in order, and the loop can
 * read to the moment of an event as it does without a pump.
 */
struct console_pump {
    pthread_t thread;
    pthread_mutex_t lock;
    int fd;
    int wake[2]; /* a byte in it wakes the loop */
    char *bytes; /* read from the socket, not yet taken by the loop */
    size_t len, cap;
    int closed; /* the socket has given its end */
};

/** Read the socket until it would wait; the lock is held. */
static void pump_fill(struct console_pump *p)
{
    char *grown;
    size_t cap;
    ssize_t n;

    while (!p->closed && p->len < CONSOLE_PUMP_MAX) {
        if (p->cap - p->len < 4096) {
            cap = p->cap != 0 ? p->cap * 2 : 65536;
            grown = realloc(p->bytes, cap);
            if (grown == NULL)
                return;
            p->bytes = grown;
            p->cap = cap;
        }
        n = read(p->fd, p->bytes + p->len, p->cap - p->len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0)
            p->closed = 1;
        else
            p->len += (size_t)n;
    }
}

/** Read the socket whenever it has bytes, until it ends. */
static void *pump_main(void *arg)
{
    struct console_pump *p = arg;
    struct pollfd fd;
    char wake = 0;
    int full = 0, closed = 0, ready;

    fd.fd = p->fd;
    fd.events = POLLIN;
    while (!closed) {
        (void)poll(full ? NULL : &fd, full ? 0 : 1, full ? 100 : -1);
        pthread_mutex_lock(&p->lock);
        pump_fill(p);
        full = p->len >= CONSOLE_PUMP_MAX;
        closed = p->closed;
        ready = p->len > 0 || closed;
        pthread_mutex_unlock(&p->lock);
        if (ready && write(p->wake[1], &wake, 1) < 0 && errno != EAGAIN)
            return NULL;
    }
    return NULL;
}

/** Start the machine's pump, with every signal blocked in its thread, so that
 * signals still reach the loop. Without one the loop reads the socket
 * itself. */
static void pump_start(struct etude_machine *m)
{
    struct console_pump *p = calloc(1, sizeof(*p));
    sigset_t all, old;
    int e, i;

    if (p == NULL)
        return;
    if (pipe(p->wake) != 0) {
        free(p);
        return;
    }
    for (i = 0; i < 2; i++) {
        fcntl(p->wake[i], F_SETFD, FD_CLOEXEC);
        fcntl(p->wake[i], F_SETFL, O_NONBLOCK);
    }
    p->fd = m->console;
    pthread_mutex_init(&p->lock, NULL);
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    e = pthread_create(&p->thread, NULL, pump_main, p);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (e != 0) {
        pthread_mutex_destroy(&p->lock);
        close(p->wake[0]);
        close(p->wake[1]);
        free(p);
        return;
    }
    m->pump = p;
}

/** End the machine's pump; the bytes the loop did not take go with it. */
static void pump_end(struct etude_machine *m)
{
    struct console_pump *p = m->pump;

    if (p == NULL)
        return;
    shutdown(p->fd, SHUT_RD);
    pthread_join(p->thread, NULL);
    pthread_mutex_destroy(&p->lock);
    close(p->wake[0]);
    close(p->wake[1]);
    free(p->bytes);
    free(p);
    m->pump = NULL;
}
#endif

/** Remove the machine's sockets and close what is open. */
static void machine_close(struct etude_machine *m)
{
#ifndef _WIN32
    pump_end(m);
#endif
    close_socket_fd(&m->console_listen);
    close_socket_fd(&m->qmp_listen);
    close_channel(&m->console, &m->console_in);
    close_channel(&m->qmp, &m->qmp_in);
    close_fd(&m->console_file);
    if (m->dir[0] != '\0') {
#ifndef _WIN32
        unlink(m->console_path);
        unlink(m->qmp_path);
#endif
    }
}

/** Free a machine and what it holds. */
static void machine_release(struct etude_machine *m)
{
    struct qmp_call *c;

    while ((c = m->calls) != NULL) {
        m->calls = c->next;
        free(c->reply);
        free(c);
    }
    machine_close(m);
    if (m->audio_created && m->audio_temporary)
        unlink(m->audio_path);
    if (m->dir[0] != '\0')
        rmdir(m->dir);
    free(m->audio_path);
    if (m->proc != NULL)
        etude_proc_free(m->proc);
    capture_free(&m->r.console);
    free(m->attempt_id);
    free(m->line);
    free(m->input);
    free(m);
}

/**
 * Make the machine's private directory and the paths of its sockets: under
 * TMPDIR, or under /tmp when TMPDIR is unset, relative, or too long for the
 * sockets' paths to fit their addresses.
 */
static int machine_dir(struct etude_machine *m)
{
#ifdef _WIN32
    const char *tmp = etude_win_temp();
    if (tmp == NULL)
        return -1;
#else
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] != '/' || strlen(tmp) + sizeof("/etude-qemu.XXXXXX") > sizeof(m->dir))
        tmp = "/tmp";
#endif
    if ((size_t)snprintf(m->dir, sizeof(m->dir), "%s/etude-qemu.XXXXXX", tmp) >= sizeof(m->dir)) {
        m->dir[0] = '\0';
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdtemp(m->dir) == NULL) {
        m->dir[0] = '\0';
        return -1;
    }
#ifndef _WIN32
    snprintf(m->console_path, sizeof(m->console_path), "%s/c", m->dir);
    snprintf(m->qmp_path, sizeof(m->qmp_path), "%s/q", m->dir);
#endif
    return 0;
}

/** Reserve a new WAV file, with its path owned until the machine is freed. */
static int machine_audio(struct etude_machine *m, const struct etude_machine_spec *spec)
{
    char path[sizeof(m->dir) + 16];
    const char *name = spec->audio_path;
    int fd;

    if (spec->profile->audio == NULL ||
        (spec->profile->audio_backend != NULL && strcmp(spec->profile->audio_backend, "wav") != 0))
        return 0;
    if (name == NULL) {
        snprintf(path, sizeof(path), "%s/audio.wav", m->dir);
        name = path;
        m->audio_temporary = 1;
    }
    m->audio_path = strdup(name);
    if (m->audio_path == NULL)
        return -1;
    fd = open(name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    close(fd);
    m->audio_created = 1;
    m->r.audio_path = m->audio_path;
    return 0;
}

/** Make a machine with nothing open, for `spec`. */
static struct etude_machine *machine_new(const struct etude_machine_spec *spec)
{
    struct etude_machine *m = calloc(1, sizeof(*m));

    if (m == NULL)
        return NULL;
    m->console_listen = m->qmp_listen = m->console = m->qmp = m->console_file = -1;
    m->accel = spec->site->accel;
    m->console_max = spec->console_max;
    m->grace_us = spec->grace_us;
    m->next_id = ETUDE_QMP_FIRST_ID;
    m->line_cap = QMP_LINE_FIRST;
    m->attempt_id = malloc(strlen(spec->attempt_id) + 1);
    m->line = malloc(m->line_cap + 1);
    if (m->attempt_id == NULL || m->line == NULL ||
        capture_init(&m->r.console, spec->console_max) != 0) {
        free(m->attempt_id);
        free(m->line);
        free(m);
        return NULL;
    }
    strcpy(m->attempt_id, spec->attempt_id);
    m->r.attempt_id = m->attempt_id;
    m->r.state = ETUDE_MACHINE_STARTING;
    m->r.end = ETUDE_END_LOST;
    m->r.started_us = posix_now_us();
    return m;
}

/** Check a boundary boot's capacity, disabled core files and artifact sizes. */
static int boundary_check(const struct etude_machine_spec *spec, int *boundary)
{
    struct etude_qemu_memory memory;
    struct stat st;
#if defined(__APPLE__)
    struct rlimit core;
#endif
    int e = etude_qemu_memory(spec->profile, &memory);

    if (e != 0) {
        errno = e;
        return -1;
    }
    *boundary = memory.resident_bytes < memory.configured_bytes;
    if (!*boundary)
        return 0;
    if (spec->memory_reserved_bytes < memory.resident_bytes + memory.overhead_bytes) {
        errno = ENOMEM;
        return -1;
    }
#if defined(__APPLE__)
    if (getrlimit(RLIMIT_CORE, &core) != 0)
        return -1;
    if (core.rlim_cur != 0) {
        errno = ENOTSUP;
        return -1;
    }
#endif
    if (spec->profile->kernel == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (stat(spec->profile->kernel, &st) != 0)
        return -1;
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > (64u << 20)) {
        errno = EFBIG;
        return -1;
    }
    if (stat(spec->profile->initrd, &st) != 0)
        return -1;
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > (256u << 20)) {
        errno = EFBIG;
        return -1;
    }
    return 0;
}

#ifdef _WIN32
/* Temporary native names remain owned until the command line copies them. */
struct native_paths {
    struct etude_qemu_disk disks[ETUDE_QEMU_DISKS + 1];
    struct etude_qemu_capture captures[ETUDE_QEMU_ARGS_MAX];
    char *owned[ETUDE_QEMU_DISKS + ETUDE_QEMU_ARGS_MAX + 5];
    size_t nowned;
};

/** Convert one file operand with the same API used to create it. */
static int native_file(struct native_paths *n, const char *path, const char **out)
{
    wchar_t *wide;
    char *utf;

    if (path == NULL) {
        *out = NULL;
        return 0;
    }
    if (n->nowned == sizeof n->owned / sizeof n->owned[0])
        return E2BIG;
    wide = etude_win_long_path(path);
    if (wide == NULL) {
        etude_win_file_error();
        return errno;
    }
    utf = etude_win_utf8(wide);
    if (utf == NULL) {
        int error;
        etude_win_file_error();
        error = errno;
        free(wide);
        return error;
    }
    free(wide);
    *out = n->owned[n->nowned++] = utf;
    return 0;
}

/** Copy validated file operands; keep profile identities and devices intact. */
static int native_profile(struct native_paths *n, struct etude_qemu_profile *p)
{
    size_t i;
    int e;

    if (p->firmware == ETUDE_FIRMWARE_DIRECT && ((e = native_file(n, p->kernel, &p->kernel)) != 0 ||
                                                 (e = native_file(n, p->initrd, &p->initrd)) != 0))
        return e;
    for (i = 0; p->disks != NULL && p->disks[i].path != NULL; i++) {
        if (i >= ETUDE_QEMU_DISKS)
            return E2BIG;
        n->disks[i] = p->disks[i];
        if ((e = native_file(n, p->disks[i].path, &n->disks[i].path)) != 0)
            return e;
    }
    p->disks = p->disks != NULL ? n->disks : NULL;
    for (i = 0; p->captures != NULL && p->captures[i].path != NULL; i++) {
        if (i + 1 >= ETUDE_QEMU_ARGS_MAX)
            return E2BIG;
        n->captures[i] = p->captures[i];
        if ((e = native_file(n, p->captures[i].path, &n->captures[i].path)) != 0)
            return e;
    }
    p->captures = p->captures != NULL ? n->captures : NULL;
    return 0;
}

/** Preserve the portable serializer and convert files at native launch only. */
static int launch_argv(const struct etude_qemu_profile *p, const struct etude_qemu_site *site,
                       const char *console, const char *qmp, const char *audio,
                       const struct etude_qemu_line *line)
{
    struct native_paths n = {0};
    struct etude_qemu_profile native = *p;
    struct etude_qemu_site selected = *site;
    int e = etude_qemu_argv(p, site, console, qmp, audio, line);

    if (e == 0)
        e = native_profile(&n, &native);
    if (e == 0 && p->firmware == ETUDE_FIRMWARE_OVMF)
        e = native_file(&n, site->ovmf_x86_64, &selected.ovmf_x86_64);
    if (e == 0 && p->audio != NULL)
        e = native_file(&n, audio, &audio);
    if (e == 0)
        e = etude_qemu_argv(&native, &selected, console, qmp, audio, line);
    for (size_t i = 0; i < n.nowned; i++)
        free(n.owned[i]);
    return e;
}
#endif

/**
 * Make QEMU's environment for `spec`, as etude_qemu_env makes it, in one
 * allocation that the caller frees, at `*out`.
 *
 * @return 0, or an error number of etude_qemu_env's or ENOMEM.
 */
static int launch_env(const struct etude_machine_spec *spec, char ***out)
{
    const char *input = spec->profile->audio_input;
    struct etude_qemu_line line;
    size_t i, n = 0, size = 128;
    char **v;
    int e = 0;
#ifdef _WIN32
    struct native_paths native = {0};

    if (input != NULL && (e = native_file(&native, input, &input)) != 0)
        return e;
#endif
    for (i = 0; spec->envp != NULL && spec->envp[i] != NULL; i++, n++)
        size += strlen(spec->envp[i]) + 1;
    if (input != NULL)
        size += strlen(input);
    v = malloc((n + 4) * sizeof(*v) + size);
    if (v == NULL)
        e = ENOMEM;
    if (e == 0) {
        line.argv = v;
        line.max = n + 4;
        line.buf = (char *)(v + n + 4);
        line.size = size;
        e = etude_qemu_env(spec->profile, spec->envp, input, &line);
    }
#ifdef _WIN32
    for (i = 0; i < native.nowned; i++)
        free(native.owned[i]);
#endif
    if (e != 0) {
        free(v);
        return e;
    }
    *out = v;
    return 0;
}

#ifndef _WIN32
#define launch_argv etude_qemu_argv
#endif

int etude_qemu_start(struct etude_qemu *q, const struct etude_machine_spec *spec,
                     struct etude_machine **mp)
{
    struct etude_machine *m = machine_new(spec);
    struct etude_exec_spec es;
    struct etude_qemu_line line;
    char *argv[ETUDE_QEMU_ARGS_MAX], *buf = NULL, **envp = NULL;
    int e;

    if (m == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (boundary_check(spec, &m->boundary) != 0 || machine_dir(m) != 0 ||
        machine_audio(m, spec) != 0)
        goto fail;
    buf = malloc(QEMU_LINE_BYTES);
    if (buf == NULL) {
        errno = ENOMEM;
        goto fail;
    }
    line.argv = argv;
    line.max = ETUDE_QEMU_ARGS_MAX;
    line.buf = buf;
    line.size = QEMU_LINE_BYTES;
    if ((m->console_listen = listen_at(m->console_path)) < 0 ||
        (m->qmp_listen = listen_at(m->qmp_path)) < 0)
        goto fail;
    e = launch_argv(spec->profile, spec->site, m->console_path, m->qmp_path, m->audio_path, &line);
    if (e == 0)
        e = launch_env(spec, &envp);
    if (e != 0) {
        errno = e;
        goto fail;
    }
    if (spec->console_path != NULL &&
        (m->console_file =
             open(spec->console_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0)
        goto fail;
    memset(&es, 0, sizeof(es));
    es.attempt_id = spec->attempt_id;
    es.argv = argv;
    es.envp = envp;
    es.output_max = QEMU_OUTPUT_MAX;
    es.grace_us = spec->grace_us;
    es.reg = spec->reg;
    es.reg_ctx = spec->reg_ctx;
    if (etude_exec_start(q->ex, &es, &m->proc) != 0) {
        m->proc = NULL;
        goto fail;
    }
    free(buf);
    free(envp);
    m->r.qemu = etude_proc_result(m->proc);
    m->q = q;
    m->next = q->machines;
    q->machines = m;
    *mp = m;
    return 0;
fail:
    e = errno;
    free(buf);
    free(envp);
    if (m->console_file >= 0)
        unlink(spec->console_path);
    if (m->audio_created)
        unlink(m->audio_path);
    machine_release(m);
    errno = e;
    return -1;
}

/** Keep `n` console bytes in memory and in the console's file. */
static void console_keep(struct etude_machine *m, const char *buf, size_t n)
{
    ssize_t w;
    size_t done = 0;

    capture_add(&m->r.console, m->console_max, buf, n);
    m->changed = 1;
    while (m->console_file >= 0 && done < n) {
        w = write(m->console_file, buf + done, n - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            m->r.console_file_error = w < 0 ? errno : EIO;
            close_fd(&m->console_file);
            return;
        }
        done += (size_t)w;
    }
}

#ifndef _WIN32
/** Keep all that the pump holds and what the socket has now, and close the
 * console when it has ended. */
static void pump_take(struct etude_machine *m)
{
    struct console_pump *p = m->pump;
    char drain[64], *bytes;
    size_t n;
    int closed;

    while (read(p->wake[0], drain, sizeof(drain)) > 0) {
    }
    pthread_mutex_lock(&p->lock);
    pump_fill(p);
    bytes = p->bytes;
    n = p->len;
    closed = p->closed;
    p->bytes = NULL;
    p->len = p->cap = 0;
    pthread_mutex_unlock(&p->lock);
    if (n > 0)
        console_keep(m, bytes, n);
    free(bytes);
    if (closed) {
        pump_end(m);
        close_channel(&m->console, &m->console_in);
        m->r.console.closed = 1;
        m->changed = 1;
    }
}
#endif

/**
 * Read what the console has written, until it would wait or ends, or after
 * `limit` bytes, so that a console that never pauses leaves the other
 * machines served; 0 sets no limit. A pump takes no limit, as CONSOLE_PUMP_MAX
 * bounds what it holds.
 */
static void console_read(struct etude_machine *m, size_t limit)
{
    char buf[4096];
    size_t total = 0;
    ssize_t n;

#ifndef _WIN32
    if (m->pump != NULL) {
        pump_take(m);
        return;
    }
#endif
    while (m->console >= 0 && (limit == 0 || total < limit)) {
        n = socket_read(&m->console_in, m->console, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0) {
            close_channel(&m->console, &m->console_in);
            m->r.console.closed = 1;
            m->changed = 1;
            return;
        }
        console_keep(m, buf, (size_t)n);
        total += (size_t)n;
    }
}

/**
 * Write QMP command `json`; a failed or short write makes QMP unusable. A
 * receive is posted first, so that the reply is kept even when QEMU ends at
 * once after it, as it does after `quit`.
 */
static int qmp_write(struct etude_machine *m, const char *json)
{
    size_t len = strlen(json);
    ssize_t n;

    if (m->qmp < 0)
        return -1;
    socket_in_pump(&m->qmp_in, m->qmp);
    do {
        n = socket_write(m->qmp, json, len);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)len) {
        m->qmp_state = QMP_GONE;
        m->qmp_lost = 1;
        return -1;
    }
    return 0;
}

/** Send the launcher's QMP command `json`, and go to state `next`. */
static void qmp_send(struct etude_machine *m, const char *json, enum qmp_state next)
{
    if (qmp_write(m, json) == 0)
        m->qmp_state = next;
}

/** Ask QEMU to quit, and arm the kill that follows if it does not. */
static void machine_quit(struct etude_machine *m)
{
    if (m->qmp_state >= QMP_ACCEL && m->qmp_state <= QMP_READY)
        qmp_send(m, "{\"execute\":\"quit\",\"id\":5}\n", QMP_QUIT);
    if (m->qmp_state == QMP_QUIT) {
        m->kill_at = posix_now_us() + m->grace_us;
    } else {
        m->kill_at = 0;
        etude_exec_cancel(m->proc);
    }
}

/**
 * Take the accelerator QMP reported in use, `name`, or for a QEMU that
 * reports only KVM, whether KVM is in use; start the machine when it is the
 * one asked for, and quit QEMU otherwise.
 */
static void accel_seen(struct etude_machine *m, const char *name, const char *method)
{
    snprintf(m->accel_seen, sizeof(m->accel_seen), "%s", name);
    m->r.accel = m->accel_seen;
    m->r.method = method;
    m->changed = 1;
    if (strcmp(name, etude_accel_name(m->accel)) == 0) {
        qmp_send(m, "{\"execute\":\"cont\",\"id\":4}\n", QMP_CONT);
    } else {
        m->accel_wrong = 1;
        machine_quit(m);
    }
}

/** Give up on a QMP that answered what the launcher cannot use. */
static void qmp_unusable(struct etude_machine *m)
{
    m->qmp_lost = 1;
    machine_quit(m);
}

/**
 * Take query-accelerators' reply, `ret`, or for a QEMU without the command,
 * its error, `cls`, and ask query-kvm.
 */
static void accel_reply(struct etude_machine *m, const struct eh_json *ret,
                        const struct eh_json *cls)
{
    const struct eh_json *v = eh_json_get(ret, "enabled");

    if (v != NULL && v->type == 's')
        accel_seen(m, v->string, "query-accelerators");
    else if (ret == NULL && cls != NULL && cls->type == 's' &&
             strcmp(cls->string, "CommandNotFound") == 0)
        qmp_send(m, "{\"execute\":\"query-kvm\",\"id\":3}\n", QMP_KVM);
    else
        qmp_unusable(m);
}

/** Take query-kvm's reply, `ret`, which says only whether KVM is in use. */
static void kvm_reply(struct etude_machine *m, const struct eh_json *ret)
{
    const struct eh_json *v = eh_json_get(ret, "enabled");

    if (v == NULL || (v->type != 't' && v->type != 'f'))
        qmp_unusable(m);
    else if (v->type == 't')
        accel_seen(m, "kvm", "query-kvm");
    else if (m->accel == ETUDE_ACCEL_KVM)
        accel_seen(m, "not kvm", "query-kvm");
    else
        accel_seen(m, etude_accel_name(m->accel), "query-kvm");
}

/** Act on the reply to the launcher's command `id`. */
static void qmp_reply(struct etude_machine *m, long id, const struct eh_json *ret,
                      const struct eh_json *error)
{
    if (m->qmp_state == QMP_CAPS && id == ID_CAPS && ret != NULL) {
        qmp_send(m, "{\"execute\":\"query-accelerators\",\"id\":2}\n", QMP_ACCEL);
    } else if (m->qmp_state == QMP_ACCEL && id == ID_ACCEL) {
        accel_reply(m, ret, eh_json_get(error, "class"));
    } else if (m->qmp_state == QMP_KVM && id == ID_KVM && ret != NULL) {
        kvm_reply(m, ret);
    } else if (m->qmp_state == QMP_CONT && id == ID_CONT && ret != NULL) {
        m->qmp_state = QMP_READY;
        m->r.state = ETUDE_MACHINE_RUNNING;
        m->r.running_us = posix_now_us();
        m->changed = 1;
    } else if (id != ID_QUIT && m->qmp_state != QMP_QUIT) {
        qmp_unusable(m);
    }
}

/**
 * Act on QMP's event `ev`: only SHUTDOWN tells the launcher anything. The
 * console is read first, so that the offset of the end counts every byte
 * QEMU wrote before it sent the event.
 *
 * On Windows a process that exits resets its loopback connections, and the
 * reset discards what the launcher has not yet read of them. QEMU runs there
 * with `-no-shutdown`, which stops the guest at its shutdown, reset or panic
 * and keeps QEMU. The console's last bytes may still be on their way when
 * the event arrives on the other connection, so the launcher goes on reading
 * the console for the grace period and then asks QEMU to quit.
 */
static void qmp_event(struct etude_machine *m, const struct eh_json *ev)
{
    const struct eh_json *name = eh_json_get(ev, "event");
    const struct eh_json *data = eh_json_get(ev, "data");
    const struct eh_json *reason = eh_json_get(data, "reason");
    const struct eh_json *guest = eh_json_get(data, "guest");

    if (name == NULL || name->type != 's' || strcmp(name->string, "SHUTDOWN") != 0 || m->shutdown)
        return;
    m->shutdown = 1;
    m->changed = 1;
    console_read(m, 0);
    m->r.end_console = m->r.console.total;
    m->r.ended_us = posix_now_us();
    m->r.by_guest = guest != NULL && guest->type == 't';
    if (reason != NULL && reason->type == 's')
        snprintf(m->r.reason, sizeof(m->r.reason), "%s", reason->string);
#ifdef _WIN32
    if (m->r.by_guest && m->qmp_state >= QMP_ACCEL && m->qmp_state <= QMP_READY)
        m->quit_at = posix_now_us() + m->grace_us;
#endif
}

/** Keep the reply `text` to the caller's command `id`. */
static void call_answered(struct etude_machine *m, long id, const char *text)
{
    struct qmp_call *c;

    for (c = m->calls; c != NULL && c->id != id; c = c->next) {
    }
    if (c == NULL || c->reply != NULL) {
        m->r.qmp_bad++;
        return;
    }
    c->reply = malloc(strlen(text) + 1);
    if (c->reply != NULL)
        strcpy(c->reply, text);
    m->changed = 1;
}

/** Act on one whole line of QMP's. */
static void qmp_line(struct etude_machine *m, char *text)
{
    struct eh_json *j;
    const struct eh_json *id;
    size_t len = strlen(text);
    long n;

    if (len > 0 && text[len - 1] == '\r')
        text[len - 1] = '\0';
    if (text[0] == '\0')
        return;
    j = eh_json_parse(text);
    if (j == NULL || j->type != 'o') {
        m->r.qmp_bad++;
        eh_json_free(j);
        return;
    }
    if (eh_json_get(j, "QMP") != NULL) {
        if (m->qmp_state == QMP_GREETING)
            qmp_send(m, "{\"execute\":\"qmp_capabilities\",\"id\":1}\n", QMP_CAPS);
    } else if (eh_json_get(j, "event") != NULL) {
        qmp_event(m, j);
    } else if (eh_json_get(j, "return") != NULL || eh_json_get(j, "error") != NULL) {
        id = eh_json_get(j, "id");
        n = id != NULL && id->type == 'n' ? (long)id->number : -1;
        if (n >= ETUDE_QMP_FIRST_ID)
            call_answered(m, n, text);
        else
            qmp_reply(m, n, eh_json_get(j, "return"), eh_json_get(j, "error"));
    } else {
        m->r.qmp_bad++;
    }
    eh_json_free(j);
}

/** Add byte `c` to the QMP line, growing it up to QMP_LINE_MAX. */
static void line_add(struct etude_machine *m, char c)
{
    char *grown;

    if (m->line_len == m->line_cap && m->line_cap < QMP_LINE_MAX) {
        grown = realloc(m->line, 2 * m->line_cap + 1);
        if (grown != NULL) {
            m->line = grown;
            m->line_cap *= 2;
        }
    }
    if (m->line_len < m->line_cap)
        m->line[m->line_len++] = c;
    else
        m->line_long = 1;
}

/** Read what QMP has sent, a line at a time, until it would wait or ends. */
static void qmp_read(struct etude_machine *m)
{
    char buf[4096];
    ssize_t n, i;

    while (m->qmp >= 0) {
        n = socket_read(&m->qmp_in, m->qmp, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0) {
            close_channel(&m->qmp, &m->qmp_in);
            if (m->qmp_state != QMP_QUIT)
                m->qmp_state = QMP_GONE;
            return;
        }
        for (i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                line_add(m, buf[i]);
                continue;
            }
            m->line[m->line_len] = '\0';
            if (m->line_long)
                m->r.qmp_bad++;
            else
                qmp_line(m, m->line);
            m->line_len = 0;
            m->line_long = 0;
        }
    }
}

/** Write what the console's input still has, as far as the socket takes. */
static void console_write(struct etude_machine *m)
{
    ssize_t n;

    while (m->console >= 0 && m->input_len > 0) {
        n = socket_write(m->console, m->input, m->input_len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return;
        memmove(m->input, m->input + n, m->input_len - (size_t)n);
        m->input_len -= (size_t)n;
    }
}

/**
 * Take QEMU's connection to listening socket `*listener`, start reading it
 * through `in`, and remove the listener.
 */
static void take_connection(etude_socket *listener, etude_socket *conn, struct socket_in *in,
                            const char *path)
{
    etude_socket fd;

    if (*listener < 0)
        return;
    fd = accept(*listener, NULL, NULL);
    if (fd < 0)
        return;
    fd_flags(fd);
    *conn = fd;
    socket_in_start(in, fd);
    close_socket_fd(listener);
#ifndef _WIN32
    unlink(path);
#else
    (void)path;
#endif
}

/** Give the end of a machine whose QEMU has ended. */
static enum etude_end machine_end(const struct etude_machine *m)
{
    const struct etude_exec_result *qemu = m->r.qemu;

    if (m->accel_wrong)
        return ETUDE_END_ACCEL_FAILED;
    if (m->cancelled)
        return ETUDE_END_STOPPED;
    if (m->shutdown && m->r.by_guest) {
        if (strcmp(m->r.reason, "guest-shutdown") == 0)
            return ETUDE_END_SHUTDOWN;
        if (strcmp(m->r.reason, "guest-reset") == 0)
            return ETUDE_END_RESET;
        if (strcmp(m->r.reason, "guest-panic") == 0)
            return ETUDE_END_PANIC;
    }
    if (m->shutdown)
        return ETUDE_END_QEMU_EXITED;
    if (m->qmp_lost || qemu->end == ETUDE_END_LOST)
        return ETUDE_END_LOST;
    if (m->r.state == ETUDE_MACHINE_STARTING && etude_qemu_accel_refused(qemu->err.data, m->accel))
        return ETUDE_END_ACCEL_FAILED;
    return ETUDE_END_QEMU_EXITED;
}

/**
 * Serve one machine: take QEMU's connections, read the console and QMP,
 * write the console's input, cancel QEMU when its quit is overdue, and
 * finish the machine once QEMU has ended.
 */
static void machine_serve(struct etude_machine *m)
{
    if (m->finished)
        return;
    take_connection(&m->console_listen, &m->console, &m->console_in, m->console_path);
#ifndef _WIN32
    if (m->console >= 0 && m->pump == NULL)
        pump_start(m);
#endif
    take_connection(&m->qmp_listen, &m->qmp, &m->qmp_in, m->qmp_path);
    console_read(m, CONSOLE_PASS_MAX);
    console_write(m);
    qmp_read(m);
    if (m->quit_at != 0 && posix_now_us() >= m->quit_at && !m->proc_done) {
        m->quit_at = 0;
        machine_quit(m);
    }
    if (m->kill_at != 0 && posix_now_us() >= m->kill_at && !m->proc_done) {
        m->kill_at = 0;
        etude_exec_cancel(m->proc);
    }
    if (!m->proc_done)
        return;
    console_read(m, 0);
    qmp_read(m);
    if ((m->console >= 0 || m->qmp >= 0) && posix_now_us() < m->drain_until)
        return;
    m->r.end = machine_end(m);
    m->r.end_console = m->r.console.total;
    if (!m->shutdown || m->r.end == ETUDE_END_STOPPED || m->r.end == ETUDE_END_ACCEL_FAILED)
        m->r.ended_us = posix_now_us();
    m->r.state = ETUDE_MACHINE_ENDED;
    machine_close(m);
    m->finished = 1;
    m->changed = 1;
}

/** Give a finished machine not yet returned, marking it returned. */
static struct etude_machine *take_finished(struct etude_qemu *q)
{
    struct etude_machine *m;

    for (m = q->machines; m != NULL; m = m->next) {
        if (m->finished && !m->returned) {
            m->returned = 1;
            return m;
        }
    }
    return NULL;
}

/**
 * Fill `w` with the descriptors of every unfinished machine, and give their
 * count and in `*soonest` the earliest kill or quit that is due, or 0.
 */
static size_t watch_set(struct etude_qemu *q, struct etude_watch *w, uint64_t *soonest)
{
    struct etude_machine *m;
    size_t n = 0;

    *soonest = 0;
    for (m = q->machines; m != NULL; m = m->next) {
        if (m->finished)
            continue;
        if (!m->proc_done && m->kill_at != 0 && (*soonest == 0 || m->kill_at < *soonest))
            *soonest = m->kill_at;
        if (!m->proc_done && m->quit_at != 0 && (*soonest == 0 || m->quit_at < *soonest))
            *soonest = m->quit_at;
        if (m->drain_until != 0 && (*soonest == 0 || m->drain_until < *soonest))
            *soonest = m->drain_until;
        if (m->console_listen >= 0) {
            w[n].fd = m->console_listen;
            w[n++].events = ETUDE_WATCH_IN;
        }
        if (m->qmp_listen >= 0) {
            w[n].fd = m->qmp_listen;
            w[n++].events = ETUDE_WATCH_IN;
        }
#ifndef _WIN32
        if (m->pump != NULL) {
            w[n].fd = m->pump->wake[0];
            w[n++].events = ETUDE_WATCH_IN;
            if (m->input_len > 0) {
                w[n].fd = m->console;
                w[n++].events = ETUDE_WATCH_OUT;
            }
        } else
#endif
            if (m->console >= 0) {
            w[n].fd = m->console;
            w[n++].events = ETUDE_WATCH_IN | (m->input_len > 0 ? ETUDE_WATCH_OUT : 0);
        }
        if (m->qmp >= 0) {
            w[n].fd = m->qmp;
            w[n++].events = ETUDE_WATCH_IN;
        }
    }
    return n;
}

/** Count the machines that have not finished. */
static size_t unfinished(const struct etude_qemu *q)
{
    const struct etude_machine *m;
    size_t n = 0;

    for (m = q->machines; m != NULL; m = m->next)
        n += !m->finished;
    return n;
}

/** Say whether any machine changed since the last wait, and clear the marks. */
static int take_changes(struct etude_qemu *q)
{
    struct etude_machine *m;
    int changed = 0;

    for (m = q->machines; m != NULL; m = m->next) {
        changed |= m->changed;
        m->changed = 0;
    }
    return changed;
}

/** Give the milliseconds to wait from `now` until `until`, or -1 at 0. */
static int wait_for(uint64_t until, uint64_t now)
{
    if (until == 0)
        return -1;
    if (until <= now)
        return 0;
    if (until - now > 60000000u)
        return 60000;
    return (int)((until - now + 999) / 1000);
}

/** Note the end of `p` when it is a machine's QEMU, and give it back when it
 * is not, or NULL for none. */
static struct etude_proc *machine_proc(struct etude_qemu *q, struct etude_proc *p)
{
    struct etude_machine *m;

    for (m = q->machines; p != NULL && m != NULL; m = m->next) {
        if (m->proc == p) {
            m->proc_done = 1;
            m->drain_until = m->r.qemu->finished_us + m->grace_us;
            return NULL;
        }
    }
    return p;
}

#ifdef _WIN32
/* The interval at which the launcher looks at the receives posted on QEMU's
 * connections, whose completions the sockets' readiness does not show. */
#define POSTED_POLL_US 10000

/**
 * Give the time by which the launcher must serve the machines for their
 * posted receives, from `now`: `now` itself when one has taken bytes or an end
 * not yet read, POSTED_POLL_US later while a connection is open, or 0.
 */
static uint64_t posted_due(struct etude_qemu *q, uint64_t now)
{
    struct etude_machine *m;
    uint64_t due = 0;

    for (m = q->machines; m != NULL; m = m->next) {
        if (m->finished)
            continue;
        if ((m->console >= 0 && socket_in_ready(&m->console_in, m->console)) ||
            (m->qmp >= 0 && socket_in_ready(&m->qmp_in, m->qmp)))
            return now;
        if (m->console >= 0 || m->qmp >= 0)
            due = now + POSTED_POLL_US;
    }
    return due;
}
#endif

/* What ended one wait of etude_qemu_watch. */
struct round {
    struct etude_proc *p; /* an attempt that finished, not a machine's QEMU */
    int ready;            /* a descriptor of the caller's was ready */
    int output;           /* nothing but an attempt's output ended the wait */
};

/**
 * Wait once on the machines' descriptors and the caller's `cw`, at most until
 * `until`, or without a bound at 0, serve the machines, and give what ended
 * the wait in `r`.
 *
 * @return 0, or -1 when memory ran out.
 */
static int watch_once(struct etude_qemu *q, uint64_t until, struct etude_watch *cw, size_t cn,
                      struct round *r)
{
    struct etude_machine *m;
    struct etude_watch *w;
    uint64_t soonest;
#ifdef _WIN32
    uint64_t due;
#endif
    size_t i, n;
    int own = 0;

    w = malloc((5 * unfinished(q) + cn + 1) * sizeof(*w));
    if (w == NULL)
        return -1;
    n = watch_set(q, w, &soonest);
    for (i = 0; i < cn; i++)
        w[n + i] = cw[i];
    if (until != 0 && (soonest == 0 || until < soonest))
        soonest = until;
#ifdef _WIN32
    due = posted_due(q, posix_now_us());
    if (due != 0 && (soonest == 0 || due < soonest))
        soonest = due;
#endif
    r->p = etude_exec_watch(q->ex, wait_for(soonest, posix_now_us()), w, n + cn);
    for (i = 0; i < cn; i++) {
        cw[i].revents = w[n + i].revents;
        r->ready |= cw[i].revents != 0;
    }
    for (i = 0; i < n; i++)
        own |= w[i].revents != 0;
    r->output = r->p == NULL && !own && !r->ready && (soonest == 0 || posix_now_us() < soonest);
    free(w);
    r->p = machine_proc(q, r->p);
    for (m = q->machines; m != NULL; m = m->next)
        machine_serve(m);
    return 0;
}

struct etude_machine *etude_qemu_watch(struct etude_qemu *q, int timeout_ms, struct etude_watch *cw,
                                       size_t cn, struct etude_proc **host)
{
    uint64_t until = timeout_ms < 0 ? 0 : posix_now_us() + (uint64_t)timeout_ms * 1000u;
    struct round r = {NULL, 0, 0};
    struct etude_machine *done;
    int polled = 0;
    size_t i;

    if (host != NULL)
        *host = NULL;
    for (i = 0; i < cn; i++)
        cw[i].revents = 0;
    for (;;) {
        done = take_finished(q);
        if (done != NULL || take_changes(q) || r.ready || r.output ||
            (host != NULL && *host != NULL) || (unfinished(q) == 0 && cn == 0 && host == NULL))
            return done;
        if (polled && until != 0 && posix_now_us() >= until)
            return NULL;
        if (watch_once(q, until, cw, cn, &r) != 0)
            return NULL;
        polled = 1;
        if (r.p != NULL && host != NULL)
            *host = r.p;
    }
}

struct etude_machine *etude_qemu_wait(struct etude_qemu *q, int timeout_ms)
{
    return etude_qemu_watch(q, timeout_ms, NULL, 0, NULL);
}

struct etude_exec *etude_qemu_exec(struct etude_qemu *q)
{
    return q->ex;
}

int etude_machine_type(struct etude_machine *m, const char *text, size_t n)
{
    char *grown;

    if (m->finished || (m->console < 0 && m->console_listen < 0)) {
        errno = EPIPE;
        return -1;
    }
    if (m->boundary) {
        int total = n == 21 && memcmp(text, "exec /memprobe total\r", 21) == 0;
        int poweroff = n == 9 && memcmp(text, "poweroff\r", 9) == 0;

        if ((!total && !poweroff) || (total && m->total_sent)) {
            errno = EPERM;
            return -1;
        }
        m->total_sent |= total;
    }
    grown = realloc(m->input, m->input_len + n + 1);
    if (grown == NULL)
        return -1;
    m->input = grown;
    memcpy(m->input + m->input_len, text, n);
    m->input_len += n;
    console_write(m);
    return 0;
}

void etude_machine_cancel(struct etude_machine *m)
{
    if (m->finished || m->cancelled || m->proc_done)
        return;
    if (m->shutdown) {
        etude_exec_resume(m->proc);
        if (m->kill_at == 0)
            m->kill_at = posix_now_us() + m->grace_us;
        return;
    }
    m->cancelled = 1;
    etude_exec_resume(m->proc);
    machine_quit(m);
}

int etude_machine_suspend(struct etude_machine *m)
{
    if (m->finished || m->cancelled || m->proc_done) {
        errno = ESRCH;
        return -1;
    }
    if (m->r.state != ETUDE_MACHINE_RUNNING) {
        errno = EAGAIN;
        return -1;
    }
    return etude_exec_suspend(m->proc);
}

int etude_machine_resume(struct etude_machine *m)
{
    if (m->finished || m->proc_done)
        return 0;
    return etude_exec_resume(m->proc);
}

/** Say whether `name` is a QMP command's name: letters, digits, - and _. */
static int command_name(const char *name)
{
    if (*name == '\0')
        return 0;
    for (; *name != '\0'; name++)
        if (!((*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z') ||
              (*name >= '0' && *name <= '9') || *name == '-' || *name == '_'))
            return 0;
    return 1;
}

/**
 * Say whether a boundary boot takes QMP command `execute` with `arguments`:
 * `qmp_capabilities`, `quit`, a `query-` command, or the human monitor's
 * `info`, each with no arguments or one JSON object. These read the machine's
 * state or end it; another command, such as `object-add` or `migrate`, could
 * make QEMU commit memory that the boundary profile does not reserve.
 */
static int boundary_command(const char *execute, const char *arguments)
{
    struct eh_json *args = NULL;
    const struct eh_json *line;
    int ok;

    if (arguments != NULL) {
        args = eh_json_parse(arguments);
        if (args == NULL || args->type != 'o') {
            eh_json_free(args);
            return 0;
        }
    }
    line = eh_json_get(args, "command-line");
    ok = strcmp(execute, "qmp_capabilities") == 0 || strcmp(execute, "quit") == 0 ||
         strncmp(execute, "query-", 6) == 0 ||
         (strcmp(execute, "human-monitor-command") == 0 && line != NULL && line->type == 's' &&
          strncmp(line->string, "info ", 5) == 0);
    eh_json_free(args);
    return ok;
}

long etude_machine_qmp(struct etude_machine *m, const char *execute, const char *arguments)
{
    struct qmp_call *c;
    size_t size;
    char *json;
    int rc;

    if (!command_name(execute) || (arguments != NULL && strchr(arguments, '\n') != NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (m->boundary && !boundary_command(execute, arguments)) {
        errno = EPERM;
        return -1;
    }
    if (m->finished || m->qmp < 0 || m->qmp_state == QMP_GONE) {
        errno = EPIPE;
        return -1;
    }
    if (m->qmp_state != QMP_READY) {
        errno = EAGAIN;
        return -1;
    }
    size = strlen(execute) + (arguments != NULL ? strlen(arguments) : 0) + 64;
    json = malloc(size);
    c = calloc(1, sizeof(*c));
    if (json == NULL || c == NULL) {
        free(json);
        free(c);
        errno = ENOMEM;
        return -1;
    }
    c->id = m->next_id++;
    if (arguments != NULL)
        snprintf(json, size, "{\"execute\":\"%s\",\"arguments\":%s,\"id\":%ld}\n", execute,
                 arguments, c->id);
    else
        snprintf(json, size, "{\"execute\":\"%s\",\"id\":%ld}\n", execute, c->id);
    c->next = m->calls;
    m->calls = c;
    rc = qmp_write(m, json);
    free(json);
    if (rc != 0) {
        errno = EPIPE;
        return -1;
    }
    return c->id;
}

int etude_machine_reply(const struct etude_machine *m, long id, const char **reply)
{
    const struct qmp_call *c;

    for (c = m->calls; c != NULL && c->id != id; c = c->next) {
    }
    if (c != NULL && c->reply != NULL) {
        *reply = c->reply;
        return 1;
    }
    if (c == NULL || m->finished || m->qmp < 0 || m->qmp_state == QMP_GONE)
        return -1;
    return 0;
}

const struct etude_machine_result *etude_machine_result(const struct etude_machine *m)
{
    return &m->r;
}

void etude_machine_free(struct etude_machine *m)
{
    struct etude_machine **mp;

    for (mp = &m->q->machines; *mp != NULL; mp = &(*mp)->next) {
        if (*mp == m) {
            *mp = m->next;
            break;
        }
    }
    machine_release(m);
}

void etude_qemu_free(struct etude_qemu *q)
{
    struct etude_machine *m;

    for (m = q->machines; m != NULL; m = m->next)
        etude_machine_cancel(m);
    while (unfinished(q) > 0)
        etude_qemu_wait(q, -1);
    while (q->machines != NULL)
        etude_machine_free(q->machines);
    etude_exec_free(q->ex);
    free(q);
}
