/**
 * Check the DHCP client, utils/dhcpc.c: its cleanup, masks, resolver updates,
 * claims, descriptor release and the waits of a lease.
 *
 * host-deps: utils/dhcpmsg.c utils/ipcheck.c
 * host-cflags: -I tests/host/compat/netutil
 *
 * Files use a private host directory. Ioctls and standard descriptors use
 * models; no host interface or device is opened. Resolver writes overlap at
 * stream creation, before either writer can publish the other's bytes. The
 * wait rows run on a modeled clock that each poll and sleep advances by its
 * whole timeout, so a wait of minutes costs nothing.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <net/route.h>

/*
 * The models' state. remove_error: nonzero to fail the removal of the
 * address with EIO. claim_error, resolver_error, record_error: the errno with
 * which opening the claim, making the resolver's temporary file and creating
 * the claim fail, or 0. claim_read_error: nonzero to report a read error on
 * the claim. overlap: the first resolver writer starts a second, and nested
 * is set while the second runs. stdio_model: the descriptor rows are
 * running. console_error, null_error: the errno with which opening
 * /dev/console and /dev/null fail, or 0. dup_error: the standard descriptor
 * whose dup2 fails, or -1. descriptors: each standard descriptor, 1 as
 * inherited, 2 replaced and 0 closed. extra_open: descriptor 10, the device
 * opened, is open. clock_model: the wait rows' clock, clock_now, is in use.
 * send_times: the clock at each of the first sixteen requests. address, mask,
 * gateway: the interface as the ioctls leave it.
 */
static char directory[] = "/tmp/dhcpc-test.XXXXXX";
static int failures, remove_error, claim_error, claim_read_error, resolver_error, record_error;
static int overlap, nested, stdio_model, console_error, null_error, dup_error;
static int descriptors[3], extra_open, mask_calls, sleep_calls;
static int clock_model, polls, sends;
static time_t clock_now, send_times[16];
static unsigned long address, mask, gateway;
/* Whether the model answers SIOCGIFADDR; while it is clear, an interface's
 * address cannot be read. */
static int getaddr_works;
/* What the parent's end of the handoff pipe answers: 'y', or 0 for an end of
 * file, a parent that died before it published the child. */
static char handshake_answer = 'y';
static jmp_buf child_exit; /* where model_exit returns to */
static int exit_status;    /* the status model_exit was given */
static void overlap_writer(void);

#define CHECK(condition, detail)                               \
    do {                                                       \
        if (!(condition)) {                                    \
            fprintf(stderr, "dhcpc-test: fail: %s\n", detail); \
            failures++;                                        \
        }                                                      \
    } while (0)

/**
 * Write into `buffer`, of `size` bytes, the path in the test's directory with
 * the last component of `path`, and return `buffer`.
 */
static const char *local_path(const char *path, char *buffer, size_t size)
{
    const char *base = strrchr(path, '/');

    snprintf(buffer, size, "%s/%s", directory, base ? base + 1 : path);
    return buffer;
}

/**
 * Open a claim or resolver file in the test's directory, or fail the claim's
 * open with `claim_error`. A file opened for writing while `overlap` is set
 * starts the second resolver writer.
 */
static FILE *model_fopen(const char *path, const char *mode)
{
    char local[256];
    FILE *f;

    if (strstr(path, "dhcpc.") && claim_error) {
        errno = claim_error;
        return NULL;
    }
    f = fopen(local_path(path, local, sizeof(local)), mode);
    if (f && overlap && !nested && strcmp(mode, "w") == 0) {
        overlap_writer();
    }
    return f;
}

/** Report the stream's error, or one when `claim_read_error` is set. */
static int model_ferror(FILE *f)
{
    return claim_read_error || ferror(f);
}

/**
 * Create an exclusive temporary file in the test's directory and give its
 * suffix back in `pattern`, or fail with `resolver_error`.
 */
static int model_mkstemp(char *pattern)
{
    char local[256];
    int fd;

    if (resolver_error) {
        errno = resolver_error;
        return -1;
    }
    local_path(pattern, local, sizeof(local));
    fd = mkstemp(local);
    if (fd >= 0) {
        memcpy(pattern + strlen(pattern) - 6, local + strlen(local) - 6, 6);
    }
    return fd;
}

