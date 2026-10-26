/*
 * metal_accept.c: the QEMU metal acceptance's controller. It runs the
 * machine's programs through the declared Rach2 SSH client, judges their
 * records in C, and checks written images with independent image tools.
 * The native Lenovo route remains tests/system/accept/metal_accept.sh.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "metal_accept.h"
#include "fileio.h"
#include <sys/stat.h>

/* Each command has evidence. Every guest mount and temporary directory is
 * recorded for cleanup before the case ends. */
struct acceptance {
    struct net_case *u;
    const struct metal_accept_request *request;
    FILE *log;
    unsigned commands, fails;
    int command_failed, installed;
    char prefix[128], root_type[32];
    char *cmdline, *mounts, *disk, *bootlog;
    char temporary[256], mounted[320], transfer[256], stick_file[384];
    int loop;
};
#define RECORD_LINE 2048

/** Give an allocated empty string when a command cannot return text. */
static char *empty_text(void)
{
    char *s = malloc(1);
    if (s == NULL)
        abort();
    s[0] = '\0';
    return s;
}

/** Remove carriage returns and trailing newlines in place. */
static char *trim(char *s)
{
    size_t i, at = 0;
    if (s == NULL)
        return s;
    for (i = 0; s[i]; i++)
        if (s[i] != '\r')
            s[at++] = s[i];
    while (at > 0 && s[at - 1] == '\n')
        at--;
    s[at] = '\0';
    return s;
}

/** Write one note or counted failure. */
static void message(struct acceptance *a, int failed, const char *format, ...)
{
    va_list ap;
    fprintf(a->log, failed ? "  FAIL: " : "  ");
    va_start(ap, format);
    vfprintf(a->log, format, ap);
    va_end(ap);
    fputc('\n', a->log);
    if (failed)
        a->fails++;
}

/** Run or download through the login account with the same SSH retries. */
static enum etude_status account_mode(struct acceptance *a, const char *command, const char *input,
                                      const char *output, struct net_run *r, int download)
{
    char who[128];
    unsigned tries;
    enum etude_status st;
    char *argv[] = {NET_SSH,
                    NET_SSH_KEY((char *)a->request->login.key),
                    "-T",
                    "-p",
                    (char *)a->request->login.port,
                    who,
                    (char *)command,
                    NULL};
    snprintf(who, sizeof(who), "%s@127.0.0.1", a->request->login.user);
    for (tries = 0;; tries++) {
        const char *error;
        st = download ? net_ssh_download(a->u, argv, output, r)
                      : net_ssh_input(a->u, argv, input, output, r);
        error = r->err != NULL ? r->err : "";
        if (st != ETUDE_OK || tries == 3 || r->end.end != ETUDE_END_EXITED || r->end.value != 255 ||
            (strstr(error, "kex_exchange_identification") == NULL &&
             strstr(error, "ssh_exchange_identification") == NULL &&
             strstr(error, "Connection refused") == NULL &&
             strstr(error, "Connection timed out") == NULL))
            return st;
        net_run_free(r);
        {
            const char *pause[] = {"/bin/sleep", "1", NULL};
            struct etude_guest_result waited;
            st = etude_guest_command(a->u->client, pause, NULL, NULL, "accept-retry", &waited);
            if (st != ETUDE_OK) {
                memset(r, 0, sizeof(*r));
                r->end.end = ETUDE_END_LOST;
                r->end.value = -1;
                return st;
            }
        }
    }
}

/** Run a command as the login account, without su or a terminal. */
static enum etude_status account(struct acceptance *a, const char *command, const char *input,
                                 const char *output, struct net_run *r)
{
    return account_mode(a, command, input, output, r, 0);
}

/** Run a required root command, retaining its status even when the caller
 * reads only its output. The caller frees the returned text. */
static char *read_command(struct acceptance *a, const char *command)
{
    struct net_run r;
    char output[180];
    char *text;
    enum etude_status st;
    snprintf(output, sizeof(output), "%s.command-%u", a->prefix, ++a->commands);
    if (a->request->use_su)
        st = net_su(a->u, &a->request->login, command, output, &r);
    else
        st = account(a, command, NULL, output, &r);
    if (st != ETUDE_OK || !net_exited0(&r)) {
        a->command_failed = 1;
        message(a, 1, "a required remote command failed (status %d): %.160s", r.end.value, command);
    }
    text = r.out;
    r.out = NULL;
    net_run_free(&r);
    return trim(text != NULL ? text : empty_text());
}

/** Run the formatted command and return its text. */
static char *read_format(struct acceptance *a, const char *format, ...)
{
    char command[8192];
    va_list ap;
    int n;
    va_start(ap, format);
    n = vsnprintf(command, sizeof(command), format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(command)) {
        message(a, 1, "a guest command is too long");
        return empty_text();
    }
    return read_command(a, command);
}

/** Judge a command's output against an exact string. */
static int expect(struct acceptance *a, const char *command, const char *expected, const char *held,
                  const char *failed)
{
    char *text = read_command(a, command);
    int ok = text != NULL && strcmp(text, expected) == 0;
    message(a, !ok, "%s%s%s", ok ? held : failed, ok ? "" : ": ", ok ? "" : text);
    free(text);
    return ok;
}

/** Check a generated path before placing it in a guest command. */
static int plain_path(const char *path, const char *prefix)
{
    const unsigned char *p = (const unsigned char *)path;
    if (strncmp(path, prefix, strlen(prefix)) != 0 || strstr(path, "..") != NULL ||
        strstr(path, "//") != NULL)
        return 0;
    for (; *p; p++)
        if (!isalnum(*p) && strchr("/._-", *p) == NULL)
            return 0;
    return 1;
}

/** Copy all bytes of a host file, including zero bytes. */
static int copy_file(const char *from, const char *to)
{
    unsigned char bytes[65536];
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    size_t n;
    int ok = in != NULL && out != NULL;
    while (ok && (n = fread(bytes, 1, sizeof(bytes), in)) != 0)
        ok = fwrite(bytes, 1, n, out) == n;
    if (in != NULL) {
        if (ferror(in))
            ok = 0;
        fclose(in);
    }
    if (out != NULL && fclose(out) != 0)
        ok = 0;
    return ok;
}

