/*
 * selfbuild_scenario.c: the scenario of machine.selfbuild and
 * machine.selfbuild-ext2, the machine building itself with the staged
 * TinyCC and pdpmake over the staged guest.mk files. Case `kernel` builds the
 * kernel from /src/rach2 as the test account, takes the bytes it built off
 * the disk, checks them on the host and boots them through GRUB: to a login
 * prompt, and to the debug shell of the test image in RAM, where every probe
 * of the default suite's PROBES table runs. Case `userland` builds the
 * utilities and the shell and runs what it built, and case `inplace` has
 * root build the staged /src/ish where it is. The ext2 family does the same
 * on an ext2 root, with one build of the kernel and the ten probes of an
 * installed root run on an ext2 root of the test manifest.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootslot.h"
#include "lifecycle.h"

/* The root's partition on the ext2 disk: 128 MiB at LBA 2048. */
#define PART_OFFSET 1048576L
#define PART_SIZE   134217728L

/* The root's partition on the KRFFS disk, which suites.mk makes with
 * `128 112`: 112 MiB at LBA 2048, larger than the LC_PART_SIZE of os-real's
 * other disks. */
#define KRFFS_PART_SIZE "117440512"

/* The free bytes a build needs on the root: the copied source tree, the
 * objects, the response file and the kernel set aside, with margin. The
 * x86-64 build with ACPICA takes about 39 MiB and its kernel 3.1 MB. */
#define NEED_BYTES 50331648L

/* The least a kernel can be: the tcc-linked kernel is about 1.7 MB on i686
 * and 2.0 MB on x86-64, and a smaller file is a build that stopped short. */
#define KERNEL_FLOOR 200000L

/* The size of a kernel slot of the boot partition of mkdisk_grub.sh, to
 * which the kernel is padded: BOOTSLOT_SLOT_BYTES, the slot's one home. */
#define SLOT_BYTES ((long)BOOTSLOT_SLOT_BYTES)

/* The most probes the counter witness `probe` counts in one attempt. */
#define PROBE_LIMIT 256

/* Where the test account builds, and the kernel it sets aside there. */
#define KEPT "/home/test/kernel.selfbuilt"

/* The record that hands the debug shell back, and the shell's greeting. */
static const char back[] = ", returning to kernel shell";
static const char greeting[] = "Type 'help'";

/* The probes that tests/Readme.md lists for an installed root, ended by NULL,
 * run on an ext2 root of the test manifest. */
static const char *const root_probes[] = {
    "shelltest", "limitprobe", "dirprobe",    "unlinkdirprobe", "mvtest", "vfsprobe",
    "dnstest",   "devprobe",   "orphanprobe", "modeprobe",      NULL,
};

/*
 * The self-built server, started with its standard output on a pipe to a
 * subshell that prints the server's first line, so that the request is sent
 * once the server says it listens; a cat then takes the rest of its log.
 * Port 8080 and not 80, since an ordinary user may not have a port below
 * 1024.
 */
static const char httpd_up[] =
    "./httpd 8080 2>&1 | (read -r up; echo \"UP_\"\"$up\"; echo UP_\"\"END; exec busybox cat > "
    "/tmp/h.log) &\r";
static const char httpd_get[] =
    "printf 'GET / HTTP/1.0\\r\\n\\r\\n' | ./nc 127.0.0.1 8080 | busybox grep -q ' 200 ' && "
    "echo SMOKE_\"\"WEB; kill %%; echo WEB_\"\"END\r";

/* A command the userland case types and the token it prints when its check
 * holds; each command prints `end` last, whatever it found. */
struct smoke {
    const char *id;
    const char *line;
    const char *token;
    const char *end;
    const char *why;
};

/*
 * The userland checks, each over a program the machine built, run from
 * /home/test/kbuild/utils. A refusal must exit 1, so that an exec failure
 * (126 or 127) or a crash (128 and a signal) cannot pass as one.
 */
