/*
 * bootctl: manage the boot partition of a machine nobody will open again.
 *
 * The name is systemd's, because that tool manages a boot partition's kernels
 * and defaults on most Linux machines and the knowledge transfers. The verbs
 * are this project's own trial and commit vocabulary from milestone 23.
 *
 *     bootctl status         what is installed, committed, and booting next
 *     bootctl kernel FILE    stage a kernel into the candidate slot
 *     bootctl config FILE    stage a boot menu into the rewritable slot
 *     bootctl grub FILE      write a boot loader into the unused gap slot
 *     bootctl commit         make the booted trial the saved default
 *     bootctl trial ID       boot one entry once, on the next boot only
 *     bootctl cancel         clear an armed trial
 *     bootctl env KEY [VAL]  read, or set, one allowlisted grubenv key
 *
 * `kernel` validates the named file and the machine's fallback kernel, writes
 * `saved_entry=fallback`, writes the candidate slot, reads it back byte for
 * byte, and sets `next_entry=candidate`, so exactly one reboot tries the new
 * kernel and a power cycle after a crash returns to the fallback. Once it has
 * proved itself, `commit` makes it the saved default.
 *
 * `status` is the only window onto this disk. It prints the four files' sector
 * extents, what the environment block has, whether a trial is armed or
 * consumed, and which slot this boot came from. Its extent lines are a test
 * contract: the install suite reads the candidate's first sector from
 * them to corrupt it, so their field order is fixed.
 *
 * `config` writes the boot menu GRUB reads after its own stub entries. That
 * file is GRUB script and whoever writes it can direct the boot, which is the
 * same authority `kernel` already has. What the immutable stub guarantees is
 * that a menu which is damaged, not written leaves a bootable machine:
 * every entry the machine needs is defined before the staged file is read.
 * `env` and `trial` write the two grubenv keys the trial and commit state
 * machine turns on, through an allowlist, not as a general editor,
 * because that block has no checksum and GRUB parses it at boot with no
 * recourse if it is malformed.
 *
 * The tool runs only on a machine that GRUB booted from a slot: it requires
 * a `rach2.slot=` token on /proc/cmdline before installing and
 * `rach2.slot=candidate` before committing, which also covers a QEMU -kernel
 * boot that happens to have a boot-partition disk attached. The device it
 * writes, /dev/bootpart, exists only on such a disk and opens for root
 * alone.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bootslot.h"
#include "bootverb.h"
#include "grubslot.h"

#define SLOT_MAX (4 * 1024 * 1024)

static uint8_t image[SLOT_MAX];
static char errbuf[128];

/* The device fd behind the bootslot_io callbacks. */
static int devfd = -1;

/*
 * The boot partition this run acts on: this machine's own unless `--target`
 * named another.
 *
 * The kernel publishes `/dev/bootpart` as the first partition of the disk it
 * probed first, which from a recovery stick can be the internal disk or the
 * stick itself, so a program repairing a named disk must be able to say which
 * one it means. `grub` and `commit` refuse the option, since both act on the
 * disk this machine booted from.
 */
static const char default_boot_partition[] = "/dev/bootpart";
static const char *boot_partition = default_boot_partition;

static int dev_read(void *ctx, uint32_t lba, void *buf)
{
    (void)ctx;
    if (lseek(devfd, (off_t)lba * BOOTSLOT_SECTOR, SEEK_SET) < 0) {
        return -1;
    }
    return read(devfd, buf, BOOTSLOT_SECTOR) == BOOTSLOT_SECTOR ? 0 : -1;
}

static int dev_write(void *ctx, uint32_t lba, const void *buf)
{
    (void)ctx;
    if (lseek(devfd, (off_t)lba * BOOTSLOT_SECTOR, SEEK_SET) < 0) {
        return -1;
    }
    return write(devfd, buf, BOOTSLOT_SECTOR) == BOOTSLOT_SECTOR ? 0 : -1;
}

static char cmdline[512];
static int cmdline_ready;

/**
 * Read `/proc/cmdline` once; everything that asks about it asks this copy.
 *
 * Once, because a second read can fail where the first succeeded: a process
 * here has sixteen descriptors, and the slot device is open across most of
 * this program. A caller that read that failure as "the token is not there"
 * would install the wrong menu entries on the one machine that could not spare
 * a descriptor.
 *
 * @return 0 when the copy is there to read.
 */