/** Make the account-owned directory through which su transfers a file. */
static int transfer_directory(struct acceptance *a)
{
    struct net_run r;
    char output[180];
    int ok;
    snprintf(output, sizeof(output), "%s.transfer-%u", a->prefix, ++a->commands);
    account(a, "mktemp -d /tmp/accept-transfer.XXXXXX", NULL, output, &r);
    trim(r.out);
    ok = net_exited0(&r) && r.out != NULL && plain_path(r.out, "/tmp/accept-transfer.") &&
         strlen(r.out) < sizeof(a->transfer);
    if (ok)
        strcpy(a->transfer, r.out);
    net_run_free(&r);
    return ok;
}

/** Remove the account's transfer directory. */
static void transfer_end(struct acceptance *a)
{
    struct net_run r;
    char command[320], output[180];
    if (a->transfer[0] == '\0')
        return;
    snprintf(command, sizeof(command), "rm -rf %s", a->transfer);
    snprintf(output, sizeof(output), "%s.transfer-clean-%u", a->prefix, ++a->commands);
    account(a, command, NULL, output, &r);
    if (!net_exited0(&r))
        a->command_failed = 1;
    net_run_free(&r);
    a->transfer[0] = '\0';
}

/** Transfer as the account, then move as root when su is used. */
static int put_file(struct acceptance *a, const char *host, const char *guest)
{
    struct net_run r;
    char command[640], output[180], *text;
    int ok;
    if (a->request->use_su && !transfer_directory(a))
        return 0;
    snprintf(command, sizeof(command), "cat > %s%s", a->request->use_su ? a->transfer : guest,
             a->request->use_su ? "/data" : "");
    snprintf(output, sizeof(output), "%s.put-%u", a->prefix, ++a->commands);
    account(a, command, host, output, &r);
    ok = net_exited0(&r);
    net_run_free(&r);
    if (ok && a->request->use_su) {
        text = read_format(a, "mv %s/data %s", a->transfer, guest);
        free(text);
        ok = !a->command_failed;
    }
    transfer_end(a);
    return ok;
}

/** Fetch binary output, moving a root-owned image through the account's
 * directory when su is used, and restoring its original path afterwards. */
static int get_file(struct acceptance *a, const char *guest, const char *host)
{
    struct net_run r;
    char command[640], output[180], *text;
    const char *evidence;
    int ok;
    enum etude_status st;
    if (a->request->use_su) {
        if (!transfer_directory(a))
            return 0;
        text =
            read_format(a, "mv %s %s/data && chmod 644 %s/data", guest, a->transfer, a->transfer);
        free(text);
    }
    snprintf(command, sizeof(command), "cat %s%s", a->request->use_su ? a->transfer : guest,
             a->request->use_su ? "/data" : "");
    snprintf(output, sizeof(output), "%s.get-%u", a->prefix, ++a->commands);
    st = account_mode(a, command, NULL, output, &r, 1);
    evidence = etude_evidence(a->u->c, output);
    ok = st == ETUDE_OK && net_exited0(&r) && evidence != NULL && copy_file(evidence, host);
    net_run_free(&r);
    if (a->request->use_su) {
        text = read_format(a, "if [ -f %s/data ]; then mv %s/data %s; fi", a->transfer, a->transfer,
                           guest);
        free(text);
    }
    transfer_end(a);
    return ok;
}

/** Give one matching record line, without its newline. */
static char *record_line(const char *text, const char *prefix, char out[RECORD_LINE])
{
    const char *line = text, *end;
    size_t len, prefix_len = strlen(prefix);
    out[0] = '\0';
    while (line != NULL && *line) {
        end = strchr(line, '\n');
        len = end != NULL ? (size_t)(end - line) : strlen(line);
        if (len >= prefix_len && memcmp(line, prefix, prefix_len) == 0) {
            if (len >= RECORD_LINE)
                len = RECORD_LINE - 1;
            memcpy(out, line, len);
            out[len] = '\0';
            break;
        }
        line = end != NULL ? end + 1 : NULL;
    }
    return out;
}

/** Give the Nth whitespace-separated field of TEXT. */
static const char *field(const char *text, unsigned n, char *out, size_t size)
{
    const char *p = text, *end;
    size_t len;
    out[0] = '\0';
    while (*p) {
        p += strspn(p, " \t");
        end = p + strcspn(p, " \t\r\n");
        if (--n == 0) {
            len = (size_t)(end - p);
            if (len >= size)
                len = size - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            break;
        }
        p = end;
        if (*p == '\n' || *p == '\r')
            break;
    }
    return out;
}

/** Print selected lines of a machine record. */
static void lines(struct acceptance *a, const char *text, const char *pattern, const char *prefix)
{
    const char *p = text, *end, *found;
    char line[RECORD_LINE];
    size_t len;
    while (p != NULL && *p) {
        end = strchr(p, '\n');
        len = end != NULL ? (size_t)(end - p) : strlen(p);
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        found = strstr(line, pattern);
        if (found != NULL)
            message(a, 0, "%s%s", prefix, pattern[0] == '\0' ? line : found);
        p = end != NULL ? end + 1 : NULL;
    }
}

