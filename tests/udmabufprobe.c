/* Export sealed memfds, map their pinned pages and exchange sync files. */
#include "probe.h"
#include "probe_report.h"
#include "../rach2/sys/drm/uapi.h"

#if defined __x86_64__
#define SYS_MEMFD_CREATE 319
#else
#define SYS_MEMFD_CREATE 356
#endif

static int fds[32], nfds;
static long maps[4];
static unsigned nmaps;

/** Close every descriptor and unmap every page the probe made. */
static void cleanup(void)
{
    unsigned i;
    for (i = 0; i < nmaps; i++)
        syscall2(SYS_MUNMAP, maps[i], 8192);
    while (nfds)
        syscall1(SYS_CLOSE, fds[--nfds]);
}

/** Report a failed check and release all resources before exiting. */
static void check(int condition, const char *detail)
{
    if (condition)
        return;
    report_stop(detail);
    probe_line("UDMABUFPROBE FAIL: ", detail, 0);
    cleanup();
    syscall1(SYS_EXIT, 1);
    for (;;) {
    }
}

/** Remember a descriptor for cleanup on both success and failure. */
static int keep(long fd)
{
    check(fd >= 0 && nfds < 32, "descriptor allocation");
    fds[nfds++] = (int)fd;
    return (int)fd;
}

/** Make a sealable two-page memfd and optionally seal its minimum size. */
static int memfd(int seal)
{
    int fd = keep(syscall2(SYS_MEMFD_CREATE, (long)"udmabufprobe", 3));
    check(probe_ftruncate64(fd, 8192) == 0, "memfd size");
    if (seal)
        check(syscall3(SYS_FCNTL, fd, 1033, 2) == 0, "shrink seal");
    return fd;
}

/** Map both pages shared and keep the mapping until cleanup. */
static unsigned char *map_buf(int fd)
{
    long at = probe_mmap(0, 8192, 3, 1, fd, 0);
    check((unsigned long)at < (unsigned long)-4095, "shared mapping");
    maps[nmaps++] = at;
    return (unsigned char *)at;
}

/** Exercise creation, Linux's range errors and descriptor identity. */
static int create_rows(int dev, int sealed, int plain)
{
    struct udmabuf_create data;
    unsigned char st[KSTAT_SIZE], other[KSTAT_SIZE];
    long long size;
    int fd, second;
    data.memfd = plain;
    data.flags = 1;
    data.offset = 0;
    data.size = 8192;
    check(syscall3(SYS_IOCTL, dev, UDMABUF_CREATE, (long)&data) == -22, "unsealed range");
    data.memfd = (uint32_t)-1;
    check(syscall3(SYS_IOCTL, dev, UDMABUF_CREATE, (long)&data) == -77, "invalid memfd");
    data.memfd = sealed;
    data.offset = 1;
    check(syscall3(SYS_IOCTL, dev, UDMABUF_CREATE, (long)&data) == -22, "misaligned offset");
    data.offset = 0;
    data.size = 4097;
    check(syscall3(SYS_IOCTL, dev, UDMABUF_CREATE, (long)&data) == -22, "misaligned size");
    data.size = 12288;
    check(syscall3(SYS_IOCTL, dev, UDMABUF_CREATE, (long)&data) == -22, "range past end");
    data.size = 0;
    check(syscall3(SYS_IOCTL, dev, UDMABUF_CREATE, (long)&data) == -22, "empty range");
    data.size = 8192;
    fd = keep(syscall3(SYS_IOCTL, dev, UDMABUF_CREATE, (long)&data));
    second = keep(syscall3(SYS_IOCTL, dev, UDMABUF_CREATE, (long)&data));
    check(syscall3(SYS_FCNTL, fd, 1, 0) == 1, "close-on-exec");
    check(probe_llseek(fd, 0, 2, &size) == 0 && size == 8192, "exported size");
    check(syscall3(SYS_LSEEK, fd, 1, 2) == -22 && syscall3(SYS_LSEEK, fd, 0, 1) == -22,
          "restricted seeks");
    check(syscall2(SYS_FSTAT, fd, (long)st) == 0 && syscall2(SYS_FSTAT, second, (long)other) == 0,
          "buffer stat");
    check(*(uint64_t *)(void *)(st + KSTAT_INO) != *(uint64_t *)(void *)(other + KSTAT_INO),
          "distinct buffer inodes");
    check(*(uint64_t *)(void *)(st + KSTAT_SIZE_AT) == 8192, "stat size");
    report_held();
    return fd;
}

