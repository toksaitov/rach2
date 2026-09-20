/**
 * Exercise completed connects, framebuffer ownership and clock rollover.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * Unused syscall exports are private so the native compiler removes their
 * hardware paths. User pointers are offsets into a checked byte array.
 */
#include "isr.h"
#include "network/socket/socket.h"
#include "fs/fd.h"
struct tty;
static struct tty *fd_tty(fd_entry_t *entry);
static void init_syscall(void);
static int32_t raw_seek_resolve(uint64_t cur, uint64_t size, long long delta, int whence,
                                uint64_t *out);
static int pgrp_in_session(int pgid, int sid);
static int user_access_ok(uint32_t addr, uint32_t len);
static int user_buffer_ok(uint32_t addr, uint32_t len, int write);
static int32_t sys_socketcall(isr_params_t *params);
static int32_t dgram_send(struct socket *sock, uint32_t dst_ip, uint16_t dst_port,
                          const uint8_t *buf, uint16_t len);
#include "syscall.c"
#include "syscall_net.c"

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);

static process_t proc;
process_t *current_proc = &proc;
volatile uint32_t kernel_ticks;
volatile uint32_t kernel_uptime_seconds;
volatile uint32_t kernel_second_ticks;
bool vga_graphics_mode;
uint32_t net_ip_addr = 0x0a00020fu;
static struct socket sock;
static struct tcb control;
static fd_entry_t entry;
static struct open_file description;
static uint8_t user[1024];
static int failures, enters, exits, palettes, masked, saves;
static int completion, immediate, waits;

#define CHECK(c, detail)                                    \
    do {                                                    \
        if (!(c)) {                                         \
            dprintf(2, "syscall-test: fail: %s\n", detail); \
            failures++;                                     \
        }                                                   \
    } while (0)

/** Refuse every range outside the model's user memory. */
int user_range_ok(uint32_t addr, uint32_t len)
{
    return addr && addr <= sizeof(user) && len <= sizeof(user) - addr;
}

/** Model mapped user pages after the range check. */
int vm_prefault_range(uint32_t addr, uint32_t len, int write)
{
    (void)write;
    return user_range_ok(addr, len);
}

/** Copy the syscall result without truncating a native pointer. */
int copy_to_user(void *dst, const void *src, uint32_t len)
{
    uint32_t at = (uint32_t)(unsigned long)dst;
    if (!user_range_ok(at, len))
        return -EFAULT;
    k_memcpy(user + at, src, len);
    return 0;
}

/** Copy a socket address from the same arena. */
int copy_from_user(void *dst, const void *src, uint32_t len)
{
    uint32_t at = (uint32_t)(unsigned long)src;
    if (!user_range_ok(at, len))
        return -EFAULT;
    k_memcpy(dst, user + at, len);
    return 0;
}

/** Record interrupt masking around a coherent clock snapshot. */
uint32_t kirq_save(void)
{
    uint32_t prior = (uint32_t)masked;
    masked = 1;
    saves++;
    return prior;
}

/** Restore the caller's interrupt state. */
void kirq_restore(uint32_t flags)
{
    CHECK(masked, "clock snapshot masks interrupts");
    masked = (int)flags;
}

/** Supply a distinct real-time value to check clock selection. */
void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 123;
    *nanoseconds = 450000000;
}

/** Record the actual request to enter graphics mode. */
uint8_t *vga_mode13h_enter(void)
{
    enters++;
    vga_graphics_mode = true;
    return NULL;
}

/** Record the request that would restore the text display. */
void vga_mode13h_exit(void)
{
    exits++;
    vga_graphics_mode = false;
}

/** Record palette writes without accessing a hardware port. */
void vga_mode13h_set_palette(const uint8_t *data)
{
    (void)data;
    palettes++;
}

/** Resolve the only socket descriptor in the model. */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    (void)table;
    return fd == 3 ? &entry : NULL;
}

/** No network interrupt runs in this model. */
uint32_t net_lock(void)
{
    return 0;
}

/** Balance the network critical section. */
void net_unlock(uint32_t flags)
{
    (void)flags;
}

/** Datagram allocation is not used by these stream rows. */
uint16_t udp_bind_ephemeral(struct socket *s)
{
    (void)s;
    return 0;
}

/** Start a connection with completion before or during the wait. */
int tcp_connect(struct socket *s, uint32_t ip, uint16_t port)
{
    (void)ip;
    (void)port;
    s->tcb = &control;
    control.state = immediate ? completion : TCP_SYN_SENT;
    return 0;
}

/** Deliver the final state before the caller next evaluates its predicate. */
int wq_wait_event(wait_queue_t *q, int (*ready)(void *), void *arg)
{
    (void)q;
    waits++;
    control.state = completion;
    CHECK(ready(arg), "completed handshake wakes caller");
    return ready(arg) ? 0 : -EINTR;
}