static const struct smoke smokes[] = {
    {"smoke-cat",
     "cd /home/test/kbuild/utils && busybox echo hi > /tmp/sf && [ \"$(./cat /tmp/sf)\" = hi ] && "
     "echo SMOKE_\"\"CAT; echo CAT_\"\"END\r",
     "SMOKE_CAT", "CAT_END", "the cat the machine built does not copy a file"},
    {"smoke-echo", "[ \"$(./echo alpha)\" = alpha ] && echo SMOKE_\"\"ECHO; echo ECHO_\"\"END\r",
     "SMOKE_ECHO", "ECHO_END", "the echo the machine built does not print its argument"},
    {"smoke-true-false", "./true && ! ./false && echo SMOKE_\"\"TF; echo TF_\"\"END\r", "SMOKE_TF",
     "TF_END", "the true and false the machine built do not exit 0 and 1"},
    {"smoke-pwd",
     "[ \"$(cd /tmp && /home/test/kbuild/utils/pwd)\" = /tmp ] && echo SMOKE_\"\"PWD; "
     "echo PWD_\"\"END\r",
     "SMOKE_PWD", "PWD_END", "the pwd the machine built does not name /tmp in /tmp"},
    {"smoke-ls", "./ls /tmp | busybox grep -q sf && echo SMOKE_\"\"LS; echo LS_\"\"END\r",
     "SMOKE_LS", "LS_END", "the ls the machine built does not list /tmp/sf"},
    {"smoke-touch-mkdir",
     "./touch /tmp/st && [ -f /tmp/st ] && ./mkdir /tmp/sd && [ -d /tmp/sd ] && "
     "echo SMOKE_\"\"TM; echo TM_\"\"END\r",
     "SMOKE_TM", "TM_END", "the touch or the mkdir the machine built made nothing"},
    {"smoke-cp-mv-rm",
     "./cp /tmp/sf /tmp/sc && busybox cmp -s /tmp/sf /tmp/sc && ./mv /tmp/sc /tmp/sm && "
     "[ -f /tmp/sm ] && ./rm /tmp/sm /tmp/st && [ ! -f /tmp/sm ] && echo SMOKE_\"\"MCR; "
     "echo MCR_\"\"END\r",
     "SMOKE_MCR", "MCR_END", "the cp, mv or rm the machine built did not do its work"},
    {"smoke-yes",
     "./yes | busybox head -n 1 | busybox grep -qx y && echo SMOKE_\"\"YES; "
     "echo YES_\"\"END\r",
     "SMOKE_YES", "YES_END", "the yes the machine built does not print y"},
    {"smoke-ping",
     "./ping -c 1 127.0.0.1 > /dev/null 2>&1 && echo SMOKE_\"\"PING; "
     "echo PING_\"\"END\r",
     "SMOKE_PING", "PING_END", "the ping the machine built has no answer from 127.0.0.1"},
    {"smoke-httpd", httpd_get, "SMOKE_WEB", "WEB_END",
     "the httpd the machine built did not answer GET / with 200 on 127.0.0.1:8080"},
    {"smoke-built",
     "[ -x halt ] && [ -x init-shell ] && [ -x init-login ] && [ -x adduser-crash ] && "
     "echo SMOKE_\"\"BUILT; echo BUILT_\"\"END\r",
     "SMOKE_BUILT", "BUILT_END",
     "the build left out halt, init-shell, init-login or adduser-crash"},
    {"smoke-login",
     "./login < /dev/null > /dev/null 2>&1; [ $? -eq 1 ] && echo SMOKE_\"\"LOGIN; "
     "echo LOGIN_\"\"END\r",
     "SMOKE_LOGIN", "LOGIN_END", "the login the machine built does not refuse with status 1"},
    {"smoke-su",
     "./su -c true < /dev/null > /dev/null 2>&1; [ $? -eq 1 ] && echo SMOKE_\"\"SU; "
     "echo SU_\"\"END\r",
     "SMOKE_SU", "SU_END", "the su the machine built does not refuse with status 1"},
    {"smoke-sudo",
     "./sudo true > /dev/null 2>&1; [ $? -eq 1 ] && echo SMOKE_\"\"SUDO; "
     "echo SUDO_\"\"END\r",
     "SMOKE_SUDO", "SUDO_END", "the sudo the machine built does not refuse with status 1"},
    {"smoke-passwd",
     "./passwd < /dev/null > /dev/null 2>&1; [ $? -eq 1 ] && echo SMOKE_\"\"PASSWD; "
     "echo PASSWD_\"\"END\r",
     "SMOKE_PASSWD", "PASSWD_END", "the passwd the machine built does not refuse with status 1"},
    {"smoke-adduser",
     "./adduser > /dev/null 2>&1; [ $? -eq 1 ] && echo SMOKE_\"\"ADD; "
     "echo ADD_\"\"END\r",
     "SMOKE_ADD", "ADD_END", "the adduser the machine built does not refuse with status 1"},
    {"smoke-bootctl",
     "./bootctl kernel ./cat > /dev/null 2>&1; [ $? -eq 1 ] && echo SMOKE_\"\"INSTK; "
     "echo INSTK_\"\"END\r",
     "SMOKE_INSTK", "INSTK_END", "the bootctl the machine built does not refuse with status 1"},
    {"smoke-ish",
     "cd /home/test/kbuild/ish && [ \"$(./ish -c '/home/test/kbuild/utils/echo selfbuilt')\" = "
     "selfbuilt ] && echo SMOKE_\"\"ISH; echo ISH_\"\"END\r",
     "SMOKE_ISH", "ISH_END", "the ish the machine built does not run a command given with -c"},
};

/* The kernel case's commands, in the order typed. Each ends by printing its
 * end marker whatever it found. */
static const char copy_one[] =
    "rm -rf /home/test/kbuild; mkdir /home/test/kbuild && cd /home/test/kbuild && "
    "cp -r /src/rach2 . && echo COPY_\"\"OK; echo COPY_\"\"END\r";
static const char copy_two[] = "cd /home/test/kbuild && rm -rf rach2 && cp -r /src/rach2 . && "
                               "echo COPY_\"\"OK; echo COPY_\"\"END\r";
static const char build_one[] =
    "cd /home/test/kbuild/rach2 && make -f guest.mk > /tmp/kb1.out 2>&1; echo BUILD_\"\"RC=$?; "
    "busybox tail -4 /tmp/kb1.out; echo BUILD_\"\"END\r";
static const char build_two[] =
    "cd /home/test/kbuild/rach2 && make -f guest.mk > /tmp/kb2.out 2>&1; echo BUILD_\"\"RC=$?; "
    "busybox tail -4 /tmp/kb2.out; echo BUILD_\"\"END\r";
static const char hash_one[] =
    "K1=$(busybox sha256sum kernel | busybox cut -c1-64) && cp kernel " KEPT " && "
    "echo HASH_\"\"ONE=$K1 SIZE_\"\"ONE=$(busybox stat -c %s kernel); echo HASH_\"\"END\r";
static const char hash_two[] =
    "K2=$(busybox sha256sum kernel | busybox cut -c1-64); [ -n \"$K1\" ] && [ \"$K1\" = \"$K2\" ] "
    "&& echo HASH_\"\"SAME; echo HASH_\"\"TWO=$K2; echo SAME_\"\"END\r";
