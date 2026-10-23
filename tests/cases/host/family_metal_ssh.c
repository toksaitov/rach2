/* family_metal_ssh.c: exact acceptance command replies and stick mutations. */
#define _POSIX_C_SOURCE 200809L
#include "family_metal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int starts(const char *s, const char *prefix)
{
    return !strncmp(s, prefix, strlen(prefix));
}
static int reply(const char *text)
{
    fputs(text, stdout);
    return 0;
}
static int sticks(const char *mode)
{
    return !strcmp(mode, "disk-prefix") || starts(mode, "fat-");
}
static int usb(const char *w, const char *mode)
{
    unsigned i;
    if (sticks(mode))
        for (i = 0; i < 2; i++) {
            char path[8192], before[8192];
            const char *disk = i ? "disk10" : "disk1";
            snprintf(path, sizeof(path), "%s/%s.img", w, disk);
            snprintf(before, sizeof(before), "%s/%s.before", w, disk);
            printf("%s device 1 flushes %d\n", disk, !fx_same(path, before));
        }
    return 0;
}
static int stick_command(const char *c, const char *w, const char *mode)
{
    char path[8192];
    if (starts(c, "mkdir /tmp/accept.fixture/mnt && mount ") || starts(c, "mount /dev/disk")) {
        const char *node = strstr(c, "mount ") + 6;
        const char *disk = starts(node, "/dev/disk1p1 ")    ? "disk1"
                           : starts(node, "/dev/disk10p1 ") ? "disk10"
                                                            : NULL;
        snprintf(path, sizeof(path), "%s/selected", w);
        return disk && fx_text(path, disk) ? 0 : 99;
    }
    if (starts(c, "mktemp /tmp/accept.fixture/mnt/accept."))
        return reply("/tmp/accept.fixture/mnt/accept.fixture\n");
    if (starts(c, "cp /tmp/accept.fixture/expected ")) {
        char *disk;
        FILE *fp;
        snprintf(path, sizeof(path), "%s/selected", w);
        disk = fx_read(path);
        if (strcmp(disk, "disk1") && strcmp(disk, "disk10")) {
            free(disk);
            return 99;
        }
        snprintf(path, sizeof(path), "%s/%s.img", w, disk);
        free(disk);
        fp = fopen(path, "a");
        if (!fp)
            return 1;
        fputs("accept\n", fp);
        return fclose(fp) ? 1 : 0;
    }
    if (starts(c, "cmp /tmp/accept.fixture/expected "))
        return !strcmp(mode, "fat-remount");
    if (starts(c, "umount /tmp/accept.fixture/mnt"))
        return !strcmp(mode, "fat-busy");
    if (starts(c, "rm -f /tmp/accept.fixture/mnt/accept.") ||
        starts(c, "rm /tmp/accept.fixture/mnt/accept.") || starts(c, "if [ -b /dev/disk1p"))
        return 0;
    return -1;
}
static int disk_command(const char *c, const char *w, const char *mode)
{
    if (starts(c, "cat /proc/disk")) {
        reply("policy read-only\ndisk 0 usb 0 1000 mbr fixture\nidentity 0 512 removable free "
              "FIXTURE\n");
        if (sticks(mode))
            reply("disk 1 usb 0 1000 mbr approved\nidentity 1 512 removable free APPROVED01\ndisk "
                  "10 usb 0 1000 mbr unapproved\nidentity 10 512 removable free UNAPPROVED10\n");
        return 0;
    }
    if (starts(c, "if [ -b /dev/disk0 ]"))
        return reply("published\n");
    if (strstr(c, "accept-mbr.")) {
        printf("%064d  /tmp/sector\n", 0);
        return !strcmp(mode, "hash-fail");
    }
    if (starts(c, "cat /proc/usb"))
        return usb(w, mode);
    if (!strcmp(c, "ls -l /dev/disk1 2>/dev/null"))
        return reply("brw------- disk1\n");
    if (!strcmp(c, "blkid 2>/dev/null"))
        return reply("/dev/disk10p1: LABEL=\"TESTVOL\" TYPE=\"vfat\"\n/dev/disk1p1: "
                     "LABEL=\"TESTVOL\" TYPE=\"vfat\"\n");
    return stick_command(c, w, mode);
}
static int transfer(const char *c, const char *w, const char *mode)
{
    if (starts(c, "mktemp -d /tmp/accept-transfer."))
        return reply("/tmp/accept-transfer.fixture\n");
    if (starts(c, "mktemp -d /tmp/accept."))
        return !strcmp(mode, "tmp-fail")    ? 7
               : !strcmp(mode, "tmp-empty") ? 0
                                            : reply("/tmp/accept.fixture\n");
    if (starts(c, "cat > ")) {
        char path[8192], data[32768];
        size_t n;
        FILE *fp;
        snprintf(path, sizeof(path), "%s/payload", w);
        fp = fopen(path, "wb");
        if (!fp)
            return 1;
        while ((n = fread(data, 1, sizeof(data), stdin)))
            if (fwrite(data, 1, n, fp) != n) {
                fclose(fp);
                return 1;
            }
        fclose(fp);
        return !strcmp(mode, "put-fail");
    }
    if (starts(c, "wc -c < "))
        return reply("1048576\n");
    if (starts(c, "loopctl attach "))
        return reply("1\n");
    if (starts(c, "mkdir -p /mnt/loop "))
        return reply("hello from inside an image");
    if (starts(c, "echo written-on-the-machine "))
        return reply("written-on-the-machine\n");
    if (starts(c, "umount /mnt/loop "))
        return reply("0\n");
    if (!strcmp(c, "cat /tmp/accept.fixture/loop.img") ||
        !strcmp(c, "cat /tmp/accept-transfer.fixture/data"))
        return reply("image\n");
    if (starts(c, "mv /tmp/accept-") || starts(c, "mv /tmp/accept.") ||
        starts(c, "if [ -f /tmp/accept-transfer.") || starts(c, "rm -rf ") ||
        starts(c, "df -k /tmp;"))
        return 0;
    return -1;
}
int family_metal_ssh(const char *c)
{
    const char *mode = getenv("ACCEPT_CASE"), *w = getenv("W");
    char path[8192];
    FILE *fp;
    int result;
    if (!mode || !w)
        return 99;
    snprintf(path, sizeof(path), "%s/calls", w);
    fp = fopen(path, "a");
    if (!fp)
        return 99;
    fprintf(fp, "%s\n", c);
    fclose(fp);
    if ((result = disk_command(c, w, mode)) >= 0 || (result = transfer(c, w, mode)) >= 0)
        return result;
    if (!strcmp(c, "echo ALIVE"))
        return reply("ALIVE\n");
    if (!strcmp(c, "id -u"))
        return reply("0\n");
    if (!strcmp(c, "cat /proc/cmdline"))
        return !strcmp(mode, "cmdline-fail") ? 7 : reply("rach2.ip=none\n");
    if (!strcmp(c, "cat /proc/mounts"))
        return reply(starts(mode, "live-") ? "/dev/disk0p3 / ext2 rw,relatime\n"
                                           : "none / krffs rw\n");
    if (!strcmp(c, "uname -a")) {
        reply("Rach2\n");
        return !strcmp(mode, "uname-fail");
    }
    if (!strcmp(c, "id"))
        return reply("uid=0(root)\n");
    if (starts(c, "cat /proc/uptime"))
        return reply("1.00 0.00\n");
    if (starts(c, "dd if=/dev/urandom"))
        return reply("16\n");
    if (!strcmp(c, "cat /dev/bootlog")) {
        reply("boot: Rach2 Booting\nusb: controller: none\nlapic: ready: fixture\n");
        if (!strcmp(mode, "live-fault"))
            reply("ext2: check: 1 lost blocks, 1 orphan inodes, kept for e2fsck\n");
        if (starts(mode, "live-"))
            reply("ext2: check: clean\n");
        return 0;
    }
    if (starts(c, "cat /proc/interrupts"))
        return reply("32:\t10\t0\tpit\n");
    if (starts(c, "ls /dev"))
        return reply("disk0\n");
    if (!strcmp(c, "bootctl status 2>/dev/null || :"))
        return 0;
    if (!strcmp(c, "bootctl status"))
        return reply("booted: candidate\n");
    if (strstr(c, "mktemp /root/accept."))
        return reply("accept\n");
    if (!strcmp(c, "cat /proc/filesystems"))
        return reply("krffs\n");
    fprintf(stderr, "accept-fixture: unexpected command: %s\n", c);
    return 99;
}