/** Start the second resolver writer while the first has its file open. */
static FILE *model_fdopen(int fd, const char *mode)
{
    FILE *f = fdopen(fd, mode);

    if (f && overlap && !nested) {
        overlap_writer();
    }
    return f;
}

/** Require the published second version to remain intact until the rename. */
static int model_fclose(FILE *f)
{
    int rc = fclose(f);

    if (overlap && !nested) {
        char local[256], text[128];
        FILE *published = fopen(local_path("resolv.conf", local, sizeof(local)), "r");
        size_t n = 0;

        if (published) {
            n = fread(text, 1, sizeof(text) - 1, published);
            fclose(published);
        }
        text[n] = '\0';
        CHECK(strcmp(text, "search second\nnameserver 10.0.0.3\n") == 0,
              "published resolver stays complete during overlapping writes");
    }
    return rc;
}

/**
 * Open a file in the test's directory, or model an open of /dev/console or
 * /dev/null as descriptor 10 without a host device.
 *
 * `/dev/urandom` is refused, so a transaction id comes from the clock.
 */
static int model_open(const char *path, int flags, ...)
{
    char local[256];
    mode_t mode = 0;
    va_list args;

    if (strcmp(path, "/dev/urandom") == 0) {
        errno = ENOENT;
        return -1;
    }
    if (strncmp(path, "/dev/", 5) == 0) {
        int error = strcmp(path, "/dev/console") == 0 ? console_error : null_error;

        CHECK(stdio_model, "device opens belong to descriptor tests");
        if (error) {
            errno = error;
            return -1;
        }
        extra_open = 1;
        return 10;
    }
    if (flags & O_CREAT) {
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    if (record_error && (flags & O_CREAT) && strstr(path, "dhcpc.")) {
        errno = record_error;
        return -1;
    }
    return open(local_path(path, local, sizeof(local)), flags, mode);
}

/**
 * Close a file, or, in the descriptor rows, mark a standard descriptor or
 * descriptor 10 closed.
 */
static int model_close(int fd)
{
    if (stdio_model) {
        if (fd >= 0 && fd < 3) {
            descriptors[fd] = 0;
        } else if (fd == 10) {
            extra_open = 0;
        }
        return 0;
    }
    return close(fd);
}

/**
 * Replace standard descriptor `target` with the device opened, or fail with
 * EBADF when it is `dup_error`.
 */
static int model_dup2(int fd, int target)
{
    CHECK(stdio_model && fd == 10 && target >= 0 && target < 3,
          "descriptor replacement uses the opened console");
    if (target == dup_error) {
        errno = EBADF;
        return -1;
    }
    descriptors[target] = 2;
    return target;
}

/** Rename a file within the test's directory. */
static int model_rename(const char *from, const char *to)
{
    char src[256], dst[256];

    return rename(local_path(from, src, sizeof(src)), local_path(to, dst, sizeof(dst)));
}

/** Remove a file from the test's directory. */
static int model_remove(const char *path)
{
    char local[256];

    return remove(local_path(path, local, sizeof(local)));
}

/**
 * Model the interface ioctls. SIOCSIFADDR sets the address, and a changed
 * address gets the mask 255.0.0.0, so a row sees whether the client sets the
 * mask after it; setting 0, the removal, fails with EIO while `remove_error` is
 * set. SIOCGIFADDR reads the address while `getaddr_works` is set.
 * SIOCSIFNETMASK sets the mask. SIOCDELRT clears the gateway and fails with
 * ESRCH. SIOCADDRT sets the gateway, or fails with EINVAL for one off the
 * address's subnet. Any other request fails with EINVAL.
 */
static int model_ioctl(int fd, unsigned long request, ...)
{
    struct ifreq *ifr;
    struct sockaddr_in *sin;
    va_list args;
    void *arg;
    unsigned long value;

    (void)fd;
    va_start(args, request);
    arg = va_arg(args, void *);
    va_end(args);
    ifr = arg;
    sin = (struct sockaddr_in *)&ifr->ifr_addr;
    if (request == SIOCSIFADDR) {
        value = ntohl(sin->sin_addr.s_addr);
        if (!value && remove_error) {
            errno = EIO;
            return -1;
        }
        if (address != value) {
            mask = 0xff000000UL;
        }
        address = value;
        return 0;
    }
    if (request == SIOCGIFADDR && getaddr_works) {
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = htonl(address);
        return 0;
    }
    if (request == SIOCSIFNETMASK) {
        mask_calls++;
        mask = ntohl(sin->sin_addr.s_addr);
        return 0;
    }
    if (request == SIOCDELRT) {
        gateway = 0;
        errno = ESRCH;
        return -1;
    }
    if (request == SIOCADDRT) {
        struct rtentry *rt = arg;

        sin = (struct sockaddr_in *)&rt->rt_gateway;
        value = ntohl(sin->sin_addr.s_addr);
        if ((value & mask) != (address & mask)) {
            errno = EINVAL;
            return -1;
        }
        gateway = value;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

/** Report that the recorded client exists, without signalling a process. */
static int model_kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;
    return 0;
}

/**
 * Advance the modeled clock, or make the claim unreadable after the stop
 * signal; neither waits.
 */
static unsigned int model_sleep(unsigned int seconds)
{
    if (clock_model) {
        clock_now += (time_t)seconds;
        return 0;
    }
    sleep_calls++;
    claim_error = EIO;
    return 0;
}

/** Read the modeled clock while the wait rows run, the host's otherwise. */
static time_t model_time(time_t *out)
{
    time_t now = clock_model ? clock_now : time(NULL);

    if (out) {
        *out = now;
    }
    return now;
}

/** Let a poll on the modeled clock run out: nothing arrives in its time. */
static int model_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if (!clock_model) {
        return poll(fds, count, timeout);
    }
    polls++;
    clock_now += timeout / 1000;
    return 0;
}

/** Record when each request goes out, and send nothing. */
static ssize_t model_sendto(int fd, const void *buffer, size_t length, int flags,
                            const struct sockaddr *to, socklen_t size)
{
    (void)fd;
    (void)buffer;
    (void)flags;
    (void)to;
    (void)size;
    if (sends < (int)(sizeof(send_times) / sizeof(send_times[0]))) {
        send_times[sends] = clock_now;
    }
    sends++;
    return (ssize_t)length;
}

/** Give descriptors 50 and 51 as the handoff pipe, without making one. */
static int model_pipe(int fds[2])
{
    fds[0] = 50;
    fds[1] = 51;
    return 0;
}

/** Return 0, so that the caller goes on as the child. */
static pid_t model_fork(void)
{
    return 0;
}

/** Start no session; return this process's id. */
static pid_t model_setsid(void)
{
    return getpid();
}

/**
 * Read `handshake_answer` from the handoff pipe, the parent's word that it
 * published the child, or an end of file when it is 0; read any other
 * descriptor as read does.
 */
static ssize_t model_read(int fd, void *buf, size_t count)
{
    if (fd == 50 && count == 1) {
        if (!handshake_answer) {
            return 0;
        }
        *(char *)buf = handshake_answer;
        return 1;
    }
    return read(fd, buf, count);
}

/** Record a child's exit status and return to the row's setjmp. */
static void model_exit(int status)
{
    exit_status = status;
    longjmp(child_exit, 1);
}

#define pipe    model_pipe
#define fork    model_fork
#define setsid  model_setsid
#define read    model_read
#define _exit   model_exit
#define fopen   model_fopen
#define ferror  model_ferror
#define fclose  model_fclose
#define mkstemp model_mkstemp
#define fdopen  model_fdopen
#define open    model_open
#define close   model_close
#define dup2    model_dup2
#define rename  model_rename
#define remove  model_remove
#define ioctl   model_ioctl
#define kill    model_kill
#define sleep   model_sleep
#define time    model_time
#define poll    model_poll
#define sendto  model_sendto
#define main    dhcpc_main
#include "dhcpc.c"
#undef pipe
#undef fork
#undef setsid
#undef read
#undef _exit
#undef main
#undef fopen
#undef ferror
#undef fclose
#undef mkstemp
#undef fdopen
#undef open
#undef close
#undef dup2
#undef rename
#undef remove
#undef ioctl
#undef kill
#undef sleep
#undef time
#undef poll
#undef sendto

/** Publish the second lease while the first writer still has its file open. */
static void overlap_writer(void)
{
    struct dhcp_lease lease;

    nested = 1;
    memset(&lease, 0, sizeof(lease));
    strcpy(lease.domain, "second");
    lease.dns[0] = 0x0a000003UL;
    lease.dns_count = 1;
    CHECK(write_resolv(&lease) == 0, "second resolver writer succeeds");
    nested = 0;
}

/**
 * Write a claim for this process with the address `ip`, in the format dhcpc.c
 * writes, and mark the interface held; exit with status 2 when it cannot.
 */
static void seed_claim(unsigned long ip)
{
    int fd = model_open(pidpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0 || write_claim(fd, (long)getpid(), ip) != 0) {
        exit(2);
    }
    close(fd);
    holding = ip;
    holds_interface = 1;
}

/** Require failed removal and expiry to retain a retryable address record. */
static void check_cleanup(void)
{
    unsigned long recorded;
    int present;
    struct dhcp_lease lease;

    address = 0x0a000002UL;
    seed_claim(address);
    remove_error = 1;
    CHECK(unconfigure(7) == -1, "removal failure propagates");
    release_interface();
    CHECK(claim_read(&recorded, &present) == (long)getpid() && present && recorded == address &&
              holding == address,
          "failed removal retains address and claim");
    memset(&lease, 0, sizeof(lease));
    lease.seconds = 10;
    CHECK(hold(7, &lease, time(NULL) - 20) == -1, "expiry propagates removal failure");
    remove_error = 0;
    CHECK(unconfigure(7) == 0 && address == 0 && holding == 0, "removal retry succeeds");
    release_interface();
    CHECK(claim_read(&recorded, &present) == 0 && !present, "successful removal releases claim");
}

/**
 * Require apply_initial to stop discovery, and keep the claim's record, when a
 * first lease fails and its address cannot be taken off; to leave the
 * interface's address when the lease cannot be recorded; and to record nothing
 * to take off for a fixed address the offer repeats, which was not this
 * client's to take off.
 */
static void check_initial(void)
{
    struct dhcp_lease lease;
    unsigned long recorded;
    int present;

    memset(&lease, 0, sizeof(lease));
    lease.address = 0x0a000002UL;
    lease.router = 0x0a000001UL;
    seed_claim(0);
    resolver_error = EMFILE;
    remove_error = 1;
    CHECK(apply_initial(7, &lease) == -1, "initial cleanup failure stops discovery");
    CHECK(claim_read(&recorded, &present) == (long)getpid() && present &&
              recorded == lease.address && holding == lease.address,
          "initial cleanup failure retains the recovery record");
    remove_error = 0;
    CHECK(apply_initial(7, &lease) == 0 && holding == 0 && address == 0,
          "successful initial cleanup permits discovery");
    resolver_error = 0;
    release_interface();

    address = 0x0a000009UL;
    seed_claim(0);
    record_error = EACCES;
    CHECK(apply_initial(7, &lease) == 0 && holding == 0 && address == 0x0a000009UL,
          "a first lease that cannot be recorded leaves the interface's address");
    record_error = 0;
    release_interface();

    address = lease.address;
    getaddr_works = 1;
    seed_claim(0);
    CHECK(apply_initial(7, &lease) == 1 && claim_read(&recorded, &present) == (long)getpid() &&
              present && recorded == 0 && address == lease.address,
          "a first lease the interface already had is recorded as nothing to take off");
    getaddr_works = 0;
    release_interface();
    address = 0;
}

/** Apply maskless leases to an unchanged address with a pre-existing mask. */
static void check_mask(void)
{
    struct dhcp_lease lease;
    char why[96];
    unsigned long ips[] = {0x0a000002UL, 0xac100002UL, 0xc0a80002UL};
    unsigned long masks[] = {0xff000000UL, 0xffff0000UL, 0xffffff00UL};
    unsigned long routers[] = {0x0a010001UL, 0xac100101UL, 0xc0a80001UL};
    unsigned int i;

    for (i = 0; i < 3; i++) {
        memset(&lease, 0, sizeof(lease));
        lease.address = ips[i];
        lease.router = routers[i];
        lease.seconds = 60;
        address = lease.address;
        mask = 0xfffffffcUL;
        mask_calls = 0;
        seed_claim(address);
        CHECK(dhcp_lease_usable(&lease, why, sizeof(why)) == 0, "maskless lease validates");
        CHECK(apply(7, &lease, 1) == 0 && mask == masks[i] && mask_calls == 1 &&
                  gateway == lease.router,
              "application uses validation mask on unchanged address");
    }
    CHECK(unconfigure(7) == 0, "mask test cleanup");
    release_interface();
}

/** Check overlapping publication and an empty resolver replacement. */
static void check_resolver(void)
{
    struct dhcp_lease lease;
    char local[256], text[128];
    FILE *f;
    struct stat st;
    size_t n;

    memset(&lease, 0, sizeof(lease));
    strcpy(lease.domain, "first");
    lease.dns_count = 1;
    lease.dns[0] = 0x0a000002UL;
    overlap = 1;
    CHECK(write_resolv(&lease) == 0, "first resolver writer succeeds");
    overlap = 0;
    local_path(RESOLV_CONF, local, sizeof(local));
    f = fopen(local, "r");
    if (!f) {
        exit(2);
    }
    n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = '\0';
    fclose(f);
    CHECK(strcmp(text, "search first\nnameserver 10.0.0.2\n") == 0,
          "last resolver rename publishes a complete version");
    CHECK(stat(local, &st) == 0 && (st.st_mode & 0777) == 0644,
          "resolver remains readable by ordinary users");
    memset(&lease, 0, sizeof(lease));
    CHECK(write_resolv(&lease) == 0 && stat(local, &st) == 0 && st.st_size == 0,
          "lease without DNS replaces old resolver");
    remove(local);
}

/** Check absent claims and I/O failures before and during the stop wait. */
static void check_claim(void)
{
    int errors[] = {EIO, EMFILE, EACCES};
    unsigned long recorded;
    int present;
    unsigned int i;

    claim_error = ENOENT;
    CHECK(claim_read(&recorded, &present) == 0 && !present && !recorded, "absent claim is absent");
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        claim_error = errors[i];
        CHECK(claim_read(&recorded, &present) == 0 && present && !recorded,
              "unreadable claim remains present");
        CHECK(claim_read(NULL, NULL) == 0, "claim outputs may be null");
    }
    claim_error = 0;
    seed_claim(0x0a000002UL);
    claim_read_error = 1;
    CHECK(claim_read(&recorded, &present) == 0 && present && !recorded, "claim read error is busy");
    claim_read_error = 0;
    CHECK(stop_client() == 1 && sleep_calls == STOP_WAIT_SECONDS,
          "stop does not report unreadable claim as removed");
    claim_error = 0;
    model_remove(pidpath);
    holding = 0;
    holds_interface = 0;
}

