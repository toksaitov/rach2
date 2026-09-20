/**
 * Check proc stat generation when every text allocation fails.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * Unused exports are private so the compiler removes unrelated generators.
 */
#include "lib/kint.h"
struct vpath;
struct fd_entry;
static int procfs_resolve_pid(int pid);
static int procfs_open(const struct vpath *vp, int flags);
static int procfs_readlink_fd(int pid, int fd, char *buf, int cap);
static int procfs_nth_pid(int n);
static uint32_t procfs_ino_dir(void);
static uint32_t procfs_ino_cmdline(void);
static uint32_t procfs_ino_netdir(void);
static uint32_t procfs_ino_netdev(void);
static uint32_t procfs_ino_disk(void);
static uint32_t procfs_ino_uptime(void);
static uint32_t procfs_ino_statsys(void);
static uint32_t procfs_ino_meminfo(void);
static uint32_t procfs_ino_mounts(void);
static uint32_t procfs_ino_usb(void);
static uint32_t procfs_ino_loop(void);
static uint32_t procfs_ino_interrupts(void);
static uint32_t procfs_ino_console(void);
static uint32_t procfs_ino_filesystems(void);
static uint32_t procfs_ino_netroute(void);
static uint32_t procfs_ino_netarp(void);
static uint32_t procfs_ino_nettcp(void);
static uint32_t procfs_fd_ino(struct fd_entry *e);
static uint32_t procfs_ino_pid(int pid);
#include "fs/procfs.c"

extern int printf(const char *format, ...);
extern void *malloc(unsigned long size);
extern void free(void *ptr);

static process_t process;
static int refuse, allocations, failures;

/** Record a failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("procfs-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Return the one process whose stat file is generated. */
process_t *get_process(uint32_t pid)
{
    return pid == 7 ? &process : NULL;
}

/** Fail every text allocation when requested. */
void *kmalloc(size_t size)
{
    allocations++;
    return refuse ? NULL : malloc(size);
}

/** Release the snapshot's host memory. */
void kfree(void *ptr)
{
    free(ptr);
}

/** Exercise empty and partially allocated text, then normal generation. */
int main(void)
{
    struct procfs_text text;
    int len;

    process.state = PROC_READY;
    k_memcpy(process.comm, "test", 5);
    procfs_text_init(&text);
    refuse = 1;
    len = procfs_gen_stat(&text, 7);
    check(len == 0 && text.nomem && !text.buf && allocations > 0,
          "stat allocation failure retains empty text for ENOMEM");
    refuse = 0;
    check(procfs_text_room(&text, 0) == 0, "allocate partial text");
    text.cap = 2;
    text.nomem = 0;
    refuse = 1;
    len = procfs_gen_stat(&text, 7);
    check(len == 1 && text.nomem && text.buf[0] == '7',
          "stat growth failure retains partial text for disposal");
    kfree(text.buf);
    procfs_text_init(&text);
    refuse = 0;
    len = procfs_gen_stat(&text, 7);
    check(len > 0 && !text.nomem && text.buf[len] == '\0' && text.buf[len - 1] == '\n',
          "stat success terminates the complete snapshot");
    check(__builtin_memcmp(text.buf, "7 (test) R ", 11) == 0, "stat fields retain their order");
    kfree(text.buf);
    if (!failures) {
        printf("procfs-hosttest: result: pass: stat allocation failure\n");
    }
    return failures != 0;
}