static const char space[] =
    "busybox du -sk /home/test/kbuild /tmp " KEPT "; busybox df -k /home/test; echo DU_\"\"END\r";
static const char sync_line[] = "sync && echo SYNC_\"\"OK; echo SYNC_\"\"END\r";

/* The userland case's copies and builds. */
static const char copy_user[] =
    "rm -rf /home/test/kbuild; mkdir /home/test/kbuild && cd /home/test/kbuild && "
    "cp -r /src/utils . && cp -r /src/ish . && echo COPY_\"\"OK; echo COPY_\"\"END\r";
static const char build_utils[] =
    "cd /home/test/kbuild/utils && make -f guest.mk > /tmp/ub.out 2>&1; echo BUILD_\"\"RC=$?; "
    "busybox tail -4 /tmp/ub.out; echo BUILD_\"\"END\r";
static const char build_ish[] =
    "cd /home/test/kbuild/ish && make -f guest.mk > /tmp/ib.out 2>&1; echo BUILD_\"\"RC=$?; "
    "busybox tail -4 /tmp/ib.out; echo BUILD_\"\"END\r";

/* The in-place build of the staged shell, as root, with no copy and no
 * touch; staged files with zero times would fail it, since pdpmake reads a
 * zero modification time as a file that does not exist. */
static const char build_inplace[] =
    "cd /src/ish && make -f guest.mk > /tmp/ip.out 2>&1; echo BUILD_\"\"RC=$?; "
    "busybox tail -3 /tmp/ip.out; echo BUILD_\"\"END\r";

/* One attempt, and what the kernel case learns on the way. */
struct sb {
    struct lc l;
    int ext2;            /* the case is of machine.selfbuild-ext2 */
    const char *disk;    /* the artifact of the root's disk */
    char hash[65];       /* the kernel's SHA-256, as the guest printed it */
    char image[1024];    /* the private copy of the root's disk */
    char root[1024];     /* the ext2 root's partition, copied to the evidence */
    char kernel[1024];   /* the kernel taken off the disk, in the evidence */
    unsigned builds;     /* the units of the witness `build` */
    unsigned probes_run; /* the units of the witness `probe` */
    unsigned port;       /* the clock reference's port, or 0 */
};

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Copy the path of evidence `name` into `buf` of `size` bytes, since the
 * library's buffer is reused. @return `buf`, or NULL after halting.
 */
static char *evidence(struct sb *s, const char *name, char *buf, size_t size)
{
    const char *path = etude_evidence(s->l.c, name);

    if (path == NULL || strlen(path) >= size) {
        lc_halt(&s->l, ETUDE_FAILED, "the path of the evidence");
        return NULL;
    }
    snprintf(buf, size, "%s", path);
    return buf;
}

/**
 * Give the bytes of the file at `path`, NUL-ended, from malloc, with their
 * count in `*len` when it is not NULL; NULL when the file cannot be read.
 */
static char *slurp(const char *path, long *len)
{
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    char *buf = NULL;
    long n = -1;

    if (f != NULL && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 &&
        fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)n + 1)) != NULL &&
        fread(buf, 1, (size_t)n, f) == (size_t)n) {
        buf[n] = '\0';
    } else {
        free(buf);
        buf = NULL;
    }
    if (f != NULL)
        fclose(f);
    if (len != NULL)
        *len = buf != NULL ? n : -1;
    return buf;
}

/** Say whether the files at `a` and `b` have the same bytes. */
static int same_file(const char *a, const char *b)
{
    long na, nb;
    char *x = slurp(a, &na), *y = slurp(b, &nb);
    int same = x != NULL && y != NULL && na == nb && memcmp(x, y, (size_t)na) == 0;

    free(x);
    free(y);
    return same;
}

/**
 * Copy `size` bytes at `offset` of the file at `from` to a new file at `to`.
 * @return 1 when every byte was copied.
 */
static int copy_range(const char *from, const char *to, long offset, long size)
{
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    char buf[65536];
    size_t n;
    long left = size;

    if (in != NULL && out != NULL && fseek(in, offset, SEEK_SET) == 0)
        while (left > 0 &&
               (n = fread(buf, 1, left < (long)sizeof(buf) ? (size_t)left : sizeof(buf), in)) > 0 &&
               fwrite(buf, 1, n, out) == n)
            left -= (long)n;
    if (in != NULL)
        fclose(in);
    if (out != NULL && fclose(out) != 0)
        left = 1;
    return in != NULL && out != NULL && left == 0;
}

/**
 * Give the byte offset of entry `i`, counted from 0, of the MBR of the disk
 * at `path`, or -1 when the sector is no MBR or the entry is empty.
 */
static long partition_offset(const char *path, int i)
{
    FILE *f = fopen(path, "rb");
    unsigned char mbr[512], *e = mbr + 446 + 16 * i;
    unsigned long lba;

    if (f == NULL)
        return -1;
    if (fread(mbr, 1, sizeof(mbr), f) != sizeof(mbr) || mbr[510] != 0x55 || mbr[511] != 0xaa) {
        fclose(f);
        return -1;
    }
    fclose(f);
    lba = (unsigned long)e[8] | (unsigned long)e[9] << 8 | (unsigned long)e[10] << 16 |
          (unsigned long)e[11] << 24;
    return lba != 0 && e[4] != 0 ? (long)lba * 512 : -1;
}

/**
 * Run host tool or artifact `argv[0]`, keeping its standard output as
 * evidence `output`. @return 1 when it exited 0, 0 when it ended otherwise,
 * and 0 after halting when it could not be run.
 */
