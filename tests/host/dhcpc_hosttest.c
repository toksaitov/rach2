/**
 * Check DHCP cleanup, masks, resolver updates, claims and descriptor release.
 *
 * host-deps: utils/dhcpmsg.c utils/ipcheck.c
 * host-cflags: -I tests/host/compat/netutil
 *
 * Files use a private host directory. Ioctls and standard descriptors use
 * models; no host interface or device is opened. Resolver writes overlap at
 * stream creation, before either writer can publish the other's bytes.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <net/route.h>

static char directory[] = "/tmp/dhcpc-test.XXXXXX";
static int failures, remove_error, claim_error, claim_read_error, resolver_error;
static int overlap, nested, stdio_model, console_error, null_error, dup_error;
static int descriptors[3], extra_open, mask_calls, sleep_calls;
static unsigned long address, mask, gateway;
static jmp_buf child_exit;
static int exit_status;
static void overlap_writer(void);

#define CHECK(condition, detail)                               \
    do {                                                       \
        if (!(condition)) {                                    \
            fprintf(stderr, "dhcpc-test: fail: %s\n", detail); \
            failures++;                                        \
        }                                                      \
    } while (0)

/** Map every utility path into the private test directory. */
static const char *local_path(const char *path, char *buffer, size_t size)
{
    const char *base = strrchr(path, '/');

    snprintf(buffer, size, "%s/%s", directory, base ? base + 1 : path);
    return buffer;
}

/** Open a claim or resolver file, or inject a claim-open error. */
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

/** Report an injected claim read failure. */
static int model_ferror(FILE *f)
{
    return claim_read_error || ferror(f);
}

/** Create an exclusive temporary file and return its suffix to the caller. */
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

/** Map regular files and model console or null opens without host devices. */
static int model_open(const char *path, int flags, ...)
{
    char local[256];
    mode_t mode = 0;
    va_list args;

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
    return open(local_path(path, local, sizeof(local)), flags, mode);
}

/** Close a real file, or release a modeled standard descriptor. */
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

/** Replace a standard descriptor or fail at the selected destination. */
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

/** Rename only private test files. */
static int model_rename(const char *from, const char *to)
{
    char src[256], dst[256];

    return rename(local_path(from, src, sizeof(src)), local_path(to, dst, sizeof(dst)));
}

/** Remove only private test files. */
static int model_remove(const char *path)
{
    char local[256];

    return remove(local_path(path, local, sizeof(local)));
}

/** Model address retention on an unchanged address and route subnet checks. */
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

/** Keep the recorded client alive without signalling a host process. */
static int model_kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;
    return 0;
}

/** Make the claim unreadable after the stop signal, without waiting. */
static unsigned int model_sleep(unsigned int seconds)
{
    (void)seconds;
    sleep_calls++;
    claim_error = EIO;
    return 0;
}

/** Supply the child side of a successful handoff without forking a process. */
static int model_pipe(int fds[2])
{
    fds[0] = 50;
    fds[1] = 51;
    return 0;
}

static pid_t model_fork(void)
{
    return 0;
}

static pid_t model_setsid(void)
{
    return getpid();
}

/** Read the parent's publication acknowledgement. */
static ssize_t model_read(int fd, void *buf, size_t count)
{
    if (fd == 50 && count == 1) {
        *(char *)buf = 'y';
        return 1;
    }
    return read(fd, buf, count);
}

/** Return a child exit to the descriptor test. */
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

/** Create a claim using the production format. */
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

/** Reject a failed initial lease when its partial address cannot be removed. */
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
    check_stdio();
    CHECK(rmdir(directory) == 0, "temporary files are removed");
    if (failures) {
        return 1;
    }
    puts("dhcpc-test: result: pass");
    return 0;
}