/** Map the same pages in opposite orders through CREATE_LIST. */
static void map_rows(int dev, int sealed, int fd)
{
    struct {
        struct udmabuf_create_list head;
        struct udmabuf_create_item items[2];
    } list;
    unsigned char *a = map_buf(sealed), *b = map_buf(fd), *c;
    int reversed;
    long child;
    int status;
    a[0] = 0x37;
    a[4096] = 0x91;
    check(b[0] == 0x37 && b[4096] == 0x91, "shared memfd bytes");
    b[17] = 0x54;
    check(a[17] == 0x54, "buffer write reaches memfd");
    list.head.flags = 0;
    list.head.count = 2;
    list.items[0].memfd = list.items[1].memfd = sealed;
    list.items[0].__pad = list.items[1].__pad = 0;
    list.items[0].offset = 4096;
    list.items[1].offset = 0;
    list.items[0].size = list.items[1].size = 4096;
    reversed = keep(syscall3(SYS_IOCTL, dev, UDMABUF_CREATE_LIST, (long)&list));
    c = map_buf(reversed);
    check(c[0] == 0x91 && c[4096] == 0x37 && c[4113] == 0x54, "list concatenates ranges");
    check(probe_mmap(0, 4096, 1, 2, fd, 0) == -22, "private mapping refused");
    check(probe_mmap(0, 4096, 1, 1, fd, 8192) == -22, "mapping past buffer refused");
    child = syscall0(SYS_FORK);
    check(child >= 0, "fork");
    if (!child) {
        c[4099] = 0x66;
        syscall1(SYS_EXIT, 0);
        for (;;) {
        }
    }
    check(syscall3(SYS_WAITPID, child, (long)&status, 0) == child && !status && b[3] == 0x66,
          "fork preserves shared mapping");
    report_held();
}

/** Export, merge, inspect, import and poll completed reservation snapshots. */
static void sync_rows(int fd)
{
    struct dma_buf_export_sync_file data;
    struct dma_buf_sync access;
    struct sync_merge_data merge = {{0}, 0, 0, 0, 0};
    struct sync_file_info info = {{0}, 0, 0, 0, 0, 0};
    volatile struct sync_fence_info row;
    struct {
        int fd;
        short events, revents;
    } pollfd;
    int syncfd;
    data.flags = DMA_BUF_SYNC_RW;
    data.fd = -1;
    check(syscall3(SYS_IOCTL, fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, (long)&data) == 0,
          "export fences");
    syncfd = keep(data.fd);
    check(syscall3(SYS_IOCTL, syncfd, SYNC_IOC_FILE_INFO, (long)&info) == 0 && info.status == 1 &&
              info.num_fences == 1,
          "empty reservation completed");
    info.num_fences = 1;
    row.status = 0;
    info.sync_fence_info = (uintptr_t)&row;
    check(syscall3(SYS_IOCTL, syncfd, SYNC_IOC_FILE_INFO, (long)&info) == 0 && row.status == 1,
          "leaf completion info");
    merge.fd2 = syncfd;
    merge.name[0] = 'm';
    check(syscall3(SYS_IOCTL, syncfd, SYNC_IOC_MERGE, (long)&merge) == 0, "merge sync files");
    keep(merge.fence);
    pollfd.fd = merge.fence;
    pollfd.events = 1;
    pollfd.revents = 0;
    check(syscall3(SYS_POLL, (long)&pollfd, 1, 0) == 1 && pollfd.revents == 1,
          "poll completed merge");
    data.fd = merge.fence;
    data.flags = DMA_BUF_SYNC_WRITE;
    check(syscall3(SYS_IOCTL, fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, (long)&data) == 0,
          "import sync file");
    access.flags = DMA_BUF_SYNC_RW;
    check(syscall3(SYS_IOCTL, fd, DMA_BUF_IOCTL_SYNC, (long)&access) == 0, "CPU access start");
    access.flags |= DMA_BUF_SYNC_END;
    check(syscall3(SYS_IOCTL, fd, DMA_BUF_IOCTL_SYNC, (long)&access) == 0, "CPU access end");
    access.flags = 8;
    check(syscall3(SYS_IOCTL, fd, DMA_BUF_IOCTL_SYNC, (long)&access) == -22, "invalid sync flags");
    report_held();
}

void probe_main(unsigned long *sp);
PROBE_SP_ENTRY;

/** Run only this probe, with all resources released on every exit. */
void probe_main(unsigned long *sp)
{
    int dev, sealed, plain, fd;
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.dmabuf.udmabuf"));
    report_plan("create");
    report_plan("map");
    report_plan("sync");
    report_plan("lifetime");
    dev = keep(syscall3(SYS_OPEN, (long)"/dev/udmabuf", 2, 0));
    sealed = memfd(1);
    plain = memfd(0);
    fd = create_rows(dev, sealed, plain);
    map_rows(dev, sealed, fd);
    sync_rows(fd);
    while (nfds)
        syscall1(SYS_CLOSE, fds[--nfds]);
    check(((unsigned char *)maps[1])[3] == 0x66 && ((unsigned char *)maps[2])[4099] == 0x66,
          "mappings retain pages after all descriptors close");
    report_held();
    cleanup();
    report_end();
    probe_line("UDMABUFPROBE OK", 0, 0);
    syscall1(SYS_EXIT, 0);
    for (;;) {
    }
}