static int host(struct sb *s, char *const argv[], const char *output)
{
    struct etude_termination end;
    enum etude_status st;
    char what[96];

    if (!lc_ok(&s->l))
        return 0;
    st = etude_host(s->l.c, argv, output, &end);
    if (st != ETUDE_OK) {
        snprintf(what, sizeof(what), "the run of %s", argv[0]);
        lc_halt(&s->l, st, what);
        return 0;
    }
    return end.end == ETUDE_END_EXITED && end.value == 0;
}

/**
 * Read the first line of artifact `name`, a password, into `buf` of `size`
 * bytes followed by a carriage return, as it is typed. @return 1 when it
 * was read.
 */
static int password(struct sb *s, const char *name, char *buf, size_t size)
{
    const char *path = etude_artifact(s->l.c, name);
    FILE *f = path != NULL ? fopen(path, "r") : NULL;
    size_t n;

    if (f == NULL || fgets(buf, (int)size - 1, f) == NULL || (n = strcspn(buf, "\r\n")) == 0) {
        if (f != NULL)
            fclose(f);
        lc_halt(&s->l, ETUDE_FAILED, "the read of the password");
        return 0;
    }
    fclose(f);
    buf[n] = '\r';
    buf[n + 1] = '\0';
    return 1;
}

/**
 * Type `line` at the shell of machine `machine`, `m`, and wait for `end`,
 * which the line prints last, whatever it found.
 *
 * @return the console from the typing to `end`, NUL-ended, which the caller
 *         frees; or NULL when the scenario cannot go on.
 */
static char *command(struct sb *s, struct etude_case_machine *m, const char *machine, uint64_t *at,
                     const char *line, const char *end)
{
    uint64_t from = *at;

    if (!lc_type(&s->l, m, line) || !lc_wait(&s->l, m, end, at, NULL))
        return NULL;
    return lc_console(&s->l, machine, from, *at);
}

/**
 * Copy into `buf` of `size` bytes the word after the first `key` of `w`, up
 * to a space or the end of its line. @return `buf`, empty when there is none.
 */
static char *word_after(const char *w, const char *key, char *buf, size_t size)
{
    const char *p = w != NULL ? strstr(w, key) : NULL;
    size_t n = 0;

    if (p != NULL)
        for (p += strlen(key); n + 1 < size && p[n] != '\0' && strchr(" \r\n", p[n]) == NULL; n++)
            buf[n] = p[n];
    buf[n] = '\0';
    return buf;
}

/**
 * Boot the root's machine `build`, report assertion `login` from a login at
 * its console as `user`, whose password is artifact `secret`, and give the
 * machine with the console read to the shell's `prompt` in `*at`; NULL when
 * the scenario cannot go on.
 */
static struct etude_case_machine *login(struct sb *s, const char *user, const char *secret,
                                        const char *prompt, uint64_t *at)
{
    struct etude_case_machine *m;
    char name[64], typed[256];

    *at = 0;
    if (!lc_step(&s->l, "boot") || (m = lc_boot(&s->l, "build")) == NULL ||
        !lc_wait(&s->l, m, "login:", at, NULL))
        return NULL;
    snprintf(name, sizeof(name), "%s\r", user);
    if (!lc_step(&s->l, "login") || !lc_type(&s->l, m, name) ||
        !lc_wait(&s->l, m, "Password:", at, NULL) || !password(s, secret, typed, sizeof(typed)) ||
        !lc_type(&s->l, m, typed) || !lc_wait(&s->l, m, prompt, at, "login"))
        return NULL;
    return m;
}

/**
 * Run build command `line` under the quiet witness `build`, which it
 * advances by one, and report assertion `id`: the build exited 0.
 * @return 1 when it did.
 */
static int build(struct sb *s, struct etude_case_machine *m, uint64_t *at, const char *line,
                 const char *id, const char *why)
{
    char *w;
    int held;

    if (!lc_ok(&s->l))
        return 0;
    etude_progress(s->l.c, "build", s->builds);
    if ((w = command(s, m, "build", at, line, "BUILD_END")) == NULL)
        return 0;
    etude_progress(s->l.c, "build", ++s->builds);
    held = strstr(w, "BUILD_RC=0") != NULL;
    lc_assert(&s->l, id, held, why);
    free(w);
    return held;
}

/**
 * Run copy command `line`, which prints COPY_OK when the copy is whole.
 * @return 1 when it is, else 0, with assertion `id` failed by `why`.
 */
static int copy(struct sb *s, struct etude_case_machine *m, uint64_t *at, const char *line,
                const char *id, const char *why)
{
    char *w = command(s, m, "build", at, line, "COPY_END");
    int held = w != NULL && strstr(w, "COPY_OK") != NULL;

    if (w != NULL && !held)
        lc_assert(&s->l, id, 0, why);
    free(w);
    return held;
}
/** Record that the scenario cannot go on, because `what` failed. */
static void stop(struct sb *s, const char *what)
{
    lc_halt(&s->l, ETUDE_FAILED, what);
}

/**
 * Copy the root's partition of the private copy of the ext2 disk to evidence
 * `root.ext2`, where debugfs and e2fsck read it. @return 1 when it was copied.
 */
static int ext2_root(struct sb *s)
{
    if (evidence(s, "root.ext2", s->root, sizeof(s->root)) == NULL)
        return 0;
    if (!copy_range(s->image, s->root, PART_OFFSET, PART_SIZE)) {
        stop(s, "the copy of the ext2 partition");
        return 0;
    }
    return 1;
}

/**
 * Report assertion `room`: the root of the private copy of the disk has the
 * free bytes a build needs, read on the host before the machine starts:
 * from the free-node chain of a KRFFS root, and from the superblock of an
 * ext2 one.
 */