static int cmdline_load(void)
{
    FILE *f;
    size_t n;

    if (cmdline_ready) {
        return 0;
    }
    f = fopen("/proc/cmdline", "r");
    if (!f) {
        return -1;
    }
    n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
    fclose(f);
    cmdline[n] = '\0';
    cmdline_ready = 1;
    return 0;
}

/** Whether /proc/cmdline has `token` as a whole word. */
static int cmdline_has(const char *token)
{
    size_t tlen = strlen(token);
    char *p;

    if (cmdline_load() != 0) {
        return 0;
    }
    for (p = cmdline; *p != '\0'; p++) {
        if ((p == cmdline || p[-1] == ' ') && strncmp(p, token, tlen) == 0 &&
            (p[tlen] == ' ' || p[tlen] == '\n' || p[tlen] == '\0')) {
            return 1;
        }
    }
    return 0;
}


static struct bootslot bs;

static int open_slot(void)
{
    struct bootslot_io io;

    /* Before the device is open, while there is still a descriptor to read it
       with: `booted` reads the line, and an install that could not say which
       entry is running would have nothing to check a commit against. */
    if (cmdline_load() != 0) {
        fprintf(stderr, "bootctl: cannot read /proc/cmdline, so which menu entries this "
                        "machine booted from is unknown: %s\n", strerror(errno));
        return -1;
    }
    devfd = open(boot_partition, O_RDWR);
    if (devfd < 0) {
        if (boot_partition == default_boot_partition) {
            fprintf(stderr, "bootctl: this machine has no boot partition "
                            "(no /dev/bootpart)\n");
        } else {
            fprintf(stderr, "bootctl: cannot open %s: %s\n", boot_partition, strerror(errno));
        }
        return -1;
    }
    io.read = dev_read;
    io.write = dev_write;
    io.ctx = NULL;
    if (bootslot_open(&bs, &io, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: %s: %s\n", boot_partition, errbuf);
        if (boot_partition != default_boot_partition) {
            fprintf(stderr, "bootctl: this machine's own boot partition is %s\n",
                    default_boot_partition);
        }
        return -1;
    }
    return 0;
}

static void print_extent(const char *name, const struct bootslot_file *f, uint32_t cluster_sectors,
                         uint32_t data_start)
{
    uint32_t first = data_start + (f->chain[0] - 2) * cluster_sectors;

    printf("bootctl: %s: first sector %u, %u sectors in %u clusters, "
           "%u bytes\n",
           name, first, f->chain_len * cluster_sectors, f->chain_len, f->size);
}

/**
 * Print one environment value, or a stated absence.
 *
 * The block is 1024 bytes of `key=value` lines with no checksum, so a torn
 * write still parses. `trial_marker` says whether an empty value has the
 * consumed-trial meaning `bootslot_commit` looks for, which is true of
 * `next_entry` and of nothing else: an empty `saved_entry` says only that the
 * key is there with no value.
 */
static void print_env(const uint8_t *env, const char *key, int trial_marker)
{
    char pat[32];
    const char *p = (const char *)env;
    const char *end = p + BOOTSLOT_ENV_BYTES;
    size_t klen;

    pat[0] = '\n';
    strcpy(pat + 1, key);
    strcat(pat, "=");
    klen = strlen(pat);

    while (p < end) {
        if ((size_t)(end - p) >= klen && memcmp(p, pat, klen) == 0) {
            const char *v = p + klen;
            const char *e = v;

            while (e < end && *e != '\n' && *e != '\0') {
                e++;
            }
            if (e == v) {
                if (trial_marker) {
                    printf("bootctl: %s: empty (a trial was consumed)\n", key);
                } else {
                    printf("bootctl: %s: empty\n", key);
                }
            } else {
                printf("bootctl: %s: %.*s\n", key, (int)(e - v), v);
            }
            return;
        }
        p++;
    }
    printf("bootctl: %s: absent\n", key);
}

/**
 * Report the slot this boot came from, which is menu text, not a kernel flag.
 *
 * `recovery` is the third answer: that kernel comes off partition 3 and runs
 * neither slot, which is what makes installing into the candidate safe from
 * there. Committing is still the running candidate's alone.
 */
static void print_booted(void)
{
    if (cmdline_has("rach2.slot=candidate")) {
        printf("bootctl: booted: candidate\n");
    } else if (cmdline_has("rach2.slot=fallback")) {
        printf("bootctl: booted: fallback\n");
    } else if (cmdline_has("rach2.slot=recovery")) {
        printf("bootctl: booted: recovery, from the recovery partition and neither slot\n");
    } else {
        printf("bootctl: booted: not from a slot (no rach2.slot= on /proc/cmdline)\n");
    }
}

static int cmd_status(void)
{
    static uint8_t env[BOOTSLOT_ENV_BYTES];

    if (open_slot() != 0) {
        return 1;
    }
    print_extent("candidate", &bs.kernel, bs.cluster_sectors, bs.data_start);
    print_extent("fallback", &bs.fallback, bs.cluster_sectors, bs.data_start);
    print_extent("grubenv", &bs.grubenv, bs.cluster_sectors, bs.data_start);
    if (bs.have_menu) {
        print_extent("boot.cfg", &bs.menu, bs.cluster_sectors, bs.data_start);
    } else {
        printf("bootctl: boot.cfg: absent (a disk built before the menu slot existed)\n");
    }

    if (bootslot_read_env(&bs, env, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: %s\n", errbuf);
        return 1;
    }
    print_env(env, "saved_entry", 0);
    print_env(env, "next_entry", 1);
    print_booted();
    return 0;
}

static int cmd_commit(void)
{
    if (!cmdline_has("rach2.slot=candidate")) {
        fprintf(stderr, "bootctl: only a machine running the candidate may "
                        "commit it (rach2.slot=candidate is not on "
                        "/proc/cmdline)\n");
        return 1;
    }
    if (open_slot() != 0) {
        return 1;
    }
    if (bootslot_commit(&bs, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: %s\n", errbuf);
        return 1;
    }
    printf("bootctl: committed: the candidate is the saved default\n");
    return 0;
}

static int cmd_kernel(const char *path)
{
    FILE *f;
    size_t size;

    if (!cmdline_has("rach2.slot=candidate") && !cmdline_has("rach2.slot=fallback") &&
        !cmdline_has("rach2.slot=recovery")) {
        fprintf(stderr, "bootctl: this boot did not come from a kernel slot "
                        "(no rach2.slot= on /proc/cmdline)\n");
        return 1;
    }
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "bootctl: cannot open '%s'\n", path);
        return 1;
    }
    size = fread(image, 1, sizeof(image), f);
    if (ferror(f)) {
        fclose(f);
        fprintf(stderr, "bootctl: reading '%s' failed\n", path);
        return 1;
    }
    if (size == sizeof(image) && fgetc(f) != EOF) {
        fclose(f);
        fprintf(stderr, "bootctl: '%s' is larger than any slot\n", path);
        return 1;
    }
    fclose(f);
    if (open_slot() != 0) {
        return 1;
    }
    if (bootslot_install(&bs, image, (uint32_t)size, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: %s\n", errbuf);
        return 1;
    }
    printf("bootctl: installed: %u bytes in the candidate slot; the next boot "
           "tries it once, and a failure falls back\n",
           (unsigned)size);
    printf("bootctl: run `bootctl commit` after the new kernel boots\n");
    return 0;
}

/* The environment keys this tool will write. grubenv is a fixed 1024-byte block
 * with no checksum that GRUB parses at boot with no recourse if it is
 * malformed, and two of its keys are the whole trial and commit state machine,
 * so this is an allowlist and not a general editor. */
static const char *const ENV_KEYS[] = {"saved_entry", "next_entry"};
#define ENV_KEY_COUNT ((int)(sizeof(ENV_KEYS) / sizeof(ENV_KEYS[0])))

/* The menu ids `trial` may arm. Every one of these is an entry the stub both
 * defines and consumes a trial for; a value the stub does not consume would
 * stay in the block forever, arming nothing and blocking every later commit.
 * `recovery` is here now that partition 3 has the kernel and the RAM image it
 * names: arming it is how a machine that still boots is sent to the image that
 * can rewrite its root, which is the only way in when the root is damaged but
 * the machine is not yet stopped. A disk built without those files still
 * defines the entry, and arming it there costs a boot that falls back, not a
 * machine that will not start. */
static const char *const ENTRY_IDS[] = {"candidate", "fallback", "recovery"};

/* What `saved_entry` may hold, which is the same set. The stub reads the saved
 * default for the two kernel entries and for recovery, because a repair is not
 * one boot: rewriting a root takes minutes, and a machine that lost power in
 * the middle of one has to come back to the image that was doing the
 * rewriting, not to a kernel that cannot mount what is there. The kernel debug
 * shell is not in either list: it is reached by adding `rach2.rescue` to a boot
 * line, and a machine saved into it would have no init and no way in. */
static const char *const SAVED_IDS[] = {"candidate", "fallback", "recovery"};
#define ENTRY_ID_COUNT ((int)(sizeof(ENTRY_IDS) / sizeof(ENTRY_IDS[0])))
#define SAVED_ID_COUNT ((int)(sizeof(SAVED_IDS) / sizeof(SAVED_IDS[0])))

static int allowed(const char *const *list, int count, const char *word)
{
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(list[i], word) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Stage a boot menu into the rewritable slot.
 *
 * The file is refused before anything is written if it does not fit the slot,
 * which is the only thing this can check. It is not a completeness test and
 * does not pretend to be one: a transfer cut after a whole menuentry produces a
 * file that parses, and nothing here can tell it from what the sender meant. A
 * caller who needs that compares a digest. What makes a wrong menu survivable
 * is the stub, which defines every entry the machine needs before it reads this
 * file and never lets it choose the default.
 */
static int cmd_config(const char *path)
{
    static uint8_t text[SLOT_MAX];
    FILE *f;
    size_t size;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "bootctl: cannot open '%s'\n", path);
        return 1;
    }
    size = fread(text, 1, sizeof(text), f);
    if (ferror(f)) {
        fclose(f);
        fprintf(stderr, "bootctl: reading '%s' failed\n", path);
        return 1;
    }
    if (size == sizeof(text) && fgetc(f) != EOF) {
        fclose(f);
        fprintf(stderr, "bootctl: '%s' is larger than this tool reads\n", path);
        return 1;
    }
    fclose(f);
    if (size == 0) {
        fprintf(stderr, "bootctl: '%s' is empty\n", path);
        return 1;
    }
    if (open_slot() != 0) {
        return 1;
    }
    if (!bs.have_menu) {
        fprintf(stderr, "bootctl: this disk has no menu slot; it was built before one existed\n");
        return 1;
    }
    if (size > bs.menu.size) {
        fprintf(stderr, "bootctl: '%s' is %u bytes and the menu slot has %u\n", path,
                (unsigned)size, (unsigned)bs.menu.size);
        return 1;
    }
    if (bootslot_write_menu(&bs, text, (uint32_t)size, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: %s\n", errbuf);
        return 1;
    }
    printf("bootctl: staged: %u bytes of menu in a %u-byte slot\n", (unsigned)size,
           (unsigned)bs.menu.size);
    return 0;
}

/** Read one environment key, or set it. */
static int cmd_env(const char *key, const char *value)
{
    static uint8_t env[BOOTSLOT_ENV_BYTES];

    if (!allowed(ENV_KEYS, ENV_KEY_COUNT, key)) {
        fprintf(stderr, "bootctl: not a key this tool writes: '%s'\n", key);
        return 1;
    }
    if (value) {
        size_t i;

        /* The block is lines of `key=value`, so a value carrying a newline
           would write a second assignment nobody asked for. */
        for (i = 0; value[i] != '\0'; i++) {
            if (value[i] < 0x20 || value[i] == 0x7f) {
                fprintf(stderr, "bootctl: a value may not carry control characters\n");
                return 1;
            }
        }
        /* Both keys name a menu entry, and the same restriction `trial` puts on
           the entry it arms has to hold here or it is not a restriction. An
           empty value is refused outright: for `next_entry` that is the marker
           a trial boot leaves behind and `commit` requires, so a tool that let
           anyone write it would let a kernel that never ran be committed. */
        if (value[0] == '\0') {
            fprintf(stderr, "bootctl: an empty value is the marker a trial boot leaves; "
                            "use `bootctl cancel` to clear a trial\n");
            return 1;
        }
        if (!allowed(ENTRY_IDS, ENTRY_ID_COUNT, value)) {
            fprintf(stderr, "bootctl: not a menu entry this tool arms: '%s'\n", value);
            return 1;
        }
        /* The stub reads the saved default for the entries it defines, so
           saving anything else would report a default the machine ignores. */
        if (strcmp(key, "saved_entry") == 0 && !allowed(SAVED_IDS, SAVED_ID_COUNT, value)) {
            fprintf(stderr, "bootctl: '%s' is a boot and not a default; arm it for one "
                            "boot with `bootctl trial %s`\n", value, value);
            return 1;
        }
    }
    if (open_slot() != 0) {
        return 1;
    }
    if (!value) {
        if (bootslot_read_env(&bs, env, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "bootctl: %s\n", errbuf);
            return 1;
        }
        print_env(env, key, strcmp(key, "next_entry") == 0);
        return 0;
    }
    if (bootslot_env_set(&bs, key, value, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: %s\n", errbuf);
        return 1;
    }
    printf("bootctl: %s: %s\n", key, value);
    return 0;
}

/** Arm one boot of `id`, and only of an entry the stub defines. */
static int cmd_trial(const char *id)
{
    if (!allowed(ENTRY_IDS, ENTRY_ID_COUNT, id)) {
        fprintf(stderr, "bootctl: not a menu entry this disk has: '%s'\n", id);
        return 1;
    }
    if (open_slot() != 0) {
        return 1;
    }
    if (bootslot_env_set(&bs, "next_entry", id, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: %s\n", errbuf);
        return 1;
    }
    printf("bootctl: armed: the next boot tries %s once\n", id);
    return 0;
}

/**
 * Make the committed candidate the fallback kernel too.
 *
 * The fallback file is written only by an install that builds the disk,
 * so a machine that has been updated through the candidate slot for a long
 * time keeps a fallback as old as its disk. This copies the slot into the
 * fallback file and reads it back, refused while the candidate is not the
 * saved default or a trial is armed; `bootctl trial fallback` then boots
 * the new fallback once before it is relied on.
 */
static int cmd_fallback(void)
{
    if (open_slot() != 0) {
        return 1;
    }
    if (bootslot_fallback(&bs, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: %s\n", errbuf);
        return 1;
    }
    printf("bootctl: fallback: %u bytes written from the committed candidate and read back\n",
           (unsigned)bs.kernel.size);
    printf("bootctl: run `bootctl trial fallback` to boot it once\n");
    return 0;
}

/**
 * Clear an armed trial, so the next boot takes the saved default.
 *
 * The key is removed and not set empty. An empty `next_entry` is the marker
 * GRUB leaves behind after a trial has actually booted, and `commit` requires
 * it, so writing it here would let a kernel that never ran be made the default.
 */
static int cmd_cancel(void)
{
    if (open_slot() != 0) {
        return 1;
    }
    if (bootslot_env_drop(&bs, "next_entry", errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: %s\n", errbuf);
        return 1;
    }
    printf("bootctl: cancelled: the next boot takes the saved default\n");
    return 0;
}

/* The gap device's window starts at LBA 1, so a gap LBA is this many sectors
 * into it. Sector 0 is outside the window on purpose: it has the partition
 * table, and a disk writes whole sectors. */
#define GAP_OFF(lba) (((off_t)((lba) - 1u)) * BOOTSLOT_SECTOR)

static uint8_t core[GRUBSLOT_MAX_IMAGE_SECTORS * BOOTSLOT_SECTOR];
static uint8_t back[BOOTSLOT_SECTOR];

/** Read or write one whole sector of /dev/grubcore at a gap LBA. */
static int gap_io(int fd, uint32_t lba, uint8_t *buf, int writing)
{
    if (lseek(fd, GAP_OFF(lba), SEEK_SET) < 0) {
        return -1;
    }
    if (writing) {
        return write(fd, buf, BOOTSLOT_SECTOR) == BOOTSLOT_SECTOR ? 0 : -1;
    }
    return read(fd, buf, BOOTSLOT_SECTOR) == BOOTSLOT_SECTOR ? 0 : -1;
}

/**
 * Read or write one gap sector through an open `/dev/grubcore`.
 *
 * The window's offsets are the disk's own LBAs, so this is `gap_io` with the
 * signature `grubslot_install` calls it through.
 */
static int grubcore_io(void *ctx, uint32_t lba, uint8_t *buf, int write)
{
    return gap_io(*(int *)ctx, lba, buf, write);
}

/**
 * Replace GRUB's core image, writing the tail that is not in use.
 *
 * The order is the whole safety argument. The image goes into the inactive
 * tail, the whole tail is read back and compared, and only then is LBA 1
 * rewritten to point at it. A failure anywhere before that last sector leaves
 * the old entry sector and the old tail selected, and the machine boots as it
 * did. Sector 0 is never written, so the partition table is never at risk.
 *
 * What remains is one sector. A torn or bad write to LBA 1 makes GRUB
 * unreachable, and recovery cannot help because GRUB is what loads it. That is
 * stated, not defended against: the exposure is a single sector written
 * after the replacement has been verified, and removing it would take something
 * below GRUB that this machine does not have.
 */
static int cmd_grub(const char *path)
{
    FILE *f;
    size_t size;
    uint32_t sectors, target;
    int fd, rc;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "bootctl: cannot open '%s'\n", path);
        return 1;
    }
    size = fread(core, 1, sizeof(core), f);
    if (ferror(f)) {
        fclose(f);
        fprintf(stderr, "bootctl: reading '%s' failed\n", path);
        return 1;
    }
    if (size == sizeof(core) && fgetc(f) != EOF) {
        fclose(f);
        fprintf(stderr, "bootctl: '%s' does not fit a tail slot\n", path);
        return 1;
    }
    fclose(f);
    if (grubslot_prepare(core, (uint32_t)sizeof(core), (uint32_t)size, &sectors, errbuf,
                         sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: '%s': %s\n", path, errbuf);
        return 1;
    }

    fd = open("/dev/grubcore", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "bootctl: this machine has no boot loader window (no /dev/grubcore)\n");
        return 1;
    }
    rc = grubslot_install(&fd, grubcore_io, core, sectors, back, &target, errbuf,
                          sizeof(errbuf));
    close(fd);
    if (rc != 0) {
        fprintf(stderr, "bootctl: %s\n", errbuf);
        return 1;
    }
    printf("bootctl: installed: %u sectors of boot loader in the tail at LBA %u; "
           "the entry sector now points there\n",
           (unsigned)sectors, (unsigned)target);
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: bootctl status\n");
    fprintf(stderr, "       bootctl kernel <file>\n");
    fprintf(stderr, "       bootctl config <file>\n");
    fprintf(stderr, "       bootctl grub <file>\n");
    fprintf(stderr, "       bootctl commit\n");
    fprintf(stderr, "       bootctl trial <entry>\n");
    fprintf(stderr, "       bootctl cancel\n");
    fprintf(stderr, "       bootctl fallback\n");
    fprintf(stderr, "       bootctl env <key> [value]\n");
    fprintf(stderr, "       bootctl --target <partition> <command> ...\n");
}

/** Resolve the command line and run the verb it names. */
int main(int argc, char **argv)
{
    const char *arg;
    const char *arg2;

    if (argc >= 3 && strcmp(argv[1], "--target") == 0) {
        boot_partition = argv[2];
        argv[2] = argv[0];
        argv += 2;
        argc -= 2;
        /* Two commands mean something about the machine that is running and
         * not about the partition named: `grub` writes the boot loader through
         * `/dev/grubcore`, which is the booted disk's gap and not the target's,
         * and `commit` accepts the running kernel, which came from the booted
         * disk. Either would act on one disk while reporting the other. */
        if (argc >= 2 && (strcmp(argv[1], "grub") == 0 || strcmp(argv[1], "commit") == 0)) {
            fprintf(stderr, "bootctl: --target does not take '%s': it acts on the disk this "
                            "machine booted from\n",
                    argv[1]);
            return 1;
        }
    }

    switch (bootverb_parse(argc, argv, &arg, &arg2)) {
    case BOOTVERB_STATUS:
        return cmd_status();
    case BOOTVERB_COMMIT:
        return cmd_commit();
    case BOOTVERB_CANCEL:
        return cmd_cancel();
    case BOOTVERB_FALLBACK:
        return cmd_fallback();
    case BOOTVERB_KERNEL:
        return cmd_kernel(arg);
    case BOOTVERB_CONFIG:
        return cmd_config(arg);
    case BOOTVERB_GRUB:
        return cmd_grub(arg);
    case BOOTVERB_TRIAL:
        return cmd_trial(arg);
    case BOOTVERB_ENV:
        return cmd_env(arg, arg2);
    default:
        break;
    }

    if (argc >= 2 && !bootverb_known(argv[1])) {
        fprintf(stderr, "bootctl: not a command this tool takes: '%s'\n", argv[1]);
    }
    usage();
    return 1;
}