/** Require one wait for a reply to last to its deadline, in several polls. */
static void check_receive_wait(void)
{
    struct dhcp_lease lease;
    time_t deadline;

    clock_model = 1;
    clock_now = 1000000;
    polls = 0;
    deadline = clock_now + 2 * POLL_CHUNK + 30;
    CHECK(receive(7, &lease, DHCP_ACK, deadline) == 0 && clock_now == deadline && polls == 3,
          "receive waits until its deadline");
    clock_model = 0;
}

/**
 * Require a rebinding request to wait for half of what is left of the lease.
 *
 * The lease is an hour long and asked for 3200 seconds ago, so rebinding has
 * begun and 400 seconds are left: the second request goes 200 seconds after
 * the first. Nothing answers, and the lease runs out at the end.
 */
static void check_rebinding(void)
{
    struct dhcp_lease lease;
    time_t asked = 1000000;

    memset(&lease, 0, sizeof(lease));
    lease.address = 0x0a000002UL;
    lease.seconds = 3600;
    address = lease.address;
    holding = lease.address;
    holds_interface = 0;
    clock_model = 1;
    clock_now = asked + 3200;
    sends = 0;
    CHECK(hold(7, &lease, asked) == 0 && address == 0 && clock_now == asked + 3600,
          "rebinding gives the address up when the lease runs out");
    CHECK(sends > 1 && send_times[0] == asked + 3200 && send_times[1] == asked + 3400,
          "rebinding waits for half of what is left of the lease");
    clock_model = 0;
}