static void room(struct sb *s)
{
    char name[] = "setidcheck", free_opt[] = "--free", image_opt[] = "--image",
         offset_opt[] = "--offset", offset[] = LC_PART_OFFSET, size_opt[] = "--size",
         size[] = KRFFS_PART_SIZE, debugfs[] = "debugfs", request_opt[] = "-R", request[] = "stats",
         why[160], *out;
    char *krffs[] = {name, free_opt, image_opt, s->image, offset_opt, offset, size_opt, size, NULL};
    char *ext2[] = {debugfs, request_opt, request, s->root, NULL};
    const char *copy, *p;
    long free_bytes = -1, blocks = -1, block = -1;

    if (!lc_step(&s->l, "room"))
        return;
    copy = etude_private_copy(s->l.c, s->disk);
    if (copy == NULL || strlen(copy) >= sizeof(s->image)) {
        lc_halt(&s->l, ETUDE_REFUSED, "the copy of the root's disk");
        return;
    }
    snprintf(s->image, sizeof(s->image), "%s", copy);
    if (s->ext2 && !ext2_root(s))
        return;
    host(s, s->ext2 ? ext2 : krffs, "room");
    if (!lc_ok(&s->l))
        return;
    out = slurp(etude_evidence(s->l.c, "room"), NULL);
    if (out != NULL && s->ext2) {
        if ((p = strstr(out, "Free blocks:")) != NULL)
            blocks = strtol(p + 12, NULL, 10);
        if ((p = strstr(out, "Block size:")) != NULL)
            block = strtol(p + 11, NULL, 10);
        if (blocks >= 0 && block > 0)
            free_bytes = blocks * block;
    } else if (out != NULL && *out >= '0' && *out <= '9') {
        free_bytes = strtol(out, NULL, 10);
    }
    free(out);
    snprintf(why, sizeof(why), "the root has %ld free bytes and a build needs %ld", free_bytes,
             NEED_BYTES);
    lc_assert(&s->l, "room", free_bytes >= NEED_BYTES, why);
}

/**
 * Report assertions `build-one` and `size` from the first build of the
 * kernel, at the shell of machine `build`, `m`, and keep the hash the guest
 * gives the kernel. @return 1 when the build made a kernel.
 */
static int first_build(struct sb *s, struct etude_case_machine *m, uint64_t *at)
{
    char *w, size[32], why[96];
    long bytes;

    if (!lc_step(&s->l, "build-one") ||
        !copy(s, m, at, copy_one, "build-one",
              "the copy of /src/rach2 into the workspace failed") ||
        !build(s, m, at, build_one, "build-one",
               "the first build of the kernel failed; its last lines are on the console") ||
        (w = command(s, m, "build", at, hash_one, "HASH_END")) == NULL) {
        stop(s, "the first build of the kernel");
        return 0;
    }
    word_after(w, "HASH_ONE=", s->hash, sizeof(s->hash));
    bytes = strtol(word_after(w, "SIZE_ONE=", size, sizeof(size)), NULL, 10);
    free(w);
    snprintf(why, sizeof(why), "the kernel the machine built is %ld bytes, which is no kernel",
             bytes);
    lc_assert(&s->l, "size", bytes >= KERNEL_FLOOR, why);
    free(command(s, m, "build", at, space, "DU_END"));
    if (bytes < KERNEL_FLOOR || strlen(s->hash) != 64) {
        stop(s, "the first build of the kernel");
        return 0;
    }
    return 1;
}

/**
 * Report assertions `build-two` and `same` from a second build of the
 * kernel from a fresh copy of the tree, which must give the same bytes.
 */
static void second_build(struct sb *s, struct etude_case_machine *m, uint64_t *at)
{
    char *w;

    if (!lc_step(&s->l, "build-two") ||
        !copy(s, m, at, copy_two, "build-two", "the second copy of /src/rach2 failed") ||
        !build(s, m, at, build_two, "build-two",
               "the second build of the kernel failed; its last lines are on the console") ||
        (w = command(s, m, "build", at, hash_two, "SAME_END")) == NULL) {
        if (lc_ok(&s->l))
            lc_notrun(&s->l, "same", "the second build of the kernel failed");
        return;
    }
    lc_assert(&s->l, "same", strstr(w, "HASH_SAME") != NULL,
              "two clean builds of one tree made two different kernels");
    free(w);
}

/**
 * Sync the root at the shell of machine `build`, `m`, and end the machine.
 * @return 1 when the sync was done and the machine has ended.
 */
static int sync_root(struct sb *s, struct etude_case_machine *m, uint64_t *at)
{
    char *w;
    int synced;

    if (!lc_step(&s->l, "sync") || (w = command(s, m, "build", at, sync_line, "SYNC_END")) == NULL)
        return 0;
    synced = strstr(w, "SYNC_OK") != NULL;
    free(w);
    if (!synced) {
        stop(s, "the sync of the root");
        return 0;
    }
    return lc_quit(&s->l, m, NULL);
}

/**
 * Report assertion `consistent`: e2fsck's forced check of the ext2 root,
 * read again after the synced machine was ended, finds nothing to repair.
 */
static void consistent(struct sb *s)
{
    char name[] = "e2fsck", force[] = "-fn";
    char *argv[] = {name, force, s->root, NULL};
    int clean;

    if (!lc_step(&s->l, "fsck") || !ext2_root(s))
        return;
    clean = host(s, argv, "e2fsck");
    if (lc_ok(&s->l))
        lc_assert(&s->l, "consistent", clean,
                  "e2fsck -fn finds faults in the root the machine built on, after its sync");
}

/**
 * Report assertions `extract` and `elfcheck`: the kernel the machine set
 * aside, taken off its disk, has the hash the guest gave it, and the
 * structural checker accepts it as a kernel that TinyCC linked.
 * @return 1 when both held.
 */