/** A FIN before scheduling must pass both the wait and the return check. */
static void connects(void)
{
    int i;
    int states[] = {TCP_ESTABLISHED, TCP_CLOSE_WAIT, TCP_CLOSED};
    sock.family = AF_INET;
    sock.type = SOCK_STREAM;
    entry.type = FD_TYPE_SOCKET;
    entry.sock = &sock;
    entry.ofd = &description;
    user[16] = AF_INET;
    user[20] = 127;
    user[23] = 1;
    for (i = 0; i < 3; i++) {
        completion = states[i];
        sock.tcb = NULL;
        CHECK(do_connect(3, 16, 16) == (i == 2 ? -ECONNREFUSED : 0), "blocking connect result");
    }
    CHECK(waits == 3, "blocking connects use wait predicate");
    description.status_flags = OFD_NONBLOCK;
    completion = TCP_CLOSE_WAIT;
    immediate = 1;
    sock.tcb = NULL;
    CHECK(do_connect(3, 16, 16) == 0, "completed nonblocking connect");
    immediate = 0;
    sock.tcb = NULL;
    CHECK(do_connect(3, 16, 16) == -EINPROGRESS, "pending nonblocking connect");
    CHECK(!connect_done(&sock), "pending handshake stays asleep");
    sock.tcb = NULL;
    CHECK(connect_done(&sock), "removed connection wakes caller");
}

/** A stale descriptor cannot alter another process's mode or palette. */
static void framebuffer(void)
{
    proc.pid = 1;
    CHECK(framebuffer_ioctl(FB_MODE_13H, 0) == 0, "acquire framebuffer");
    CHECK(enters == 1 && fb_owner_pid == 1 && vga_graphics_mode, "graphics entered");
    CHECK(framebuffer_ioctl(FB_MODE_13H, 0) == 0 && enters == 1, "mode request is idempotent");
    CHECK(framebuffer_ioctl(FB_MODE_TEXT, 0) == 0 && !fb_owner_pid, "release framebuffer");
    CHECK(framebuffer_ioctl(FB_MODE_13H, 0) == 0, "reacquire framebuffer");
    CHECK(enters == 2 && vga_graphics_mode && fb_owner_pid == 1, "graphics reentered");
    CHECK(framebuffer_ioctl(FB_MODE_TEXT, 0) == 0, "release before second owner");
    proc.pid = 2;
    CHECK(framebuffer_ioctl(FB_MODE_13H, 0) == 0, "second owner acquires");
    proc.pid = 1;
    CHECK(framebuffer_ioctl(FB_MODE_TEXT, 0) == -EBUSY, "stale text request refused");
    CHECK(framebuffer_ioctl(FB_MODE_13H, 0) == -EBUSY, "stale graphics request refused");
    CHECK(framebuffer_ioctl(FB_SETPALETTE, 16) == -EBUSY, "stale palette request refused");
    CHECK(fb_owner_pid == 2 && vga_graphics_mode && enters == 3 && exits == 2 && !palettes,
          "second owner's display unchanged");
    proc.pid = 2;
    CHECK(framebuffer_ioctl(FB_SETPALETTE, 0) == -EFAULT, "null palette refused");
    CHECK(framebuffer_ioctl(FB_SETPALETTE, 900) == -EFAULT, "short palette refused");
    CHECK(framebuffer_ioctl(FB_SETPALETTE, 16) == 0 && palettes == 1, "owner sets palette");
    CHECK(framebuffer_ioctl(0, 0) == -ENOTTY, "unknown framebuffer request refused");
}

/** Read across the tick wrap and the following whole-second boundary. */
static void clocks(void)
{
    isr_params_t p;
    struct kernel_timespec ts;
    int i;
    uint32_t ticks[] = {0xfffffffeu, 0xffffffffu, 0, 3, 4};
    int32_t seconds[] = {42949672, 42949672, 42949672, 42949672, 42949673};
    int32_t nanos[] = {940000000, 950000000, 960000000, 990000000, 0};
    k_memset(&p, 0, sizeof(p));
    p.ebx = 1;
    p.ecx = 16;
    for (i = 0; i < 5; i++) {
        kernel_ticks = ticks[i];
        kernel_uptime_seconds = (uint32_t)seconds[i];
        kernel_second_ticks = (uint32_t)nanos[i] / 10000000;
        CHECK(sys_clock_gettime(&p) == 0, "read monotonic clock");
        k_memcpy(&ts, user + 16, sizeof(ts));
        CHECK(ts.tv_sec == seconds[i] && ts.tv_nsec == nanos[i], "monotonic survives tick wrap");
        CHECK(saves == i + 1 && !masked, "clock restores interrupt state");
    }
    p.ebx = 0;
    CHECK(sys_clock_gettime(&p) == 0, "read realtime clock");
    k_memcpy(&ts, user + 16, sizeof(ts));
    CHECK(ts.tv_sec == 123 && ts.tv_nsec == 450000000, "realtime unchanged");
    p.ebx = 2;
    CHECK(sys_clock_gettime(&p) == -EINVAL, "unknown clock refused");
    p.ecx = 0;
    CHECK(sys_clock_gettime(&p) == -EFAULT, "null timespec refused");
}

/** Run the independent syscall state cases. */
int main(void)
{
    connects();
    framebuffer();
    clocks();
    if (failures)
        return 1;
    printf("syscall-test: pass: connect, framebuffer and clock\n");
    return 0;
}
