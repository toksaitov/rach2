/* wdtrial: arm /dev/watchdog, feed it, then stop feeding until it resets the
 * machine; 78-1b's Lenovo trial, built there with `tcc` and run over SSH. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Linux's <linux/watchdog.h>, which musl does not install. */
#define WDIOC_GETSUPPORT  0x80285700u
#define WDIOC_GETSTATUS   0x80045701u
#define WDIOC_KEEPALIVE   0x80045705u
#define WDIOC_SETTIMEOUT  0xC0045706u
#define WDIOC_GETTIMEOUT  0x80045707u
#define WDIOC_GETTIMELEFT 0x8004570Au

static volatile sig_atomic_t stop;
static struct timespec origin;
static int fd = -1;

/** Note a signal; the loops then disarm the timer and leave. */
static void caught(int number)
{
    (void)number;
    stop = 1;
}

/** Seconds since the device was opened, to a tenth. */
static double since(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - origin.tv_sec) + (double)(now.tv_nsec - origin.tv_nsec) / 1e9;
}

/** Read one integer ioctl; -1 with the error printed. */
static int get(unsigned long request, const char *name)
{
    int value = -1;

    if (ioctl(fd, request, &value) != 0) {
        printf("wdtrial: %s: %s\n", name, strerror(errno));
        return -1;
    }
    return value;
}

/** Halt the timer by magic close and exit with `status`. */
static void disarm(int status, const char *why)
{
    if (write(fd, "V", 1) != 1 || close(fd) != 0) {
        printf("wdtrial: %.1f s: magic close failed: %s\n", since(), strerror(errno));
        exit(3);
    }
    printf("wdtrial: %.1f s: disarmed by magic close: %s\n", since(), why);
    exit(status);
}

/** Sleep until `when` seconds after the open, or until a signal. */
static void sleep_until(double when)
{
    struct timespec step = {0, 100000000};

    while (!stop && since() < when) {
        nanosleep(&step, NULL);
    }
}

/** wdtrial TIMEOUT FEED: arm at TIMEOUT seconds, feed every 10 s for FEED
 * seconds, alternating write and WDIOC_KEEPALIVE, then feed no more and
 * print the time left each second. A version 2 TCO resets 2 x TIMEOUT
 * seconds after the last feed; if the machine still runs at 4 x TIMEOUT +
 * 30 s, or a signal comes, the timer is halted by magic close. */
int main(int argc, char **argv)
{
    struct {
        unsigned options, firmware;
        char identity[32];
    } info;
    struct sigaction action;
    int timeout, feed, value, n;
    double last;

    if (argc != 3 || (timeout = atoi(argv[1])) < 3 || (feed = atoi(argv[2])) < 0) {
        fprintf(stderr, "wdtrial: usage: wdtrial TIMEOUT FEED\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&action, 0, sizeof(action));
    action.sa_handler = caught;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    clock_gettime(CLOCK_MONOTONIC, &origin);
    fd = open("/dev/watchdog", O_WRONLY);
    if (fd < 0) {
        printf("wdtrial: open: %s\n", strerror(errno));
        return 1;
    }
    printf("wdtrial: 0.0 s: armed by open\n");
    if (ioctl(fd, WDIOC_GETSUPPORT, &info) != 0) {
        printf("wdtrial: GETSUPPORT: %s\n", strerror(errno));
        disarm(1, "GETSUPPORT failed");
    }
    info.identity[31] = '\0';
    printf("wdtrial: identity %s, firmware %u, options 0x%x\n", info.identity, info.firmware,
           info.options);
    value = timeout;
    if (ioctl(fd, WDIOC_SETTIMEOUT, &value) != 0) {
        printf("wdtrial: SETTIMEOUT %d: %s\n", timeout, strerror(errno));
        disarm(1, "SETTIMEOUT failed");
    }
    printf("wdtrial: %.1f s: timeout set %d, read back %d, status 0x%x\n", since(), value,
           get(WDIOC_GETTIMEOUT, "GETTIMEOUT"), get(WDIOC_GETSTATUS, "GETSTATUS"));
    last = since();
    for (n = 1; n * 10 <= feed && !stop; n++) {
        sleep_until(n * 10.0);
        if (stop) {
            break;
        }
        value = get(WDIOC_GETTIMELEFT, "GETTIMELEFT");
        if (n & 1 ? write(fd, "x", 1) != 1 : ioctl(fd, WDIOC_KEEPALIVE, 0) != 0) {
            printf("wdtrial: feed: %s\n", strerror(errno));
            disarm(1, "a feed failed");
        }
        last = since();
        printf("wdtrial: %.1f s: left %d s before feed %d by %s, status 0x%x\n", last, value, n,
               n & 1 ? "write" : "KEEPALIVE", get(WDIOC_GETSTATUS, "GETSTATUS"));
    }
    if (stop) {
        disarm(2, "signal while feeding");
    }
    printf("wdtrial: %.1f s: feeding stopped; a reset is due by %.1f s\n", last,
           last + 2.0 * timeout);
    for (n = 1; !stop && since() < last + 4.0 * timeout + 30.0; n++) {
        sleep_until(last + n);
        printf("wdtrial: %.1f s: left %d s, unfed\n", since(), get(WDIOC_GETTIMELEFT, "GETTIMELEFT"));
    }
    disarm(stop ? 2 : 1, stop ? "signal while unfed" : "FAIL: no reset");
    return 1;
}
