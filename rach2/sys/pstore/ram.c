/*
 * ram.c: ramoops, the crash records kept in reserved RAM across a reset, as
 * Linux 6.12's fs/pstore/ram.c keeps them, with its module parameters as
 * boot options.
 */
#include "pstore.h"
#include "ram_core.h"

#include "../arch/arch.h"
#include "../boot.h"
#include "../cmdline.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstd.h"
#include "../lib/kstring.h"
#include "../mm/frame.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../panic.h"
#include "../rtc.h"
#include "../timer.h"

/* The header of a dump in its zone, ram.c:28, and its body's first line,
 * platform.c:323 with the one reason this kernel dumps for. */
#define RAMOOPS_KERNMSG_HDR "===="
#define RAMOOPS_PANIC_HDR   "Panic#1 Part1\n"

/* ram.c:29 and the defaults of its parameters, ram.c:31 to 45. */
#define MIN_MEM_SIZE 4096u

/* The options, as ramoops_register_dummy (ram.c:908) reads its module
 * parameters. */
static uint64_t mem_address;
static uint64_t mem_size;
static uint64_t record_size = MIN_MEM_SIZE;
static uint64_t console_size = MIN_MEM_SIZE;
static uint64_t ftrace_size = MIN_MEM_SIZE;
static uint64_t pmsg_size = MIN_MEM_SIZE;
static uint64_t mem_type;
static uint64_t ecc;

static int reserved; /* `ramoops_reserve` took the options */
static int ready;    /* `ramoops_init` laid out the zones */

static struct prz *dprzs; /* the dmesg zones */
static uint32_t max_dump_cnt, dump_write_cnt;
static struct prz cprz; /* the console zone */
static int console_on;  /* the console zone takes the log */
static int dumped;      /* the panic's dump is written */

/**
 * Read the option `ramoops.NAME` into `*value` when the line has it, as
 * kstrtoull reads a module parameter of type ulong or ullong. 0 when it is
 * absent or read, -1, with a record, when its value is not a number.
 */
static int option(const char *name, uint64_t *value)
{
    char key[32], text[24];
    long long v;

    k_strncpy(key, "ramoops.", sizeof(key));
    k_strncpy(key + 8, name, sizeof(key) - 8);
    if (!kcmdline_value(key, text, sizeof(text))) {
        return 0;
    }
    if (text[0] == '-' || k_strtoll(text, sizeof(text), &v) != 0) {
        klog(KLOG_ERR, "ramoops", "refused: %s=%s is not a number", key, text);
        return -1;
    }
    *value = (uint64_t)v;
    return 0;
}

/** `v` rounded down to a power of two, as rounddown_pow_of_two; 0 stays 0. */
static uint64_t pow2_down(uint64_t v)
{
    uint64_t p = 1;

    if (v == 0) {
        return 0;
    }
    while (p <= v / 2) {
        p <<= 1;
    }
    return p;
}

/** Whether a range of type usable in the firmware's map overlaps
 * [start, end). */
static int firmware_usable(const struct boot_info *boot, uint64_t start, uint64_t end)
{
    uint64_t addr, len;
    uint32_t type, i;

    for (i = 0; frame_map_entry(boot->mmap, boot->mmap_len, i, &addr, &len, &type); i++) {
        if (type == FRAME_MAP_USABLE && addr < end && addr + len > start) {
            return 1;
        }
    }
    return 0;
}

/**
 * Read the options and check them as ramoops_probe does (ram.c:741): a
 * size and something to keep in it, each size rounded down to a power of
 * two, and room for the console, ftrace and pmsg zones after the dmesg
 * zones. 0 when they name a region, -1, with a record for a fault, when not.
 */
static int read_options(void)
{
    if (option("mem_address", &mem_address) || option("mem_size", &mem_size) ||
        option("record_size", &record_size) || option("console_size", &console_size) ||
        option("ftrace_size", &ftrace_size) || option("pmsg_size", &pmsg_size) ||
        option("mem_type", &mem_type) || option("ecc", &ecc) || mem_size == 0) {
        return -1;
    }
    if (!record_size && !console_size && !ftrace_size && !pmsg_size) {
        klog(KLOG_ERR, "ramoops", "refused: the record and console sizes are both 0");
        return -1;
    }
    record_size = pow2_down(record_size);
    console_size = pow2_down(console_size);
    ftrace_size = pow2_down(ftrace_size);
    pmsg_size = pow2_down(pmsg_size);
    ecc = ecc == 1 ? 16 : ecc; /* ram.c:944: 1 means 16 bytes of parity */
    if (mem_size > 0xffffffffu) {
        klog(KLOG_ERR, "ramoops", "refused: mem_size is 4 GiB or more");
        return -1;
    }
    if (console_size + ftrace_size + pmsg_size > mem_size) {
        klog(KLOG_ERR, "ramoops", "refused: no room for the console, ftrace and pmsg zones");
        return -1;
    }
    if (ecc > PRZ_ECC_MAX) {
        klog(KLOG_ERR, "ramoops",
             "refused: ecc=%lu: a 128-byte block takes at most %u parity bytes", (unsigned long)ecc,
             PRZ_ECC_MAX);
        return -1;
    }
    if (mem_type > 2) {
        klog(KLOG_ERR, "ramoops", "refused: mem_type=%lu is out of range", (unsigned long)mem_type);
        return -1;
    }
    return 0;
}