static int extract(struct sb *s)
{
    char readfile[] = "readfile.krffs", kept[] = KEPT, offset_opt[] = "--offset",
         offset[] = LC_PART_OFFSET, size_opt[] = "--size", size[] = KRFFS_PART_SIZE,
         debugfs[] = "debugfs", request_opt[] = "-R", request[1200], sha[] = "sha256",
         elfcheck[] = "elfcheck", kernel_opt[] = "--kernel", tcc_opt[] = "--tcc", why[200], *out;
    char *krffs[] = {readfile, s->image, kept, s->kernel, offset_opt, offset, size_opt, size, NULL};
    char *ext2[] = {debugfs, request_opt, request, s->root, NULL};
    char *hash[] = {sha, s->kernel, NULL};
    char *check[] = {elfcheck, kernel_opt, tcc_opt, s->kernel, NULL};
    long bytes;
    int same, checked;

    if (!lc_step(&s->l, "extract") || evidence(s, "kernel", s->kernel, sizeof(s->kernel)) == NULL)
        return 0;
    snprintf(request, sizeof(request), "dump %s %s", KEPT, s->kernel);
    host(s, s->ext2 ? ext2 : krffs, "extract");
    free(slurp(s->kernel, &bytes));
    if (!lc_ok(&s->l))
        return 0;
    if (bytes <= 0) {
        lc_assert(&s->l, "extract", 0,
                  "the kernel the machine built could not be taken off its disk");
        stop(s, "the extraction of the kernel");
        return 0;
    }
    host(s, hash, "sha256");
    out = slurp(etude_evidence(s->l.c, "sha256"), NULL);
    same = out != NULL && strncmp(out, s->hash, 64) == 0;
    snprintf(why, sizeof(why), "the kernel taken off the disk hashes %.64s and the guest said %s",
             out != NULL ? out : "to nothing", s->hash);
    free(out);
    if (!lc_ok(&s->l))
        return 0;
    lc_assert(&s->l, "extract", same, why);
    checked = host(s, check, "elfcheck");
    if (!lc_ok(&s->l))
        return 0;
    lc_assert(&s->l, "elfcheck", checked, "the structural checker refuses the kernel");
    if (!same || !checked)
        stop(s, "the check of the kernel the machine built");
    return same && checked;
}

/**
 * Write the file at `path` to `target` of the FAT volume at byte `offset`
 * of the disk at `disk`, in place of the file there, and read it back to
 * evidence `back`. @return 1 when the bytes read back are the bytes written.
 */
static int fat_put(struct sb *s, const char *disk, long offset, const char *path,
                   const char *target, const char *back)
{
    char mcopy[] = "mcopy", over[] = "-o", plain[] = "-n", image_opt[] = "-i", image[1100],
         from[1024], to[64], read[1024];
    char *put[] = {mcopy, over, image_opt, image, from, to, NULL};
    char *get[] = {mcopy, plain, image_opt, image, to, read, NULL};

    if (strlen(path) >= sizeof(from) || strlen(target) >= sizeof(to) ||
        evidence(s, back, read, sizeof(read)) == NULL)
        return 0;
    snprintf(image, sizeof(image), "%s@@%ld", disk, offset);
    snprintf(from, sizeof(from), "%s", path);
    snprintf(to, sizeof(to), "%s", target);
    return host(s, put, "mcopy") && host(s, get, "mcopy") && same_file(path, read);
}

/**
 * Give the path of the private copy of disk artifact `name` in `buf` of
 * `size` bytes, with the offsets of its first two partitions in `boot` and
 * `recovery`. @return `buf`, or NULL after halting.
 */
static char *grub_disk(struct sb *s, const char *name, char *buf, size_t size, long *boot,
                       long *recovery)
{
    const char *copy = etude_private_copy(s->l.c, name);

    if (copy == NULL || strlen(copy) >= size) {
        lc_halt(&s->l, ETUDE_REFUSED, "the copy of a GRUB disk");
        return NULL;
    }
    snprintf(buf, size, "%s", copy);
    *boot = partition_offset(buf, 0);
    *recovery = partition_offset(buf, 1);
    if (*boot < 0 || *recovery < 0) {
        stop(s, "the partition table of a GRUB disk");
        return NULL;
    }
    return buf;
}

/**
 * Report assertion `slot`: the kernel the machine built, padded to the
 * slot's size, is written in place of the placeholder in the candidate slot
 * and the recovery slot of the GRUB disk, and for the ext2 family in the
 * candidate slot of the disk of its ext2 root, and reads back whole.
 */
static void slot(struct sb *s)
{
    char padded[1024], disk[1024], why[160];
    long bytes, boot, recovery;
    char *k;
    FILE *f;
    int ok;

    if (!lc_step(&s->l, "slot") || evidence(s, "kernel.slot", padded, sizeof(padded)) == NULL)
        return;
    if ((k = slurp(s->kernel, &bytes)) == NULL) {
        stop(s, "the read of the kernel");
        return;
    }
    if (bytes > SLOT_BYTES) {
        free(k);
        snprintf(why, sizeof(why), "the kernel is %ld bytes and the slot has %ld", bytes,
                 (long)SLOT_BYTES);
        lc_assert(&s->l, "slot", 0, why);
        stop(s, "the kernel's slot");
        return;
    }
    f = fopen(padded, "wb");
    ok = f != NULL && fwrite(k, 1, (size_t)bytes, f) == (size_t)bytes &&
         fseek(f, SLOT_BYTES - 1, SEEK_SET) == 0 && fputc(0, f) == 0;
    if (f != NULL && fclose(f) != 0)
        ok = 0;
    free(k);
    if (!ok) {
        stop(s, "the padded kernel");
        return;
    }
    ok = grub_disk(s, "grub-disk", disk, sizeof(disk), &boot, &recovery) != NULL &&
         fat_put(s, disk, boot, padded, "::/boot/kernel", "slot.candidate") &&
         fat_put(s, disk, recovery, s->kernel, "::/boot/recovery", "slot.recovery");
    if (ok && s->ext2)
        ok = grub_disk(s, "roots-disk", disk, sizeof(disk), &boot, &recovery) != NULL &&
             fat_put(s, disk, boot, padded, "::/boot/kernel", "slot.roots");
    if (!lc_ok(&s->l))
        return;
    lc_assert(&s->l, "slot", ok, "a slot of a GRUB disk does not read back as the kernel written");
    if (!ok)
        stop(s, "the slots of the GRUB disks");
}