/** Identify the root and whether it was installed on a disk. */
static int root(struct acceptance *a)
{
    char *text, line[RECORD_LINE], mount[RECORD_LINE], device[128], point[128], type[32];
    const char *p;
    static const char *const disk_options[] = {"rach2.fsdisk",         "rach2.root=disk",
                                               "rach2.root=ext2",      "rach2.root=MBRID=",
                                               "rach2.root=GPTID=",    "rach2.root=PARTUUID=",
                                               "rach2.root=UUID=",     "rach2.root=LABEL=",
                                               "rach2.root=/dev/disk", NULL};
    size_t i;
    if (!expect(a, "echo ALIVE", "ALIVE", "SSH answers", "no SSH answer") ||
        !expect(a, "id -u", "0", "root commands are available", "root commands are required"))
        return 0;
    a->cmdline = read_command(a, "cat /proc/cmdline");
    a->mounts = read_command(a, "cat /proc/mounts");
    if (a->cmdline[0] == '\0') {
        message(a, 1, "the command line is empty");
        return 0;
    }
    p = a->mounts;
    while (*p) {
        record_line(p, "", mount);
        if (sscanf(mount, "%127s %127s %31s", device, point, type) == 3 && strcmp(point, "/") == 0)
            snprintf(a->root_type, sizeof(a->root_type), "%s", type);
        p = strchr(p, '\n');
        if (p == NULL)
            break;
        p++;
    }
    if (strcmp(a->root_type, "ext2") != 0 && strcmp(a->root_type, "krffs") != 0) {
        message(a, 1, "the mount table has no supported root type");
        return 0;
    }
    if ((strstr(a->cmdline, "rach2.rootfstype=ext2") != NULL ||
         strstr(a->cmdline, "rach2.root=ext2") != NULL) &&
        strcmp(a->root_type, "ext2") != 0) {
        message(a, 1, "the requested ext2 root is not mounted");
        return 0;
    }
    for (i = 0; disk_options[i] != NULL; i++)
        if (strstr(a->cmdline, disk_options[i]) != NULL)
            a->installed = 1;
    text = read_command(a, "uname -a");
    message(a, 0, "uname: %s", text);
    free(text);
    text = read_command(a, "cat /proc/uptime");
    message(a, 0, "uptime ticks: %s", text);
    free(text);
    message(a, 0, "boot line: %s", a->cmdline);
    text = read_command(a, "id");
    message(a, 0, "identity: %s", text);
    free(text);
    p = strstr(a->cmdline, "rach2.ip=");
    field(p != NULL ? p : "none", 1, line, sizeof(line));
    message(a, 0, "address given: %s", line);
    text = read_command(a, "dd if=/dev/urandom bs=16 count=1 2>/dev/null | wc -c");
    message(a, strtoul(text, NULL, 10) != 16, "the random pool %s (16 bytes from /dev/urandom)",
            strtoul(text, NULL, 10) == 16 ? "is trusted" : "refused a read");
    free(text);
    return 1;
}

/** Check the disk registry, whole-disk window and the recorded baseline. */
static void disk(struct acceptance *a)
{
    char line[RECORD_LINE], identity[RECORD_LINE], policy[32], mounted[32], sectors[32];
    char *text, hex[65] = "";
    a->disk = read_command(a, "cat /proc/disk 2>/dev/null");
    if (a->disk[0] == '\0') {
        message(a, 1, "/proc/disk gave no disk report");
        return;
    }
    field(record_line(a->disk, "policy ", line), 2, policy, sizeof(policy));
    message(a, 0, "disk: policy=%s", policy);
    message(a, strcmp(policy, a->installed ? "read-write" : "read-only") != 0,
            "the %s kernel %s the required disk policy", a->installed ? "installed" : "running",
            strcmp(policy, a->installed ? "read-write" : "read-only") == 0 ? "has"
                                                                           : "does not have");
    record_line(a->disk, "disk 0 ", line);
    record_line(a->disk, "identity 0 ", identity);
    if (line[0] == '\0') {
        message(a, a->installed, "disk: %s disk0 in the registry", a->installed ? "missing" : "no");
        return;
    }
    message(a, 0, "disk: %s", line);
    message(a, 0, "disk: %s", identity);
    field(identity, 5, mounted, sizeof(mounted));
    if (strcmp(mounted, "free") != 0 && strcmp(mounted, "mounted") != 0)
        message(a, 1, "disk: disk0 has no valid identity record");
    if (strcmp(mounted, a->installed ? "mounted" : "free") != 0)
        message(a, 1, "the disk's mount state does not match the root");
    text = read_command(a, "if [ -b /dev/disk0 ]; then echo published; else echo absent; fi");
    if (strcmp(text, "published") == 0) {
        free(text);
        text = read_command(
            a, "f=$(mktemp /dev/shm/accept-mbr.XXXXXX) || exit 1; "
               "trap 'rm -f \"$f\"' EXIT; /bin/busybox dd if=/dev/disk0 of=\"$f\" bs=512 count=1 "
               ">/dev/null 2>&1 && [ \"$(wc -c < \"$f\")\" = 512 ] && sha256sum \"$f\"");
        if (strspn(text, "0123456789abcdef") == 64) {
            memcpy(hex, text, 64);
            hex[64] = '\0';
            message(a, 0, "disk: the MBR hashes to %s", hex);
        } else
            message(a, 1,
                    "disk: the published whole-disk window did not supply one complete sector");
    } else if (strcmp(text, "absent") == 0)
        message(a, 0, "disk: /dev/disk0 is not published; no MBR read on this boot");
    else
        message(a, 1, "disk: the whole-disk window check gave no answer");
    free(text);
    lines(a, a->disk, "part 0 ", "disk: ");
    if (strstr(line, "Samsung SSD 870 EVO 1TB") != NULL) {
        field(line, 5, sectors, sizeof(sectors));
        message(a, strcmp(sectors, "1953525168") != 0, "disk: baseline sector count is %s",
                sectors);
        if (hex[0] != '\0')
            message(a,
                    strcmp(hex,
                           "652ad40fe72061185cf3a69c1a9a45394f68374f409153e082b7c1fa04c107d3") != 0,
                    "disk: the MBR comparison with the Windows baseline");
    } else
        message(a, 0, "disk: not the drive the baseline describes, so nothing is compared");
}