/* Linux leaves the region's reservation to whoever passes the address
 * (Documentation/admin-guide/ramoops.rst, "be sure to reserve the memory");
 * here the kernel takes it out of the map itself, as `memmap=` would. */
void ramoops_reserve(struct frame_map *map, const struct boot_info *boot)
{
    uint64_t start, end, frame, usable = 0, frames;

    if (read_options() != 0) {
        return;
    }
    start = mem_address & ~(uint64_t)(PAGE_SIZE - 1);
    end = (mem_address + mem_size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (end < start || end > mmu_phys_reach()) {
        klog(KLOG_ERR, "ramoops", "refused: 0x%lx bytes at 0x%lx lie past what the kernel maps",
             (unsigned long)mem_size, (unsigned long)mem_address);
        return;
    }
    frames = (end - start) / PAGE_SIZE;
    for (frame = start / PAGE_SIZE; frame < end / PAGE_SIZE; frame++) {
        usable += (uint64_t)frame_map_usable(map, frame);
    }
    if (usable == 0 && firmware_usable(boot, start, end)) {
        klog(KLOG_ERR, "ramoops", "refused: [mem 0x%lx-0x%lx] holds the kernel or a module",
             (unsigned long)start, (unsigned long)(end - 1));
        return;
    }
    if (usable != 0 && usable != frames) {
        klog(KLOG_ERR, "ramoops", "refused: [mem 0x%lx-0x%lx] is partly in use",
             (unsigned long)start, (unsigned long)(end - 1));
        return;
    }
    if (usable != 0 && frame_map_reserve(map, start, end) != 0) {
        klog(KLOG_ERR, "ramoops", "refused: [mem 0x%lx-0x%lx]: the memory map is full",
             (unsigned long)start, (unsigned long)(end - 1));
        return;
    }
    klog(KLOG_INFO, "ramoops", "reserving [mem 0x%lx-0x%lx], %s", (unsigned long)start,
         (unsigned long)(end - 1), usable ? "usable memory" : "not memory the kernel manages");
    reserved = 1;
}

/** Read the header ramoops_write_kmsg_hdr wrote at the start of `buf`, of
 * `len` bytes, as ramoops_read_kmsg_hdr (ram.c:149): its time and whether
 * the body is compressed, into `r`. The header's length, or 0 for none. */
static uint32_t read_kmsg_hdr(const char *buf, uint32_t len, struct pstore_record *r)
{
    uint32_t at = 4, digits;
    int64_t s = 0;
    uint32_t us = 0;

    if (len < 6 || buf[0] != '=' || buf[1] != '=' || buf[2] != '=' || buf[3] != '=') {
        return 0;
    }
    for (digits = 0; at < len && buf[at] >= '0' && buf[at] <= '9'; at++, digits++) {
        s = s * 10 + (buf[at] - '0');
    }
    if (digits == 0 || digits > 19 || at >= len || buf[at++] != '.') {
        return 0;
    }
    for (digits = 0; at < len && buf[at] >= '0' && buf[at] <= '9'; at++, digits++) {
        us = us * 10u + (uint32_t)(buf[at] - '0');
    }
    if (digits == 0 || digits > 9 || at >= len) {
        return 0;
    }
    r->compressed = 0;
    if (buf[at] == '-' && at + 2 < len && buf[at + 2] == '\n') {
        r->compressed = buf[at + 1] == 'C';
        at += 2;
    }
    if (buf[at] != '\n') {
        return 0;
    }
    r->sec = s;
    r->nsec = (int32_t)(us < 1000000u ? us * 1000u : 0);
    return at + 1;
}

/** Make the record `r`, whose kind, number and time are set, of what zone
 * `z` held at boot past `skip` bytes of header, with the parity's notice
 * after it, as ramoops_pstore_read does (ram.c:178). */
static void keep(struct prz *z, struct pstore_record *r, uint32_t skip)
{
    uint32_t body = z->old_size - skip, notice = prz_ecc_string(z, 0, 0);
    char *buf = kmalloc(body + notice + 1);

    if (!buf) {
        klog(KLOG_ERR, "ramoops", "refused: no memory for a record of %d bytes", (int)body);
        return;
    }
    k_memcpy(buf, z->old_log + skip, body);
    prz_ecc_string(z, buf + body, notice + 1);
    r->buf = buf;
    r->size = body + notice;
    if (pstore_record_add(r) == 0) {
        klog(KLOG_INFO, "ramoops", "found: %s-ramoops-%d, %d bytes",
             r->type == PSTORE_TYPE_DMESG ? "dmesg" : "console", (int)r->id, (int)r->size);
    }
}

/** The cache bits of Linux's mem_type: 0 write-combining, 1 uncached, 2
 * cached (persistent_ram_vmap, ram_core.c:414). */
static pte_t cache_bits(void)
{
    if (mem_type == 1) {
        return PTE_NOCACHE;
    }
    if (mem_type == 2) {
        return 0;
    }
#ifdef PTE_WC
    return PTE_WC;
#else
    return PTE_NOCACHE;
#endif
}

/** Copy the log so far into the console zone and let it take each record
 * after, with interrupts off between the two, as the pstore console
 * registered with CON_PRINTBUFFER prints the log it missed. */
static void console_start(void)
{
    char chunk[128];
    uint32_t flags = kirq_save(), at = klog_clamp(0), n;

    while ((n = klog_read_from(chunk, sizeof(chunk), &at)) != 0) {
        prz_write(&cprz, chunk, n);
    }
    console_on = 1;
    kirq_restore(flags);
}

/**
 * The layout is ramoops_probe's: the dmesg zones of `record_size` from the
 * region's start, as many as fit in what the console, ftrace and pmsg zones
 * leave, each of the same even size (ram.c:524); then the console zone. The
 * pmsg and ftrace zones keep their place after it, unwritten, so that the
 * first two lie where Linux would put them with the same options.
 */
void ramoops_init(void)
{
    volatile uint8_t *mem;
    uintptr_t va;
    uint64_t dump_mem = 0, zone = 0;
    size_t most;
    uint32_t i;
    struct pstore_record r;
    uint32_t hl;

    if (!reserved) {
        return;
    }
    va = map_mmio_typed((uintptr_t)mem_address, (size_t)mem_size, cache_bits());
    mem = (volatile uint8_t *)va;
    if (!va) {
        klog(KLOG_ERR, "ramoops", "refused: the region does not map");
        return;
    }
    dump_mem = mem_size - console_size - ftrace_size - pmsg_size;
    if (dump_mem != 0 && record_size != 0) {
        max_dump_cnt = record_size > dump_mem
                           ? 0
                           : (uint32_t)tick_div(dump_mem, (uint32_t)record_size, (uint32_t *)0);
        if (max_dump_cnt == 0) {
            klog(KLOG_ERR, "ramoops", "refused: dmesg record count is 0 (%lu / %lu)",
                 (unsigned long)dump_mem, (unsigned long)record_size);
            return;
        }
        zone = tick_div(dump_mem, max_dump_cnt, (uint32_t *)0) & ~(uint64_t)1;
        if (zone <= PRZ_HEADER) {
            klog(KLOG_ERR, "ramoops", "refused: a dmesg zone of %lu bytes holds no data",
                 (unsigned long)zone);
            max_dump_cnt = 0;
            return;
        }
        /* kcalloc's check of the product, ram.c:542: on i686 a small
         * record_size makes enough zones to wrap a size_t. */
        most = (size_t)-1 / sizeof(*dprzs);
        if (max_dump_cnt > most) {
            klog(KLOG_ERR, "ramoops", "refused: %lu dmesg zones are too many to track",
                 (unsigned long)max_dump_cnt);
            max_dump_cnt = 0;
            return;
        }
        dprzs = kzalloc(max_dump_cnt * sizeof(*dprzs));
        if (!dprzs) {
            max_dump_cnt = 0;
            klog(KLOG_ERR, "ramoops", "refused: no memory for the zones");
            return;
        }
        for (i = 0; i < max_dump_cnt; i++) {
            if (prz_init(&dprzs[i], mem + (size_t)(i * zone), (uint32_t)zone, 0, (uint32_t)ecc,
                         0) == -EINVAL) {
                klog(KLOG_ERR, "ramoops", "refused: a dmesg zone of %lu bytes holds no data",
                     (unsigned long)zone);
                kfree(dprzs);
                dprzs = 0;
                max_dump_cnt = 0;
                return;
            }
        }
    }
    if (console_size != 0 && prz_init(&cprz, mem + (size_t)(max_dump_cnt * zone),
                                      (uint32_t)console_size, 0, (uint32_t)ecc, 1) == -EINVAL) {
        klog(KLOG_ERR, "ramoops", "refused: a console zone of %lu bytes holds no data",
             (unsigned long)console_size);
        console_size = 0;
    }
    for (i = 0; i < max_dump_cnt; i++) {
        struct prz *z = &dprzs[i];

        if (!z->old_log) {
            continue;
        }
        k_memset(&r, 0, sizeof(r));
        r.type = PSTORE_TYPE_DMESG;
        r.id = i;
        hl = read_kmsg_hdr(z->old_log, z->old_size, &r);
        if (hl == 0) {
            prz_free_old(z);
            prz_zap(z);
            continue;
        }
        keep(z, &r, hl);
        prz_free_old(z);
    }
    if (cprz.old_log) {
        k_memset(&r, 0, sizeof(r));
        r.type = PSTORE_TYPE_CONSOLE;
        keep(&cprz, &r, 0);
        prz_free_old(&cprz);
    }
    ready = 1;
    k_panic_dump = pstore_panic_dump;
    if (console_size != 0) {
        console_start();
    }
    klog(KLOG_INFO, "ramoops",
         "ready: 0x%lx bytes at 0x%lx, %d dmesg zones of %lu bytes, console %lu bytes, ecc %d",
         (unsigned long)mem_size, (unsigned long)mem_address, (int)max_dump_cnt,
         (unsigned long)zone, (unsigned long)console_size, (int)ecc);
}

int ramoops_ready(void)
{
    return ready;
}

void pstore_console_write(const char *text, uint32_t n)
{
    if (!console_on) {
        return;
    }
    prz_write(&cprz, text, n);
    arch_dma_fence();
}

/** Append the decimal digits of `v`, at least `width` of them, to `buf` at
 * `*at`, without libgcc's 64-bit division. */
static void put_dec(char *buf, uint32_t *at, uint64_t v, int width)
{
    char digits[20];
    int n = 0;
    uint32_t rem;

    do {
        v = tick_div(v, 10, &rem);
        digits[n++] = (char)('0' + rem);
    } while (v != 0 || n < width);
    while (n > 0) {
        buf[(*at)++] = digits[--n];
    }
}

int pstore_panic_dump(void)
{
    char hdr[40], chunk[128];
    struct prz *z;
    int64_t sec;
    int32_t us;
    uint32_t hl = 0, room, off, len, n;

    if (!ready || max_dump_cnt == 0 || dumped) {
        return 0;
    }
    dumped = 1;
    z = &dprzs[dump_write_cnt];
    rtc_realtime_unlocked(&sec, &us);
    /* ram.c:333: "====%lld.%06lu-%c\n" */
    k_memcpy(hdr, RAMOOPS_KERNMSG_HDR, 4);
    hl = 4;
    if (sec < 0) {
        sec = 0;
    }
    put_dec(hdr, &hl, (uint64_t)sec, 1);
    hdr[hl++] = '.';
    put_dec(hdr, &hl, (uint64_t)us, 6);
    hdr[hl++] = '-';
    hdr[hl++] = 'D';
    hdr[hl++] = '\n';
    prz_zap(z);
    prz_write(z, hdr, hl);
    prz_write(z, RAMOOPS_PANIC_HDR, sizeof(RAMOOPS_PANIC_HDR) - 1);

    /* kmsg_dump_get_buffer's newest whole records that fit: from the first
     * record that begins `room` bytes or less before the end. */
    room = hl + sizeof(RAMOOPS_PANIC_HDR) - 1 < z->buffer_size
               ? z->buffer_size - hl - (uint32_t)(sizeof(RAMOOPS_PANIC_HDR) - 1)
               : 0;
    len = klog_len();
    off = len > room ? len - room : 0;
    if (off > 0) {
        char c = 0;

        if (klog_read(&c, 1, off - 1) == 1 && c != '\n') {
            while (klog_read(&c, 1, off) == 1 && c != '\n') {
                off++;
            }
            off++;
        }
    }
    while (off < len && (n = klog_read(chunk, sizeof(chunk), off)) != 0) {
        prz_write(z, chunk, n);
        off += n;
    }
    dump_write_cnt = (dump_write_cnt + 1) % max_dump_cnt;
    arch_dma_fence();
    return 1;
}

void ramoops_erase(int type, uint32_t id)
{
    if (!ready) {
        return;
    }
    if (type == PSTORE_TYPE_DMESG && id < max_dump_cnt) {
        prz_zap(&dprzs[id]);
    } else if (type == PSTORE_TYPE_CONSOLE && console_size != 0) {
        uint32_t flags = kirq_save();

        prz_zap(&cprz);
        kirq_restore(flags);
    }
}
