/*
 * pstore.h: the records a crash leaves for the next boot, as Linux's pstore
 * keeps them: ramoops writes them to reserved RAM, and the next boot shows
 * them as files under /sys/fs/pstore.
 */
#ifndef _PSTORE_H_
#define _PSTORE_H_

#include "../lib/kint.h"

struct boot_info;
struct frame_map;
struct vfs_type;

/* The kinds of record, as Linux's enum pstore_type_id numbers the two
 * ramoops keeps here: a crash's dump of the log, and the last boot's log as
 * the console printed it. */
#define PSTORE_TYPE_DMESG   0
#define PSTORE_TYPE_CONSOLE 2

/**
 * Read the `ramoops.` options and take their region out of the frame
 * allocator's map, before `init_paging` carves the map: the whole region
 * when it is usable memory, nothing when the firmware's map in `boot` has
 * no usable memory there, as for a device's memory; the options are
 * refused, with a record, when the region is partly usable or overlaps what
 * the boot already took. Under the big lock, once.
 */
void ramoops_reserve(struct frame_map *map, const struct boot_info *boot);

/**
 * Map the region `ramoops_reserve` accepted, lay out its zones, keep what
 * each held from the boot before as a record, and start the console zone
 * with the log so far. Needs paging and the heap. Under the big lock, once.
 */
void ramoops_init(void);

/** Whether ramoops runs, so that /sys/fs/pstore is made. */
int ramoops_ready(void);

/**
 * Append the `n` bytes of a record of the log to the console zone, as
 * Linux's pstore console does for each line it prints. From any context,
 * with interrupts off, by the one writer the log's ring has at a time; a
 * no-op until `ramoops_init` has run.
 */
void pstore_console_write(const char *text, uint32_t n);

/** Whether the shared pstore dump threshold permits a first ramoops record. */
int pstore_dump_enabled(void);

/**
 * Write the tail of the log, as many of its newest whole records as fit,
 * into the next dmesg zone with Linux's headers, as kmsg_dump does with
 * KMSG_DUMP_PANIC (kernel/panic.c:440) through pstore_dump and ramoops.
 * From `k_panic` through `k_panic_dump`, which `ramoops_init` sets, after
 * the panic's record is in the log: it allocates nothing, takes no lock and
 * waits for nothing. Returns 1 when it wrote the record, 0 when there is no
 * region, no dmesg zone or a record was already written.
 */
int pstore_panic_dump(void);

/**
 * Erase record `id` of `type` from its zone, for an unlink of its file, as
 * ramoops_pstore_erase: the zone emptied, and the console zone too, which
 * then holds this boot's log, as on Linux. Under the big lock.
 */
void ramoops_erase(int type, uint32_t id);

/* A record, as Linux's struct pstore_record holds one: its kind, its
 * number among those of its kind, the time it was written, 0 when unknown,
 * whether its body is compressed, and the body. */
struct pstore_record {
    int type;
    uint32_t id;
    int64_t sec;
    int32_t nsec;
    int compressed;
    char *buf; /* kmalloc'd */
    uint32_t size;
};

/**
 * Add the record `r` to the file system, which takes its `buf`: a file
 * named `TYPE-ramoops-ID`, and `.enc.z` after it when compressed, as
 * pstore_mkfile names it (inode.c:362). 0, or -ENOMEM, which frees `buf`.
 */
int pstore_record_add(const struct pstore_record *r);

/**
 * Make /sys/fs/pstore and mount the records there, once the root and /sys
 * are mounted and when ramoops runs, as systemd mounts pstore on Linux.
 */
void pstore_start(void);

/** The file system, which mount(2) also takes as `pstore`. */
extern const struct vfs_type vfs_pstore_type;

#endif