/** Check console success, fallback, open failure and each dup2 failure. */
static void check_stdio(void)
{
    int fault, i, rc;

    stdio_model = 1;
    for (fault = 0; fault < 6; fault++) {
        console_error = fault == 1 || fault == 2 ? EACCES : 0;
        null_error = fault == 2 ? EMFILE : 0;
        dup_error = fault >= 3 ? fault - 3 : -1;
        for (i = 0; i < 3; i++) {
            descriptors[i] = 1;
        }
        exit_status = 0;
        if (setjmp(child_exit) == 0) {
            rc = go_background();
        } else {
            rc = -1;
        }
        CHECK(rc == (fault >= 2 ? -1 : 0), "redirection failure propagates");
        CHECK(exit_status == (fault >= 2 ? 1 : 0), "failed redirection exits child");
        CHECK(!extra_open, "replacement descriptor closes");
        for (i = 0; i < 3; i++) {
            CHECK(descriptors[i] == (fault >= 2 ? 0 : 2),
                  "inherited standard descriptor is detached");
        }
    }
    stdio_model = 0;
}

/**
 * Require a child that was never handed the claim to leave an address the
 * interface had before the first lease, and to remove the claim.
 */
static void check_handoff_adopted(void)
{
    unsigned long recorded;
    int present;

    stdio_model = 1;
    handshake_answer = 0;
    getaddr_works = 1;
    remove_error = 0;
    address = 0x0a000002UL;
    seed_claim(address);
    adopted = 1;
    exit_status = 0;
    if (setjmp(child_exit) == 0) {
        (void)go_background();
    }
    CHECK(exit_status == 1 && address == 0x0a000002UL,
          "a child never handed the claim leaves an address it did not put on");
    CHECK(claim_read(&recorded, &present) <= 0 && !present,
          "a child never handed the claim removes it");
    adopted = 0;
    getaddr_works = 0;
    handshake_answer = 'y';
    stdio_model = 0;
    holds_interface = 0;
    holding = 0;
    address = 0;
}