/** Report assertion `grub-login`: GRUB starts the kernel to a login prompt. */
static void grub_login(struct sb *s)
{
    struct etude_case_machine *m;
    uint64_t at = 0;

    if (lc_step(&s->l, "grub") && (m = lc_boot(&s->l, "grub")) != NULL &&
        lc_wait(&s->l, m, "login:", &at, "grub-login"))
        lc_quit(&s->l, m, NULL);
}

/**
 * Report assertion `timing` from the console of machine `machine` up to
 * `at`, the first prompt of the debug shell: the kernel's record of its
 * clock's check is at level 6.
 */
static void timing(struct sb *s, const char *machine, uint64_t at)
{
    char *w = lc_console(&s->l, machine, 0, at);
    int level;

    if (w == NULL)
        return;
    level = lc_timing_level(w, strlen(w));
    lc_assert(&s->l, "timing", level == 6,
              level == 3 ? "the kernel's timing record is at level 3: its check failed"
                         : "the kernel said nothing about time");
    free(w);
}

/**
 * Run each of the probes `names`, ended by NULL, at the debug shell of
 * machine `machine`, `m`, whose console has been read to its prompt at
 * `*at`, and report assertion `id`: every one exited with status 0, and none
 * was killed. Each probe advances the counter witness `probe`. clockmusl is
 * given the clock reference's port, `s->port`, and when that is not 0 its
 * result of the comparison is assertion `reference`.
 */
static void run_probes(struct sb *s, struct etude_case_machine *m, const char *machine,
                       const char *const names[], uint64_t *at, const char *id)
{
    char line[128], why[160], first[200], detail[260], *w;
    size_t i, failed = 0;
    int clock, said = 0;
    uint64_t from;

    first[0] = '\0';
    for (i = 0; names[i] != NULL && lc_ok(&s->l); i++) {
        clock = strcmp(names[i], "musl/clockmusl") == 0;
        if (clock)
            snprintf(line, sizeof(line), "exec /%s %u\r", names[i], s->port);
        else
            snprintf(line, sizeof(line), "exec /%s\r", names[i]);
        from = *at;
        if (!lc_type(&s->l, m, line) || !lc_wait(&s->l, m, back, at, NULL) ||
            (w = lc_console(&s->l, machine, from, *at)) == NULL)
            break;
        if (!lc_exited0(w, strlen(w), why, sizeof(why)) && failed++ == 0)
            snprintf(first, sizeof(first), "%s: %s", names[i], why);
        if (clock)
            said = lc_clockref_said(w, strlen(w));
        free(w);
        etude_progress(s->l.c, "probe", ++s->probes_run);
        if (!lc_wait(&s->l, m, greeting, at, NULL) || !lc_wait(&s->l, m, "\n> ", at, NULL))
            break;
    }
    if (!lc_ok(&s->l))
        return;
    snprintf(detail, sizeof(detail), "%lu of %lu probes failed, the first %s",
             (unsigned long)failed, (unsigned long)i, first);
    lc_assert(&s->l, id, failed == 0, detail);
    if (s->port != 0)
        lc_assert(&s->l, "reference", said,
                  "clockmusl gave no result of its comparison with the reference");
}

/**
 * Report assertions `timing`, `probes` and `reference`: the kernel the
 * machine built, started by GRUB's recovery entry with the test image in
 * RAM, passes the probes of artifact `probes-list` at its debug shell.
 */
static void probes(struct sb *s)
{
    static const char menu[] = "set next_entry=recovery\n";
    const char *names[PROBE_LIMIT + 1];
    char cfg[1024], disk[1024], *list, *p;
    long boot, recovery;
    size_t n = 0;
    pid_t ref;
    uint64_t at = 0;
    struct etude_case_machine *m;
    FILE *f;
    int ok;

    if (!lc_step(&s->l, "probes") || evidence(s, "boot.cfg", cfg, sizeof(cfg)) == NULL ||
        grub_disk(s, "grub-disk", disk, sizeof(disk), &boot, &recovery) == NULL)
        return;
    f = fopen(cfg, "w");
    ok = f != NULL && fputs(menu, f) >= 0;
    if (f != NULL && fclose(f) != 0)
        ok = 0;
    if (!ok || !fat_put(s, disk, boot, cfg, "::/boot/boot.cfg", "slot.menu")) {
        stop(s, "the menu that selects the recovery entry");
        return;
    }
    if ((list = slurp(etude_artifact(s->l.c, "probes-list"), NULL)) == NULL) {
        stop(s, "the read of the list of probes");
        return;
    }
    for (p = strtok(list, "\n"); p != NULL && n < PROBE_LIMIT; p = strtok(NULL, "\n"))
        names[n++] = p;
    names[n] = NULL;
    if ((ref = lc_clockref_start(&s->port)) < 0) {
        free(list);
        stop(s, "the clock reference's socket");
        return;
    }
    if ((m = lc_boot(&s->l, "probes")) != NULL && lc_wait(&s->l, m, greeting, &at, NULL) &&
        lc_wait(&s->l, m, "\n> ", &at, NULL)) {
        timing(s, "probes", at);
        run_probes(s, m, "probes", names, &at, "probes");
        lc_quit(&s->l, m, NULL);
    }
    lc_clockref_stop(ref);
    s->port = 0;
    free(list);
}