/** Read the boot log once and judge the hardware inventory from it. */
static void inventory(struct acceptance *a)
{
    char path[4096] = "", *interrupts, line[RECORD_LINE], count[64];
    FILE *f;
    unsigned n = 0;
    const char *p;
    a->bootlog = read_command(a, "cat /dev/bootlog");
    if (a->bootlog[0] == '\0')
        message(a, 1, "/dev/bootlog gave nothing; the log is empty or unavailable");
    if (net_path(a->u, "metal-bootlog.txt", path, sizeof(path)) != NULL &&
        (f = fopen(path, "wb")) != NULL) {
        if (fwrite(a->bootlog, 1, strlen(a->bootlog), f) != strlen(a->bootlog))
            message(a, 1, "the boot log snapshot could not be written");
        if (fclose(f) != 0)
            message(a, 1, "the boot log snapshot could not be closed");
    } else
        message(a, 1, "the boot log snapshot could not be created");
    for (p = a->bootlog; *p; p++)
        if (*p == '\n')
            n++;
    message(a, 0, "boot log: %u lines saved to %s", n, path);
    if (strstr(a->bootlog, "refused command") != NULL)
        message(a, 1, "the boot log records a refused disk command");
    if (strstr(a->bootlog, "usb: controller:") == NULL)
        message(a, 1, "the boot log has no usb inventory");
    else {
        lines(a, a->bootlog, "usb: ", "");
        if (strstr(a->bootlog, "usb: refused:") != NULL)
            message(a, 1, "the USB controller was refused");
        else if (strstr(a->bootlog, "usb: device: port") != NULL)
            message(a, 0, "usb: a device was enumerated on this machine");
        else if (strstr(a->bootlog, "usb: ready:") != NULL)
            message(a, 0, "usb: the controller came up and nothing was enumerated on it");
        else if (strstr(a->bootlog, "usb: controller: none") == NULL)
            message(a, 1, "a USB controller was found and its stack did not come up");
    }
    interrupts = read_command(a, "cat /proc/interrupts 2>/dev/null");
    if (interrupts[0] == '\0')
        message(a, 1, "/proc/interrupts is not available");
    else {
        lines(a, interrupts, "", "interrupts: ");
        field(record_line(interrupts, "32:", line), 2, count, sizeof(count));
        if (count[0] == '\0' || strspn(count, "0123456789") != strlen(count))
            message(a, 1, "the timer's vector has no interrupt count");
        else
            message(a, 0, "interrupts: the timer has taken %s, so its interrupts arrive", count);
        if (strstr(a->bootlog, "lapic: refused:") != NULL)
            lines(a, a->bootlog, "lapic: refused:", "interrupts: ");
        else if (strstr(a->bootlog, "lapic: ready:") != NULL)
            lines(a, a->bootlog, "lapic: ready:", "interrupts: ");
        else
            message(a, 1, "the boot log says nothing about the local APIC");
        if (strstr(a->bootlog, "lapic: timer:") != NULL)
            message(a, 1, "the timer stopped when the APIC was enabled");
    }
    free(interrupts);
}

/** Compare the FAT listing's file sizes with bootctl's independent report. */
static void boot_sizes(struct acceptance *a, const char *listing, const char *slots)
{
    static const char *const names[] = {"kernel", "fallback", "boot.cfg", "grubenv"};
    char line[RECORD_LINE], slot[RECORD_LINE], prefix[80], name[128], size[64];
    const char *p;
    unsigned long long have, want;
    unsigned i;
    for (i = 0; i < 4; i++) {
        have = want = 0;
        p = listing;
        while (p != NULL && *p) {
            record_line(p, "", line);
            field(line, 9, name, sizeof(name));
            if (strcmp(name, names[i]) == 0) {
                field(line, 5, size, sizeof(size));
                have = strtoull(size, NULL, 10);
            }
            p = strchr(p, '\n');
            if (p != NULL)
                p++;
        }
        snprintf(prefix, sizeof(prefix), "bootctl: %s:", i == 0 ? "candidate" : names[i]);
        record_line(slots, prefix, slot);
        p = strstr(slot, " bytes");
        if (p != NULL) {
            while (p > slot && isdigit((unsigned char)p[-1]))
                p--;
            want = strtoull(p, NULL, 10);
        }
        message(a, have == 0 || have != want,
                "partitions: /boot/%s is %llu bytes on the mount and %llu in the slot report",
                names[i], have, want);
    }
}

/** A lag stimulus checks guest progress and guest ticks, never host time. */
static void block_lag(struct acceptance *a)
{
    const char *p = strstr(a->cmdline, "rach2.blocklag=");
    char *text;
    unsigned long lag, count;
    double start, end;
    unsigned long ticks;
    if (p == NULL) {
        message(a, 0, "partitions: the lag check needs rach2.blocklag=N on the boot line");
        return;
    }
    lag = strtoul(p + 15, NULL, 10);
    text = read_command(
        a,
        "rm -f /tmp/accept-lag-ok /tmp/accept-lag-bad; t0=$(cut -d' ' -f1 /proc/uptime); "
        "(if dd if=/dev/disk0p1 of=/dev/null bs=65536 count=1 2>/dev/null; then "
        "touch /tmp/accept-lag-ok; else touch /tmp/accept-lag-bad; fi) & i=0; "
        "while [ ! -f /tmp/accept-lag-ok ] && [ ! -f /tmp/accept-lag-bad ]; do i=$((i+1)); done; "
        "t1=$(cut -d' ' -f1 /proc/uptime); if [ -f /tmp/accept-lag-ok ]; then echo ok $i $t0 $t1; "
        "else echo bad $i $t0 $t1; fi; rm -f /tmp/accept-lag-ok /tmp/accept-lag-bad");
    if (sscanf(text, "ok %lu %lf %lf", &count, &start, &end) != 3)
        message(a, 1, "partitions: the lag read failed or gave no answer: %s", text);
    else {
        ticks = end >= start ? (unsigned long)((end - start) * 100 + .5) : 0;
        message(a, count == 0 || ticks < 16 * lag,
                "partitions: the shell counted to %lu while a 64 KiB read slept %lu ticks through "
                "lag %lu",
                count, ticks, lag);
    }
    free(text);
}