/**
 * Require a claim that has gone to be written again with the address this
 * client put on the interface, and with none for an address it adopted, so
 * that a later `dhcpc -k` takes off only what this client added.
 */
static void check_claim_rewritten(void)
{
    unsigned long recorded;
    int present;

    holds_interface = 1;
    holding = 0x0a000002UL;
    adopted = 1;
    (void)model_remove(pidpath);
    CHECK(claim_is_ours(0) == 1 && claim_read(&recorded, &present) == (long)getpid() && present &&
              recorded == 0,
          "a claim written again for an adopted address records nothing to take off");
    adopted = 0;
    (void)model_remove(pidpath);
    CHECK(claim_is_ours(0) == 1 && claim_read(&recorded, &present) == (long)getpid() && present &&
              recorded == 0x0a000002UL,
          "a claim written again records the address this client put on");
    (void)model_remove(pidpath);
    holds_interface = 0;
    holding = 0;
}

/**
 * Make the test's directory, run the rows and remove the directory; return 0
 * when every row passes, 1 when one fails, and 2 when the directory cannot be
 * made.
 */
int main(void)
{
    if (!mkdtemp(directory)) {
        return 2;
    }
    snprintf(pidpath, sizeof(pidpath), "%s/dhcpc.%s.pid", RUN_DIR, ifname);
    check_cleanup();
    check_initial();
    check_mask();
    check_resolver();
    check_claim();
    check_receive_wait();
    check_rebinding();
    check_stdio();
    check_handoff_adopted();
    check_claim_rewritten();
    CHECK(rmdir(directory) == 0, "temporary files are removed");
    if (failures) {
        return 1;
    }
    puts("dhcpc-test: result: pass");
    return 0;
}