/**
 * Report assertion `roots`: the kernel the machine built, started by GRUB
 * on an ext2 root of the test manifest, passes the probes of an installed
 * root at its debug shell.
 */
static void roots(struct sb *s)
{
    struct etude_case_machine *m;
    uint64_t at;

    if ((m = lc_shell(&s->l, "roots", "roots", &at)) == NULL)
        return;
    run_probes(s, m, "roots", root_probes, &at, "roots");
    lc_quit(&s->l, m, NULL);
}

/** Run case `kernel` of either family. */
static void kernel_case(struct sb *s)
{
    struct etude_case_machine *m;
    uint64_t at;

    room(s);
    if ((m = login(s, "test", "test-password", "$ ", &at)) == NULL || !first_build(s, m, &at))
        return;
    if (!s->ext2)
        second_build(s, m, &at);
    if (!sync_root(s, m, &at))
        return;
    if (s->ext2)
        consistent(s);
    if (!extract(s))
        return;
    slot(s);
    grub_login(s);
    probes(s);
    if (s->ext2)
        roots(s);
}

/** Run case `userland`: build the utilities and the shell, and run them. */
static void userland_case(struct sb *s)
{
    struct etude_case_machine *m;
    uint64_t at;
    size_t i;
    char *w;

    if ((m = login(s, "test", "test-password", "$ ", &at)) == NULL || !lc_step(&s->l, "build") ||
        !copy(s, m, &at, copy_user, "utils-built", "the copy of /src/utils and /src/ish failed") ||
        !build(s, m, &at, build_utils, "utils-built",
               "the build of the utilities failed; its last lines are on the console") ||
        !build(s, m, &at, build_ish, "ish-built",
               "the build of the shell failed; its last lines are on the console")) {
        stop(s, "the build of the utilities and the shell");
        return;
    }
    if (!lc_step(&s->l, "smoke"))
        return;
    for (i = 0; i < sizeof(smokes) / sizeof(smokes[0]) && lc_ok(&s->l); i++) {
        if (strcmp(smokes[i].id, "smoke-httpd") == 0) {
            if ((w = command(s, m, "build", &at, httpd_up, "UP_END")) == NULL)
                break;
            free(w);
        }
        if ((w = command(s, m, "build", &at, smokes[i].line, smokes[i].end)) == NULL)
            break;
        lc_assert(&s->l, smokes[i].id, strstr(w, smokes[i].token) != NULL, smokes[i].why);
        free(w);
    }
    if (lc_ok(&s->l))
        lc_quit(&s->l, m, NULL);
}

/** Run case `inplace`: root builds the staged /src/ish where it is. */
static void inplace_case(struct sb *s)
{
    struct etude_case_machine *m;
    uint64_t at;

    if ((m = login(s, "root", "demo-password", "# ", &at)) == NULL || !lc_step(&s->l, "inplace"))
        return;
    if (build(s, m, &at, build_inplace, "inplace",
              "make -f guest.mk in the staged /src/ish failed; its last lines are on the console"))
        lc_quit(&s->l, m, NULL);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const krffs_ids[] = {"room",   "login",   "build-one", "size", "build-two",
                                            "same",   "extract", "elfcheck",  "slot", "grub-login",
                                            "timing", "probes",  "reference", NULL};
    static const char *const ext2_ids[] = {
        "room", "login",      "build-one", "size",   "consistent", "extract", "elfcheck",
        "slot", "grub-login", "timing",    "probes", "reference",  "roots",   NULL};
    static const char *const user_ids[] = {"login",
                                           "utils-built",
                                           "ish-built",
                                           "smoke-cat",
                                           "smoke-echo",
                                           "smoke-true-false",
                                           "smoke-pwd",
                                           "smoke-ls",
                                           "smoke-touch-mkdir",
                                           "smoke-cp-mv-rm",
                                           "smoke-yes",
                                           "smoke-ping",
                                           "smoke-httpd",
                                           "smoke-built",
                                           "smoke-login",
                                           "smoke-su",
                                           "smoke-sudo",
                                           "smoke-passwd",
                                           "smoke-adduser",
                                           "smoke-bootctl",
                                           "smoke-ish",
                                           NULL};
    static const char *const inplace_ids[] = {"login", "inplace", NULL};
    const char *name = case_name(c);
    struct sb s;

    memset(&s, 0, sizeof(s));
    s.ext2 = strncmp(etude_case_id(c), "machine.selfbuild-ext2.", 23) == 0;
    s.disk = s.ext2 ? "selfbuild-ext2-disk" : "selfbuild-disk";
    if (strcmp(name, "kernel") == 0) {
        lc_begin(&s.l, c, s.ext2 ? ext2_ids : krffs_ids);
        kernel_case(&s);
    } else if (strcmp(name, "userland") == 0 && !s.ext2) {
        lc_begin(&s.l, c, user_ids);
        userland_case(&s);
    } else if (strcmp(name, "inplace") == 0) {
        lc_begin(&s.l, c, inplace_ids);
        inplace_case(&s);
    } else {
        return 1;
    }
    return lc_finish(&s.l);
}