/** Judge partition publication and an installed ext2 root. */
static void partitions(struct acceptance *a)
{
    char *nodes, *slots, *text;
    static const char boot_command[] =
        "mkdir -p /mnt/accept-boot && mount -r -t vfat /dev/disk0p1 "
        "/mnt/accept-boot && grep '^/dev/disk0p1 /mnt/accept-boot vfat ro' /proc/mounts && "
        "ls -l /mnt/accept-boot/boot /mnt/accept-boot/boot/grub; status=$?; "
        "umount /mnt/accept-boot || status=1; rmdir /mnt/accept-boot || status=1; exit $status";
    char command[1200];
    unsigned i;
    nodes = read_command(a, "ls /dev 2>/dev/null");
    slots = read_command(a, a->installed ? "bootctl status 2>/dev/null"
                                         : "bootctl status 2>/dev/null || :");
    if (strstr(nodes, "disk0p") == NULL) {
        message(a, slots[0] != '\0', "partitions: %s",
                slots[0] == '\0' ? "no partition nodes (no disk, or an empty table)"
                                 : "bootctl reports slots without partition nodes");
    } else {
        lines(a, nodes, "disk0p", "partitions: ");
        if (slots[0] != '\0')
            for (i = 1; i <= 3; i++) {
                snprintf(command, sizeof(command), "disk0p%u", i);
                if (strstr(nodes, command) == NULL)
                    message(a, 1, "partitions: the installed layout has no %s", command);
            }
        if (a->installed || slots[0] != '\0')
            snprintf(command, sizeof(command), "%s", boot_command);
        else
            snprintf(command, sizeof(command), "( %s ) || :", boot_command);
        text = read_command(a, command);
        if (text[0] == '\0')
            message(a, 0, "partitions: /dev/disk0p1 is not a FAT volume this kernel mounts");
        else {
            message(a,
                    strncmp(text, "/dev/disk0p1 /mnt/accept-boot vfat ro",
                            strlen("/dev/disk0p1 /mnt/accept-boot vfat ro")) != 0,
                    "partitions: /dev/disk0p1 mounted read-only, as asked");
            if (strstr(text, " kernel") == NULL)
                message(a, a->installed || slots[0] != '\0',
                        "partitions: /dev/disk0p1 is a FAT volume without a kernel");
            else
                boot_sizes(a, text, slots);
        }
        free(text);
        block_lag(a);
    }
    free(nodes);
    if (strcmp(a->root_type, "ext2") == 0) {
        message(a, strstr(a->mounts, "/dev/disk0p3 / ext2 rw,relatime") == NULL,
                "the ext2 root is mounted writable from /dev/disk0p3");
        message(a, strstr(slots, "booted: candidate") == NULL,
                "the ext2 root boot came from the candidate slot");
        if (strstr(a->bootlog, "ext2: check") != NULL) {
            const char *p = a->bootlog;
            while ((p = strstr(p, "ext2: check")) != NULL) {
                if (strncmp(p, "ext2: check: clean", 17) != 0)
                    message(a, 1, "the ext2 root's check found a fault");
                p++;
            }
        }
        expect(a,
               "f=$(mktemp /root/accept.XXXXXX) && trap 'rm -f \"$f\"' EXIT && "
               "echo accept > \"$f\" && sync && cat \"$f\"",
               "accept", "the ext2 root takes a file and gives it back",
               "the ext2 root did not take a write");
    } else
        message(a, 0, "the mount table identifies a krffs root; no ext2 root row");
    free(slots);
}

/** Check a tool's end independently of its output. */
static int image_tool(struct acceptance *a, char *const argv[], const char *what)
{
    struct etude_termination end;
    char output[180];
    enum etude_status st;
    snprintf(output, sizeof(output), "%s.%s-%u", a->prefix, what, ++a->commands);
    st = etude_host(a->u->c, argv, output, &end);
    return st == ETUDE_OK && end.end == ETUDE_END_EXITED && end.value == 0;
}

/** Make a root-owned temporary directory for one filesystem row. */
static int temporary_directory(struct acceptance *a)
{
    char *text = read_command(a, "mktemp -d /tmp/accept.XXXXXX");
    int ok = plain_path(text, "/tmp/accept.") && strlen(text) < sizeof(a->temporary);
    if (ok)
        strcpy(a->temporary, text);
    else
        message(a, 1, "the machine gave no plain temporary directory");
    free(text);
    return ok;
}

static int same_file(const char *one, const char *two);

/** Attach, mount, write, unmount and detach a filesystem in a file, then
 * judge its downloaded bytes with the independent native KRFFS tools. */
static void loop_image(struct acceptance *a)
{
    char *text = read_command(a, "cat /proc/filesystems");
    const char *image;
    char guest[320], after[4096], file[4096], input[4096], name[180], size[32];
    char *fsck[] = {"fsck-krffs", after, "--strict", NULL};
    char *readfile[] = {"readfile-krffs", after, "/accept.txt", file, NULL};
    struct stat before, downloaded;
    int ok;
    if (text[0] == '\0')
        message(a, 1, "the machine gave no file-system list");
    else if (net_find(text, "krffs") == NULL) {
        message(a, 0, "this kernel does not take the krffs type; no image row");
        free(text);
        return;
    }
    free(text);
    image = etude_artifact(a->u->c, "accept-loop-image");
    if (image == NULL || !temporary_directory(a))
        return;
    if (stat(image, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size <= 0) {
        message(a, 1, "the prepared loop image is not a nonempty regular file");
        return;
    }
    snprintf(size, sizeof(size), "%llu", (unsigned long long)before.st_size);
    snprintf(guest, sizeof(guest), "%s/loop.img", a->temporary);
    snprintf(name, sizeof(name), "%s.loop.input", a->prefix);
    if (net_path(a->u, name, input, sizeof(input)) == NULL || !copy_file(image, input) ||
        !same_file(image, input) || net_ssh_attach(a->u, input) != ETUDE_OK ||
        !put_file(a, input, guest)) {
        message(a, 1, "image: the transfer failed");
        return;
    }
    text = read_format(a, "wc -c < %s", guest);
    message(a, strcmp(text, size) != 0, "image: the transfer has %s bytes (expected %s)", text,
            size);
    free(text);
    text = read_format(a, "loopctl attach 0 %s && grep -c '^loop0 ' /proc/loop", guest);
    ok = strcmp(text, "1") == 0;
    message(a, !ok, "%s",
            ok ? "an image on the root is attached to /dev/loop0" : "the image was not attached");
    free(text);
    a->loop = 1;
    snprintf(a->mounted, sizeof(a->mounted), "/mnt/loop");
    expect(a,
           "mkdir -p /mnt/loop && mount -t krffs /dev/loop0 /mnt/loop && cat /mnt/loop/hello.txt",
           "hello from inside an image", "the file system in that file mounts and reads",
           "the file system in that file did not read");
    expect(a, "echo written-on-the-machine > /mnt/loop/accept.txt && cat /mnt/loop/accept.txt",
           "written-on-the-machine", "and takes a write",
           "the file system in that file did not take a write");
    if (expect(a, "umount /mnt/loop && rmdir /mnt/loop && loopctl detach 0 && wc -c < /proc/loop",
               "0", "and unmounts and detaches, leaving no device attached",
               "the unmount or the detach left something behind")) {
        a->mounted[0] = '\0';
        a->loop = 0;
    }
    snprintf(name, sizeof(name), "%s.loop.after", a->prefix);
    if (net_path(a->u, name, after, sizeof(after)) == NULL)
        return;
    snprintf(name, sizeof(name), "%s.loop.accept", a->prefix);
    if (net_path(a->u, name, file, sizeof(file)) == NULL)
        return;
    if (!get_file(a, guest, after) || stat(after, &downloaded) != 0 ||
        downloaded.st_size != before.st_size)
        message(a, 1, "the image the machine wrote could not be downloaded");
    else {
        ok = image_tool(a, fsck, "loop.fsck");
        message(a, !ok, "the host's checker finds the image the machine wrote %s",
                ok ? "clean" : "faulty");
        ok = image_tool(a, readfile, "loop.read");
        text = net_file(file);
        message(a, !ok || strstr(text, "written-on-the-machine") == NULL,
                "with the file the machine wrote inside it");
        free(text);
    }
    text = read_format(a, "rm -rf %s", a->temporary);
    free(text);
    a->temporary[0] = '\0';
}

/** A flush count belongs to the named USB record, not the host clock. */
static long usb_flushes(struct acceptance *a, const char *disk_name)
{
    char *text = read_command(a, "cat /proc/usb 2>/dev/null"), line[RECORD_LINE];
    long count;
    record_line(text, disk_name, line);
    count = net_number_after(line, "flushes ");
    free(text);
    return count;
}

/** Compare two complete host files. */
static int same_file(const char *one, const char *two)
{
    FILE *a = fopen(one, "rb"), *b = fopen(two, "rb");
    unsigned char x[65536], y[65536];
    size_t nx, ny;
    int ok = a != NULL && b != NULL;
    while (ok) {
        nx = fread(x, 1, sizeof(x), a);
        ny = fread(y, 1, sizeof(y), b);
        if (nx != ny || memcmp(x, y, nx) != 0) {
            ok = 0;
            break;
        }
        if (nx == 0) {
            ok = !ferror(a) && !ferror(b);
            break;
        }
    }
    if (a != NULL)
        fclose(a);
    if (b != NULL)
        fclose(b);
    return ok;
}

/** Read a private snapshot of the unmounted FAT volume. The machine still
 * owns its disk and will remount the volume to remove the witness. */
static int stick_copy(struct acceptance *a, const char *source, const char *actual)
{
    char name[180], snapshot[4096];
    char *copy[] = {"mcopy", "-n", "-i", snapshot, (char *)source, (char *)actual, NULL};
    snprintf(name, sizeof(name), "%s.stick.snapshot", a->prefix);
    if (net_path(a->u, name, snapshot, sizeof(snapshot)) == NULL || !a->request->fat_bytes ||
        case_copy_range(a->request->fat_image, a->request->fat_offset, a->request->fat_bytes,
                        snapshot, 0, 1) != 0)
        return 0;
    return image_tool(a, copy, "stick.copy");
}

/** The FAT witness is judged after each successful unmount, and by an
 * independent image tool while the guest volume is unmounted. */
static int stick_witness(struct acceptance *a, const char *node)
{
    char expected[4096], actual[4096], name[180], guest[320], mount[320], source[420];
    char command[1200], *text, *base;
    FILE *f;
    int ok;
    snprintf(name, sizeof(name), "%s.stick.expected", a->prefix);
    if (net_path(a->u, name, expected, sizeof(expected)) == NULL)
        return 0;
    f = fopen(expected, "wb");
    if (f == NULL)
        return 0;
    ok = fprintf(f, "fat acceptance %s\n", a->prefix) > 0;
    if (fclose(f) != 0)
        ok = 0;
    if (!ok || !temporary_directory(a))
        return 0;
    snprintf(guest, sizeof(guest), "%s/expected", a->temporary);
    if (!put_file(a, expected, guest))
        return 0;
    snprintf(mount, sizeof(mount), "%s/mnt", a->temporary);
    snprintf(a->mounted, sizeof(a->mounted), "%s", mount);
    text = read_format(a, "mkdir %s && mount %s %s", mount, node, mount);
    free(text);
    text = read_format(a, "mktemp %s/accept.XXXXXX", mount);
    if (!plain_path(text, mount) || strlen(text) >= sizeof(a->stick_file)) {
        free(text);
        return 0;
    }
    strcpy(a->stick_file, text);
    free(text);
    text = read_format(a, "cp %s %s && cmp %s %s", guest, a->stick_file, guest, a->stick_file);
    free(text);
    if (a->command_failed)
        return 0;
    message(a, 0, "stick: cached bytes match");
    text = read_format(a, "umount %s && mount %s %s && cmp %s %s && umount %s", mount, node, mount,
                       guest, a->stick_file, mount);
    free(text);
    if (a->command_failed)
        return 0;
    a->mounted[0] = '\0';
    message(a, 0, "stick: remount bytes match after successful unmounts");
    if (a->request->fat_image != NULL) {
        snprintf(name, sizeof(name), "%s.stick.host", a->prefix);
        if (net_path(a->u, name, actual, sizeof(actual)) == NULL)
            return 0;
        base = strrchr(a->stick_file, '/');
        snprintf(source, sizeof(source), "::/%s", base != NULL ? base + 1 : a->stick_file);
        if (!stick_copy(a, source, actual) || !same_file(expected, actual)) {
            message(a, 1, "stick: host FAT bytes differ");
            return 0;
        }
        message(a, 0, "stick: host FAT bytes match while unmounted");
    } else
        message(a, 0, "stick: no host FAT image supplied; no offline byte check");
    snprintf(a->mounted, sizeof(a->mounted), "%s", mount);
    snprintf(command, sizeof(command), "mount %s %s && rm %s && umount %s && rmdir %s", node, mount,
             a->stick_file, mount, mount);
    text = read_command(a, command);
    free(text);
    if (a->command_failed)
        return 0;
    a->stick_file[0] = a->mounted[0] = '\0';
    text = read_format(a, "rm -rf %s", a->temporary);
    free(text);
    a->temporary[0] = '\0';
    return !a->command_failed;
}

/** A recovery ISO partition excludes the whole stick from writes. */
static int recovery_stick(const char *disk, unsigned number)
{
    const char *p = disk;
    char line[RECORD_LINE], type[64];
    unsigned n;
    while (p != NULL && *p) {
        record_line(p, "", line);
        if (sscanf(line, "part %u", &n) == 1 && n == number &&
            strcmp(field(line, 6, type, sizeof(type)), "mbr:17") == 0)
            return 1;
        p = strchr(p, '\n');
        if (p != NULL)
            p++;
    }
    return 0;
}

/** Select by identity alone and refuse duplicate identities. */
static int select_stick(struct acceptance *a, const char *usb, char selected[32])
{
    const char *p = usb, *identity, *serial;
    char line[RECORD_LINE], idline[RECORD_LINE], prefix[64], candidate[32];
    unsigned number, i;
    int duplicate = 0;
    selected[0] = '\0';
    while (p != NULL && *p) {
        record_line(p, "", line);
        if (sscanf(line, "disk%u ", &number) == 1 && strncmp(line, "disk", 4) == 0) {
            snprintf(candidate, sizeof(candidate), "disk%u", number);
            snprintf(prefix, sizeof(prefix), "identity %u ", number);
            identity = record_line(a->disk, prefix, idline);
            serial = identity;
            for (i = 0; i < 5; i++) {
                serial += strcspn(serial, " \t");
                serial += strspn(serial, " \t");
            }
            message(a, 0, "stick: %s", line);
            if (recovery_stick(a->disk, number))
                message(a, 0,
                        "stick: /dev/%s (%s) carries a Rach2 recovery ISO in its table and is not "
                        "written",
                        candidate, serial);
            else if (a->request->serial == NULL)
                message(a, 0, "stick: /dev/%s (%s) was not named by the owner and is not written",
                        candidate, serial);
            else if (strcmp(serial, a->request->serial) != 0)
                message(a, 0, "stick: /dev/%s (%s) is not the stick named, %s, and is not written",
                        candidate, serial, a->request->serial);
            else if (!duplicate) {
                if (selected[0] != '\0') {
                    message(
                        a, 1,
                        "stick: /dev/%s and /dev/%s both report the serial %s; neither is written",
                        candidate, selected, serial);
                    duplicate = 1;
                    selected[0] = '\0';
                } else
                    strcpy(selected, candidate);
            }
        }
        p = strchr(p, '\n');
        if (p != NULL)
            p++;
    }
    if (selected[0] == '\0' && a->request->serial != NULL)
        message(a, 1, "stick: no flash drive reports the serial %s that is not a recovery stick",
                a->request->serial);
    return selected[0] != '\0';
}

/** Find the first FAT or exFAT volume with a plain label on this disk. */
static int stick_volume(struct acceptance *a, const char *blkid, const char *disk_name,
                        char node[128], char label[128])
{
    const char *p = blkid, *end, *value, *q;
    char line[RECORD_LINE], prefix[48];
    size_t len;
    snprintf(prefix, sizeof(prefix), "/dev/%s", disk_name);
    node[0] = label[0] = '\0';
    while (p != NULL && *p) {
        record_line(p, "", line);
        if (strncmp(line, prefix, strlen(prefix)) == 0) {
            message(a, 0, "stick: %s", line);
            if (strstr(line, "TYPE=\"iso9660\"") != NULL)
                message(a, 0, "stick: the ISO half of a Rach2 stick is on it and is left alone");
            if (node[0] == '\0' &&
                (strstr(line, "TYPE=\"vfat\"") != NULL || strstr(line, "TYPE=\"exfat\"") != NULL)) {
                end = strchr(line, ':');
                if (end == NULL || (size_t)(end - line) >= 128)
                    break;
                memcpy(node, line, (size_t)(end - line));
                node[end - line] = '\0';
                value = strstr(line, "LABEL=\"");
                if (value != NULL && (end = strchr(value + 7, '"')) != NULL) {
                    len = (size_t)(end - value - 7);
                    if (len >= 128)
                        len = 127;
                    memcpy(label, value + 7, len);
                    label[len] = '\0';
                }
            }
        }
        p = strchr(p, '\n');
        if (p != NULL)
            p++;
    }
    for (q = label; *q; q++)
        if (!isalnum((unsigned char)*q) && *q != '_' && *q != '-') {
            message(a, 1, "stick: the label has a character that cannot be recorded: %s", label);
            return 0;
        }
    if (node[0] == '\0' || !plain_path(node, "/dev/disk")) {
        message(a, 1, "stick: no FAT or exFAT volume with a plain label was found on it");
        return 0;
    }
    return 1;
}

/** Exercise the first unrecognized partition that mounts as KRFFS. */
static void stick_krffs(struct acceptance *a, const char *disk_name, const char *blkid)
{
    char node[128], prefix[160], *text;
    unsigned part;
    long before, after;
    int found = 0;
    for (part = 1; part <= 4; part++) {
        snprintf(node, sizeof(node), "/dev/%sp%u", disk_name, part);
        snprintf(prefix, sizeof(prefix), "%s:", node);
        text = read_format(a, "if [ -b %s ]; then echo PRESENT; fi", node);
        if (strcmp(text, "PRESENT") == 0 && strstr(blkid, prefix) == NULL) {
            free(text);
            text = read_format(
                a, "mkdir -p /mnt/k || exit 1; if mount -t krffs %s /mnt/k; then echo MOUNTED; fi",
                node);
            if (strcmp(text, "MOUNTED") == 0)
                found = 1;
        }
        free(text);
        if (found)
            break;
    }
    if (!found) {
        message(a, 0, "stick: no KRFFS volume on it (no partition of it mounts as krffs)");
        return;
    }
    strcpy(a->mounted, "/mnt/k");
    before = usb_flushes(a, disk_name);
    text = read_command(a, "cat /mnt/k/hello.txt; echo written-on-the-stick > /mnt/k/accept.txt; "
                           "cat /mnt/k/accept.txt");
    message(a, strcmp(text, "hello from inside an imagewritten-on-the-stick") != 0,
            "stick: the KRFFS volume on %s mounts, reads and takes a write", node);
    free(text);
    after = usb_flushes(a, disk_name);
    message(a, before < 0 || after <= before,
            "stick: writing it put the device's own cache on the medium (%ld to %ld)", before,
            after);
    if (expect(a, "umount /mnt/k && rmdir /mnt/k && echo UNMOUNTED", "UNMOUNTED",
               "stick: and it unmounts", "stick: the KRFFS volume did not unmount"))
        a->mounted[0] = '\0';
}

/** Exercise only the stick whose serial was named, preserving every other
 * disk and every recovery stick. */
static void sticks(struct acceptance *a)
{
    char *usb = read_command(a, "cat /proc/usb 2>/dev/null");
    char selected[32], node[128], label[128], *text, *blkid;
    long before, after;
    if (strstr(usb, "disk") == NULL)
        message(a, 0, "no flash drive is plugged in (/proc/usb names no usb disk)");
    free(a->disk);
    a->disk = read_command(a, "cat /proc/disk 2>/dev/null || :");
    if (!select_stick(a, usb, selected)) {
        free(usb);
        return;
    }
    free(usb);
    message(a, 0, "stick: /dev/%s (%s) is the one written", selected, a->request->serial);
    text = read_format(a, "ls -l /dev/%s 2>/dev/null", selected);
    message(a, text[0] != 'b', "stick: /dev/%s is %sa block device", selected,
            text[0] == 'b' ? "" : "not ");
    free(text);
    blkid = read_command(a, "blkid 2>/dev/null");
    if (stick_volume(a, blkid, selected, node, label)) {
        before = usb_flushes(a, selected);
        if (stick_witness(a, node))
            message(a, 0, "stick: the volume %s on %s mounts, writes and reads after remount",
                    label, node);
        else
            message(a, 1, "stick: the volume %s failed its persistence check", label);
        after = usb_flushes(a, selected);
        message(a, before < 0 || after <= before,
                "stick: flushes during writes and unmounts (%ld to %ld)", before, after);
    }
    stick_krffs(a, selected, blkid);
    free(blkid);
}

/** Record hub topology and the translator and SuperSpeed conditions. */
static void hubs(struct acceptance *a)
{
    char *usb = read_command(a, "cat /proc/usb 2>/dev/null");
    int seen = net_find(a->bootlog, "usb: device: *, a hub") != NULL;
    int hub = strstr(usb, "class 9 ") != NULL;
    if (strstr(a->bootlog, "usb: hub:") == NULL && !seen && !hub)
        message(a, 0, "no hub is plugged in");
    else if (seen && !hub)
        message(a, 1, "a hub was seen and then went away");
    else {
        lines(a, a->bootlog, "usb: hub:", "");
        lines(a, usb, "class 9 ", "hub: ");
        if (net_find(usb, "port #.#") != NULL)
            lines(a, usb, "port ", "behind a hub: ");
        else
            message(a, 1, "a hub is there and nothing behind it came up");
        message(a, 0, "hub: %s",
                net_find(usb, "port #.#* low ") != NULL
                    ? "a low-speed device behind a hub is driven, which is the translator's case"
                    : "no low-speed device behind a hub; the translator is untested on this run");
        if (strstr(a->bootlog, "refused a translator per port") != NULL)
            message(a, 0, "hub: one translator per port was refused; one is used");
        else if (strstr(a->bootlog, "think time") != NULL)
            message(a, 0,
                    "hub: the think time is recorded; one translator per port is selected when "
                    "offered");
        if (strstr(a->bootlog, "SuperSpeed hub") != NULL) {
            message(a, 0,
                    "hub: a SuperSpeed hub was refused by name; its high-speed half is driven");
            message(a, net_find(usb, "port #.#* super ") != NULL,
                    "hub: no SuperSpeed device appeared behind it");
        }
    }
    free(usb);
}

/** Release guest objects in the same order as the native acceptance. */
static void cleanup(struct acceptance *a)
{
    char *text;
    if (a->mounted[0] != '\0') {
        if (a->stick_file[0] != '\0') {
            text = read_format(a, "rm -f %s", a->stick_file);
            free(text);
        }
        text = read_format(a, "umount %s && rmdir %s && echo CLEANED", a->mounted, a->mounted);
        if (strcmp(text, "CLEANED") == 0)
            a->mounted[0] = '\0';
        free(text);
    }
    if (a->loop) {
        text = read_command(a, "loopctl detach 0");
        free(text);
    }
    if (a->temporary[0] != '\0' && a->mounted[0] == '\0') {
        text = read_format(a, "rm -rf %s", a->temporary);
        free(text);
    }
    transfer_end(a);
    free(a->cmdline);
    free(a->mounts);
    free(a->disk);
    free(a->bootlog);
}

enum etude_status metal_accept_run(struct net_case *u, const struct metal_accept_request *request,
                                   const char *output, struct net_run *r)
{
    struct acceptance a;
    char path[4096];
    int passed;
    memset(&a, 0, sizeof(a));
    memset(r, 0, sizeof(*r));
    r->end.end = ETUDE_END_LOST;
    r->end.value = -1;
    a.u = u;
    a.request = request;
    if (!net_ok(u) || strlen(output) >= sizeof(a.prefix) ||
        net_path(u, output, path, sizeof(path)) == NULL)
        return ETUDE_REFUSED;
    strcpy(a.prefix, output);
    a.log = fopen(path, "wb");
    if (a.log == NULL) {
        net_fail(u, "the acceptance log could not be created");
        return ETUDE_FAILED;
    }
    fprintf(a.log, "metal-accept: 127.0.0.1:%s\n", request->login.port);
    if (root(&a)) {
        disk(&a);
        inventory(&a);
        partitions(&a);
        loop_image(&a);
        sticks(&a);
        hubs(&a);
    }
    cleanup(&a);
    if (a.command_failed)
        message(&a, 1, "a required remote command failed; its output is not acceptance evidence");
    passed = a.fails == 0 && net_ok(u);
    if (passed)
        fprintf(a.log, "metal-accept: PASS\n");
    else
        fprintf(a.log, "metal-accept: FAIL (%u)\n", a.fails);
    if (fclose(a.log) != 0)
        passed = 0;
    r->out = net_file(path);
    r->err = empty_text();
    r->end.end = ETUDE_END_EXITED;
    r->end.value = passed ? 0 : 1;
    return ETUDE_OK;
}
