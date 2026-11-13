/*
 * installer_repair_resume.c: the scenario of machine.installer-repair.resume:
 * updates stopped after the root's write, by a stop planted on the disk or
 * made by a copy of the installer that exits there, are run again and give
 * the machine back its accounts, homes and keys.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib_installer_repair.h"
#include "lib_installer_repair_disk.h"

/* The name the stick's volume gives the root image with the keys added. */
#define KEY_ROOT_NAME "key-root.ext2"

/* The assertions of the case, in the order the rows report them. */
static const char *const assertions[] = {
    "keys-recorded",     "payload",           "key-names",          "home-inside-root",
    "half-slot-ignored", "outside-home-keys", "outside-home-empty", "archived",
    "legacy-planted",    "legacy-archive",    "slot-planted",       "stopped-update",
    "restored-keys",     "restored-login",    "finished",           "key-hashes",
    "retry-finished",    "retry-stopped",     "retry-restored",     NULL};

/* What the rows share: the attempt's files and what earlier rows found. */
struct resume {
    struct etude_case *c;
    char password[128];
    char demo[4096];   /* root's demo key, a copy only its owner reads */
    char pubkey[512];  /* its public half */
    char keys[3][512]; /* the public keys of worker, Alice and alice */
    char disk[4096];   /* the installed disk */
    char stick[4096];  /* the test stick */
    char root[4096];   /* the root image with keys in homes outside /home */
    uint64_t rec;      /* the first sector of the disk's recovery partition */
    uint64_t rootat;   /* the first sector of its root partition */
    char *before;      /* the key digests the machine had before the stops */
    char why[300];     /* why the rows stopped, once one the rest need failed */
};

/**
 * Report as failed every assertion from `from` on, since the row that the
 * rest need failed: the rows of the suite stopped there as well.
 */
static void rest(struct resume *r, const char *from)
{
    char detail[400];
    int i, on = 0;

    snprintf(detail, sizeof(detail), "not reached: %s", r->why);
    for (i = 0; assertions[i] != NULL; i++) {
        if (strcmp(assertions[i], from) == 0)
            on = 1;
        if (on)
            repair_assert(r->c, assertions[i], 0, detail);
    }
}

/** Give the lines of `text` that are a key's digest, `KEYHASH:` and a hex
 * digit first, each with a newline; the caller frees them. */
static char *key_digests(const char *text)
{
    const char *line = text, *eol;
    size_t n, len = 0;
    char *out = calloc(1, text != NULL ? strlen(text) + 1 : 1);

    while (out != NULL && line != NULL && *line != '\0') {
        eol = strchr(line, '\n');
        n = eol != NULL ? (size_t)(eol - line) : strlen(line);
        if (n > 8 && strncmp(line, "KEYHASH:", 8) == 0 && strchr("0123456789abcdef", line[8])) {
            memcpy(out + len, line, n);
            len += n;
            out[len++] = '\n';
        }
        line = eol != NULL ? eol + 1 : NULL;
    }
    return out;
}

/** Give the console of boot `d` once it has ended or been judged, or NULL. */
static char *console_of(const struct drive *d)
{
    return d->boot > 0 ? drive_console(d) : NULL;
}

/**
 * Make the keys the rows give the accounts: the public half of root's demo
 * key, and a new key for each of three accounts, as the suite made them
 * with ssh-keygen.
 */
static int make_keys(struct resume *r)
{
    static const char *const accounts[] = {"worker", "upper", "lower"};
    char path[4096], pub[4200], *text = NULL;
    int i;

    if (repair_path(r->c, "demo-key", r->demo, sizeof(r->demo)) != 0 ||
        repair_copy(etude_artifact(r->c, "demo-key"), r->demo, 0, 0600) != 0) {
        snprintf(r->why, sizeof(r->why), "root's demo key could not be copied");
        return -1;
    }
    if (repair_run(r->c, (char *const[]){"ssh-keygen", "-y", "-f", r->demo, NULL}, "pubkey",
                   &text) != 0 ||
        text == NULL || sscanf(text, "%511[^\n]", r->pubkey) != 1) {
        snprintf(r->why, sizeof(r->why), "ssh-keygen gave no public key: %.200s",
                 text != NULL ? text : "");
        free(text);
        return -1;
    }
    free(text);
    for (i = 0; i < 3; i++) {
        if (repair_path(r->c, accounts[i], path, sizeof(path)) != 0 ||
            (size_t)snprintf(pub, sizeof(pub), "%s.pub", path) >= sizeof(pub) ||
            repair_run(r->c,
                       (char *const[]){"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C",
                                       (char *)accounts[i], "-f", path, NULL},
                       accounts[i], NULL) != 0 ||
            repair_read_line(pub, r->keys[i], sizeof(r->keys[i])) != 0) {
            snprintf(r->why, sizeof(r->why), "ssh-keygen made no key for %s", accounts[i]);
            return -1;
        }
    }
    return 0;
}

/**
 * Record the installed machine's keys, with keys of its own for root and the
 * test account, a host key for each server, and accounts whose homes have
 * keys inside and outside /home, and keep their digests.
 */
static void record(struct resume *r)
{
    struct drive d;
    char *text = NULL;
    int lines = 0;
    const char *p;

    repair_login(&d, r->c, "installed", r->password);
    drive_send(&d, "mkdir -p /root/.ssh && echo '%s' > /root/.ssh/authorized_keys", r->pubkey);
    drive_send(&d, "%s",
               "(test -s /etc/dropbear/dropbear_ed25519_host_key || dropbearmulti dropbearkey -t "
               "ed25519 -f /etc/dropbear/dropbear_ed25519_host_key) && echo HOST\"\"READY");
    drive_expect(&d, "HOSTREADY");
    drive_send(&d, "%s",
               "mkdir -p /etc/ssh && echo 'openssh host key' > /etc/ssh/ssh_host_ed25519_key && "
               "chmod 600 /etc/ssh/ssh_host_ed25519_key");
    drive_send(&d, "%s",
               "echo '# machine root' >> /root/.ssh/authorized_keys; echo '# machine test' >> "
               "/home/test/.ssh/authorized_keys");
    drive_send(&d, "%s",
               "chmod 700 /root/.ssh /home/test/.ssh; chmod 600 /root/.ssh/authorized_keys "
               "/home/test/.ssh/authorized_keys");
    drive_send(&d, "%s", "chown -R 0:0 /root/.ssh; chown -R 1000:1000 /home/test/.ssh");
    drive_send(&d, "%s",
               "echo worker:x:2001:2001::/home/work:/bin/sh >> /etc/passwd; echo "
               "Alice:x:2002:2002::/home/Alice:/bin/sh >> /etc/passwd");
    drive_send(&d, "%s",
               "echo alice:x:2003:2003::/home/alice:/bin/sh >> /etc/passwd; mkdir -p "
               "/home/work/.ssh /home/Alice/.ssh /home/alice/.ssh");
    drive_send(&d, "echo '%s' > /home/work/.ssh/authorized_keys", r->keys[0]);
    drive_send(&d, "echo '%s' > /home/Alice/.ssh/authorized_keys", r->keys[1]);
    drive_send(&d, "echo '%s' > /home/alice/.ssh/authorized_keys", r->keys[2]);
    drive_send(&d, "%s",
               "for a in work:2001 Alice:2002 alice:2003; do h=/home/${a%:*}; "
               "chown -R ${a#*:}:${a#*:} $h; chmod 700 $h $h/.ssh; "
               "chmod 600 $h/.ssh/authorized_keys; done");
    drive_send(&d, "%s",
               "echo service:x:2004:2004::/srv/alice:/bin/sh >> /etc/passwd; echo "
               "empty:x:2005:2005::/srv/empty:/bin/sh >> /etc/passwd");
    drive_send(&d, "mkdir -p /srv/alice/.ssh; echo '%s' > /srv/alice/.ssh/authorized_keys",
               r->keys[0]);
    drive_send(&d, "%s",
               "chown -R 2004:2004 /srv/alice; chmod 750 /srv/alice/.ssh; chmod 640 "
               "/srv/alice/.ssh/authorized_keys");
    drive_send(&d, "%s",
               "sha256sum /etc/dropbear/* /etc/ssh/ssh_host_* /root/.ssh/authorized_keys "
               "/home/test/.ssh/authorized_keys | sed s/^/KEYHASH:/");
    drive_send(&d, "%s",
               "sha256sum /home/work/.ssh/authorized_keys /home/Alice/.ssh/authorized_keys "
               "/home/alice/.ssh/authorized_keys /srv/alice/.ssh/authorized_keys | sed "
               "s/^/KEYHASH:/");
    drive_send(&d, "%s", "sync; echo KEYS\"\"SAVED");
    drive_expect(&d, "KEYSSAVED");
    if (!d.failed && (text = drive_console(&d)) != NULL)
        r->before = key_digests(text);
    free(text);
    drive_stop(&d);
    for (p = r->before; p != NULL && (p = strchr(p, '\n')) != NULL; p++)
        lines++;
    if (d.failed)
        snprintf(r->why, sizeof(r->why), "recording the machine's keys: %s", d.why);
    else if (lines < 6)
        snprintf(r->why, sizeof(r->why), "the machine's key digests are missing: %d lines", lines);
}

/**
 * Put keys in two homes outside /home of a copy of the root image, one of an
 * account whose machine configuration has no keys, and make it the one file
 * of the stick's volume, as the suite did with debugfs and mkexfatfixture.
 */
static void payload(struct resume *r)
{
    char work[4096], vol[4200], add[4200], sectors[32], offset[32], *out = NULL;
    unsigned char e[8];
    uint64_t start = 0, size = 0;
    const char *p;
    FILE *f;
    int i;
    struct repair_disk_request keys = {"root-keys", r->root, NULL, NULL, 0, 0};

    if ((p = etude_private_copy(r->c, "root")) != NULL)
        snprintf(r->root, sizeof(r->root), "%s", p);
    if ((p = etude_private_copy(r->c, "stick")) != NULL)
        snprintf(r->stick, sizeof(r->stick), "%s", p);
    if (r->root[0] == '\0' || r->stick[0] == '\0' ||
        repair_path(r->c, "", work, sizeof(work)) != 0) {
        snprintf(r->why, sizeof(r->why), "the root image or the stick could not be copied");
        return;
    }
    if (repair_disk(r->c, &keys, "root-keys", &out) != 0) {
        snprintf(r->why, sizeof(r->why), "%.290s", out != NULL ? out : "the root image's keys");
        free(out);
        return;
    }
    free(out);
    out = NULL;
    if ((f = fopen(r->stick, "rb")) != NULL) {
        if (fseek(f, 446 + 16 + 8, SEEK_SET) == 0 && fread(e, 1, 8, f) == 8)
            for (i = 3; i >= 0; i--) {
                start = start << 8 | e[i];
                size = size << 8 | e[4 + i];
            }
        fclose(f);
    }
    snprintf(vol, sizeof(vol), "%s/key-source.vol", work);
    snprintf(add, sizeof(add), "%s=%s", KEY_ROOT_NAME, r->root);
    snprintf(sectors, sizeof(sectors), "%llu", (unsigned long long)size);
    snprintf(offset, sizeof(offset), "%llu", (unsigned long long)start);
    if (start == 0 || size == 0 ||
        repair_run(r->c,
                   (char *const[]){"mkexfatfixture", "--make", vol, "--sectors", sectors,
                                   "--offset", offset, "--add", add, NULL},
                   "key-source", &out) != 0 ||
        repair_copy(vol, r->stick, start * 512, 0) != 0)
        snprintf(r->why, sizeof(r->why), "the stick's volume with the keyed root: %.200s",
                 out != NULL ? out : "the stick's table");
    free(out);
}

/**
 * Put key names in /srv/alice/.ssh of the machine's root that FAT32 would
 * fold to one case or match to a short alias, and a symbolic link among them,
 * and an account whose home is `/../../tmp/escape`, with a .ssh of the same
 * path in the recovery system's own /tmp for the update to leave alone.
 */
static void plant_key_names(struct drive *d)
{
    drive_send(d, "%s",
               "mkdir -p /r /tmp/escape/.ssh; echo outside > /tmp/escape/.ssh/marker; mount -t "
               "ext2 /dev/disk0p3 /r && cd /r/srv/alice/.ssh && cp -p authorized_keys "
               "AUTHORIZED_KEYS && echo long > ABCDEFGHI && echo alias > 'ABCDEF~1' && ln -s "
               "authorized_keys link; cd /");
    drive_send(d, "%s",
               "echo escape:x:2006:2006::/../../tmp/escape:/bin/sh >> /r/etc/passwd; umount /r; "
               "echo NAMES\"\"PLANTED");
    drive_expect(d, "NAMESPLANTED");
}

/**
 * Print NAMESKEPT=, NAMESSTORED= and ESCAPEKEPT=, each 0 when the names and the
 * link that plant_key_names put in /srv/alice/.ssh are on the root, are in the
 * archive's keys/2004.tar, and the recovery system's /tmp/escape/.ssh is
 * still there. The archive is unpacked in the tmpfs at /dev/shm, as
 * rach2-install restores it, since the recovery root takes no links.
 */
static void check_key_names(struct drive *d)
{
    drive_send(d, "%s",
               "mount -t ext2 /dev/disk0p3 /r && cd /r/srv/alice/.ssh && cmp -s authorized_keys "
               "AUTHORIZED_KEYS && grep -qx long ABCDEFGHI && grep -qx alias 'ABCDEF~1' && test "
               "\"$(readlink link)\" = authorized_keys; echo NAMES\"\"KEPT=$?; cd /; umount /r");
    drive_send(d, "%s",
               "mkdir -p /a /dev/shm/stored && mount -t vfat /dev/disk0p2 /a && tar -xf "
               "/a/rach2-config-a/keys/2004.tar -C /dev/shm/stored && cd /dev/shm/stored && cmp -s "
               "authorized_keys AUTHORIZED_KEYS && grep -qx long ABCDEFGHI && grep -qx alias "
               "'ABCDEF~1' && test \"$(readlink link)\" = authorized_keys; echo "
               "NAMES\"\"STORED=$?; cd /; umount /a");
    drive_send(d, "%s",
               "test -f /tmp/escape/.ssh/marker; echo ESCAPE\"\"KEPT=$?; echo STORE\"\"DONE");
    drive_expect(d, "STOREDONE");
}

/**
 * Archive the machine's configuration by an update over a half-written slot A
 * that the update must pass by. A home outside /home holds key names that
 * FAT32 would fold or match to a short alias and a symbolic link, which the
 * root and the archive's tar file must both keep, and an account's home of
 * `/../../tmp/escape` must be read inside the root and not reach the
 * recovery system's /tmp.
 */
static void archive_keys(struct resume *r)
{
    char update[32] = "", alice[32] = "", empty[32] = "", names[32] = "", stored[32] = "";
    char escape[32] = "";
    char line[64];
    char *text = NULL;
    struct drive d;
    int kept, inside, half, outside, none;

    repair_recovery(&d, r->c, "recovery");
    drive_send(&d, "%s",
               "mkdir -p /r /a; mount -t ext2 /dev/disk0p3 /r && mount -t vfat /dev/disk0p2 /a");
    drive_send(&d, "%s",
               "rm -rf /a/rach2-config /a/rach2-config.new /a/rach2-config.old /a/rach2-config-a "
               "/a/rach2-config-b");
    drive_send(&d, "%s",
               "mkdir -p /a/rach2-config-a/etc && cp /r/etc/passwd /r/etc/group /r/etc/shadow "
               "/a/rach2-config-a/etc/");
    drive_send(&d, "%s",
               "test -f /a/rach2-config-a/etc/passwd && test -f /a/rach2-config-a/etc/group && "
               "test -f /a/rach2-config-a/etc/shadow && test ! -e /a/rach2-config-a/generation "
               "&& echo HALF\"\"READY");
    drive_expect(&d, "HALFREADY");
    drive_send(&d, "%s",
               "echo stopped > /a/rach2-config-a/update-in-progress && umount /a && umount /r && "
               "echo HALF\"\"PLANTED");
    drive_expect(&d, "HALFPLANTED");
    plant_key_names(&d);
    drive_send(&d, "rach2-install --update /m/%s /dev/disk0; echo UPDATE\"\"STATUS=$?",
               KEY_ROOT_NAME);
    drive_expect(&d, "UPDATESTATUS=");
    drive_send(&d, "%s",
               "mount -t ext2 /dev/disk0p3 /r; stat -c STAT=%u:%g:%a /r/srv/alice/.ssh "
               "/r/srv/alice/.ssh/authorized_keys");
    drive_send(&d,
               "grep -qxF '%s' /r/srv/alice/.ssh/authorized_keys; echo ALICE\"\"KEY=$?; test -e "
               "/r/srv/empty/.ssh; echo EMPTY\"\"SSH=$?; umount /r; echo CHECKS\"\"DONE",
               r->keys[0]);
    drive_expect(&d, "CHECKSDONE");
    check_key_names(&d);
    drive_send(&d, "%s", "umount /m; echo KEYS\"\"ARCHIVED");
    drive_expect(&d, "KEYSARCHIVED");
    text = console_of(&d);
    drive_stop(&d);
    if (drive_halted()) {
        free(text);
        return;
    }
    repair_last_field(text, "UPDATESTATUS=", update, sizeof(update));
    repair_last_field(text, "NAMESKEPT=", names, sizeof(names));
    repair_last_field(text, "NAMESSTORED=", stored, sizeof(stored));
    repair_last_field(text, "ESCAPEKEPT=", escape, sizeof(escape));
    repair_last_field(text, "ALICEKEY=", alice, sizeof(alice));
    repair_last_field(text, "EMPTYSSH=", empty, sizeof(empty));
    kept = !d.failed && strcmp(names, "0") == 0 && strcmp(stored, "0") == 0;
    inside = !d.failed && strcmp(escape, "0") == 0;
    half = !d.failed && strcmp(update, "0") == 0 && text != NULL &&
           strstr(text, "copying this machine's accounts") != NULL &&
           strstr(text, "taking the configuration saved") == NULL;
    outside = !d.failed && text_field(text, "STAT=2004:2004:750", 1, line, sizeof(line)) == 0 &&
              text_field(text, "STAT=2004:2004:640", 1, line, sizeof(line)) == 0 &&
              strcmp(alice, "0") == 0;
    none = !d.failed && strcmp(empty, "1") == 0;
    repair_assert(r->c, "key-names", kept,
                  d.failed ? d.why
                           : "key names that differ in case or match a short alias, or a link, "
                             "were not kept on the root and in the archive's tar file");
    repair_assert(r->c, "home-inside-root", inside,
                  d.failed ? d.why
                           : "a home of /../../tmp/escape reached the recovery system's /tmp");
    repair_assert(r->c, "half-slot-ignored", half,
                  d.failed ? d.why : "the update took a half-written slot or did not finish");
    repair_assert(r->c, "outside-home-keys", outside,
                  d.failed ? d.why : "the keys of a home outside /home were not kept");
    repair_assert(r->c, "outside-home-empty", none,
                  d.failed ? d.why : "an image key survived in a home outside /home");
    if (d.failed)
        snprintf(r->why, sizeof(r->why), "the normal update's boot did not finish: %s", d.why);
    else if (!kept || !inside || !half || !outside || !none)
        snprintf(r->why, sizeof(r->why),
                 "archiving the machine's configuration before the interrupted update");
    repair_assert(r->c, "archived", r->why[0] == '\0', r->why);
    free(text);
}

/**
 * Run the compiled fixture's MODE on the installed disk: legacy or slot,
 * with the partitions' first sectors and the keyed root image.
 */
static int plant(struct resume *r, const char *mode)
{
    struct repair_disk_request request = {mode, r->disk, KEY_ROOT_NAME, r->pubkey, r->rec, 0};
    char *out = NULL;
    int rc;

    if (repair_copy(r->root, r->disk, r->rootat * 512, 0) != 0) {
        snprintf(r->why, sizeof(r->why), "the image could not be written onto the root");
        return -1;
    }
    rc = repair_disk(r->c, &request, mode, &out);
    if (rc != 0)
        snprintf(r->why, sizeof(r->why), "%.290s",
                 out != NULL && out[0] != '\0' ? out : "the disk fixture did not finish");
    free(out);
    return rc == 0 ? 0 : -1;
}

/**
 * Retry the update after a copy of the installer stopped itself just before
 * it wrote the new archive: the old layout's archive must be left whole, and
 * the retry must take it and leave only the new slots.
 */
static void archive_stop(struct resume *r)
{
    char line[64];
    char *text = NULL;
    struct drive d;
    int ok;

    repair_recovery(&d, r->c, "recovery");
    drive_send(&d, "%s",
               "mkdir -p /r; mount -t vfat /dev/disk0p2 /r; cp -R /r/rach2-config "
               "/dev/shm/legacy-before; umount /r");
    drive_send(&d, "%s",
               "sed 's|^    mkdir -p \"$next_archive/etc/rach2\" |    sync; exit 99; mkdir -p "
               "\"$next_archive/etc/rach2\" |' /bin/rach2-install > /dev/shm/stop-install");
    drive_send(&d, "sh /dev/shm/stop-install --update /m/%s /dev/disk0; echo STOP_STATUS=$?",
               KEY_ROOT_NAME);
    drive_expect(&d, "STOP_STATUS=99");
    drive_send(&d, "%s",
               "mount -t vfat /dev/disk0p2 /r; diff -r /dev/shm/legacy-before /r/rach2-config && "
               "test ! -e /r/rach2-config/generation && echo CHOSEN\"\"WHOLE; umount /r; echo "
               "CHOSEN\"\"CHECKED");
    drive_expect(&d, "CHOSENCHECKED");
    drive_send(&d, "rach2-install --update /m/%s /dev/disk0", KEY_ROOT_NAME);
    drive_expect(&d, "before the root is written");
    drive_expect(&d, "what reached the root is the image");
    drive_expect(&d, "the configuration is archived on");
    drive_expect(&d, "updated:");
    drive_send(&d, "%s",
               "mkdir -p /r && mount -t vfat /dev/disk0p2 /r && echo RECOVERY\"\"MOUNTED");
    drive_expect(&d, "RECOVERYMOUNTED");
    drive_send(&d, "%s", "grep -c ^extra: /r/rach2-config-a/etc/passwd | sed s/^/EXTRA=/");
    drive_send(&d, "%s",
               "test ! -e /r/rach2-config && test ! -e /r/rach2-config.new && test ! -e "
               "/r/rach2-config.old && echo LEGACY\"\"GONE");
    drive_send(&d, "%s",
               "a=$(cat /r/rach2-config-a/generation); b=$(cat /r/rach2-config-b/generation); "
               "test \"$a\" -gt \"$b\" && test ! -e /r/rach2-config-a/update-in-progress && echo "
               "ARCHIVE\"\"CURRENT");
    drive_send(&d, "%s", "umount /r; umount /m; echo ARCHIVESTOP\"\"DONE");
    drive_expect(&d, "ARCHIVESTOPDONE");
    text = console_of(&d);
    drive_stop(&d);
    if (drive_halted()) {
        free(text);
        return;
    }
    ok = !d.failed && text != NULL &&
         strstr(text, "taking the configuration saved on the recovery partition") != NULL &&
         text_field(text, "CHOSENWHOLE", 1, line, sizeof(line)) == 0 &&
         text_field(text, "LEGACYGONE", 1, line, sizeof(line)) == 0 &&
         text_field(text, "EXTRA=1", 1, line, sizeof(line)) == 0 &&
         text_field(text, "ARCHIVECURRENT", 1, line, sizeof(line)) == 0;
    if (!ok)
        snprintf(r->why, sizeof(r->why),
                 "the retry did not recover the complete legacy archive%s%s", d.failed ? ": " : "",
                 d.failed ? d.why : "");
    repair_assert(r->c, "legacy-archive", ok, r->why);
    free(text);
}

/** Retry an update stopped between the root's write and the accounts'
 * return, from the marked archive, row 16. */
static void stopped_update(struct resume *r)
{
    struct drive d;

    repair_recovery(&d, r->c, "recovery");
    drive_send(&d, "rach2-install --update /m/%s /dev/disk0", KEY_ROOT_NAME);
    drive_expect(&d, "an update was stopped part way, and the root holds the image's accounts; "
                     "taking the configuration saved on the recovery partition");
    drive_expect(&d, "before the root is written");
    drive_expect(&d, "what reached the root is the image");
    drive_expect(&d, "the configuration is archived on");
    drive_expect(&d, "updated:");
    drive_send(&d, "%s", "umount /m; echo STOPPED\"\"DONE");
    drive_expect(&d, "STOPPEDDONE");
    drive_stop(&d);
    if (drive_halted())
        return;
    repair_assert(r->c, "stopped-update", !d.failed, d.why);
}

/** Expect on `d`'s console each text of `want`, a list ended by NULL, in
 * order. */
static void expect_all(struct drive *d, const char *const want[])
{
    int i;

    for (i = 0; want[i] != NULL; i++)
        drive_expect(d, want[i]);
}

/**
 * Run `command` as root on the machine over SSH, through the case's port,
 * with root's demo key alone and nothing of the host's own configuration,
 * as `repair_run` runs a tool.
 */
static int root_ssh(struct resume *r, const char *command, const char *output, char **out)
{
    char port[16], known[4200], opt[4300];

    snprintf(port, sizeof(port), "%u", etude_port(r->c, 0));
    if (repair_path(r->c, "known_hosts", known, sizeof(known)) != 0)
        return -1;
    snprintf(opt, sizeof(opt), "UserKnownHostsFile=%s", known);
    return repair_run(r->c,
                      (char *const[]){"ssh",
                                      "-F",
                                      "/dev/null",
                                      "-o",
                                      "StrictHostKeyChecking=no",
                                      "-o",
                                      opt,
                                      "-o",
                                      "BatchMode=yes",
                                      "-o",
                                      "IdentitiesOnly=yes",
                                      "-o",
                                      "PreferredAuthentications=publickey",
                                      "-o",
                                      "PasswordAuthentication=no",
                                      "-i",
                                      r->demo,
                                      "-p",
                                      port,
                                      "root@127.0.0.1",
                                      (char *)command,
                                      NULL},
                      output, out);
}

/**
 * Boot the machine the retried update left, row 17: its accounts, homes,
 * keys and their owners and modes are the machine's, root logs in over SSH
 * with its own key, and every key's digest is what it was.
 */
static void stopped_boot(struct resume *r)
{
    char *out = NULL, *text = NULL, *after = NULL;
    char detail[400];
    struct drive d;
    int ready, login = 0, told = 0;

    repair_login(&d, r->c, "installed-net", r->password);
    drive_send(&d, "%s",
               "grep -c ^extra: /etc/passwd | sed s/^/EXTRA=/; stat -c %u:%g:%a /home/extra");
    expect_all(&d, (const char *const[]){"EXTRA=1", "1001:1001:700", NULL});
    drive_send(&d, "%s", "test -z \"$(ls -A /home/extra)\" && echo HOME\"\"MADE");
    drive_expect(&d, "HOMEMADE");
    drive_send(&d, "%s",
               "test ! -e /home/worker && ! grep -rqs orphan-uid-2999 /root/.ssh /home/*/.ssh && "
               "echo ORPHAN\"\"GONE");
    drive_expect(&d, "ORPHANGONE");
    drive_send(&d, "%s",
               "stat -c %u:%g:%a /home/work /home/work/.ssh /home/work/.ssh/authorized_keys");
    expect_all(&d, (const char *const[]){"2001:2001:700", "2001:2001:700", "2001:2001:600", NULL});
    drive_send(&d, "%s",
               "stat -c %u:%g:%a /home/Alice /home/Alice/.ssh /home/Alice/.ssh/authorized_keys");
    expect_all(&d, (const char *const[]){"2002:2002:700", "2002:2002:700", "2002:2002:600", NULL});
    drive_send(&d, "%s",
               "stat -c %u:%g:%a /home/alice /home/alice/.ssh /home/alice/.ssh/authorized_keys");
    expect_all(&d, (const char *const[]){"2003:2003:700", "2003:2003:700", "2003:2003:600", NULL});
    drive_send(&d, "%s", "stat -c %u:%g:%a /srv/alice/.ssh /srv/alice/.ssh/authorized_keys");
    expect_all(&d, (const char *const[]){"2004:2004:750", "2004:2004:640", NULL});
    drive_send(&d, "%s",
               "test ! -e /srv/empty/.ssh && ! grep -q image-key /srv/alice/.ssh/authorized_keys "
               "&& echo SERVICE\"\"SAFE");
    drive_expect(&d, "SERVICESAFE");
    drive_send(&d, "%s", "stat -c %u:%g:%a /etc/dropbear /root/.ssh /home/test/.ssh");
    expect_all(&d, (const char *const[]){"0:0:700", "0:0:700", "1000:1000:700", NULL});
    drive_send(&d, "%s",
               "stat -c %u:%g:%a /etc/dropbear/* /etc/ssh/ssh_host_* /root/.ssh/authorized_keys "
               "/home/test/.ssh/authorized_keys");
    expect_all(&d, (const char *const[]){"0:0:600", "0:0:600", "0:0:600", "1000:1000:600", NULL});
    drive_send(&d, "%s",
               "sha256sum /etc/dropbear/* /etc/ssh/ssh_host_* /root/.ssh/authorized_keys "
               "/home/test/.ssh/authorized_keys | sed s/^/KEYHASH:/");
    drive_send(&d, "%s",
               "sha256sum /home/work/.ssh/authorized_keys /home/Alice/.ssh/authorized_keys "
               "/home/alice/.ssh/authorized_keys /srv/alice/.ssh/authorized_keys | sed "
               "s/^/KEYHASH:/");
    drive_send(&d, "%s",
               "mkdir -p /r && mount -t vfat /dev/disk0p2 /r && test ! -e "
               "/r/rach2-config-a/update-in-progress && echo MARK\"\"GONE; umount /r");
    drive_expect(&d, "MARKGONE");
    drive_send(&d, "%s", "echo RESUME\"\"READY");
    drive_expect(&d, "RESUMEREADY");
    ready = !d.failed;
    repair_assert(r->c, "restored-keys", ready, d.why);

    if (ready) {
        login = root_ssh(r, "echo RESUMEKEYLOGIN", "ssh.login", &out) == 0 && out != NULL &&
                strstr(out, "RESUMEKEYLOGIN") != NULL;
        snprintf(detail, sizeof(detail), "restored key login: %.300s", out != NULL ? out : "");
        repair_assert(r->c, "restored-login", login, detail);
        free(out);
        out = NULL;
        told = root_ssh(r, "echo STOPPEDUP > /dev/console", "ssh.told", &out) == 0;
        if (told)
            drive_expect(&d, "STOPPEDUP");
        snprintf(detail, sizeof(detail),
                 "the resumed machine did not finish its key checks: %.300s",
                 !told ? (out != NULL ? out : "") : d.why);
        repair_assert(r->c, "finished", told && !d.failed, detail);
        free(out);
    } else {
        repair_assert(r->c, "restored-login", 0, "the resumed machine did not restore the keys");
        repair_assert(r->c, "finished", 0, "the resumed machine did not restore the keys");
    }
    text = console_of(&d);
    drive_stop(&d);
    if (drive_halted()) {
        free(text);
        return;
    }
    after = key_digests(text);
    repair_assert(r->c, "key-hashes",
                  r->before != NULL && after != NULL && strcmp(r->before, after) == 0,
                  "host or account keys changed during the resumed update");
    free(after);
    free(text);
}

/**
 * Stop an update with a copy of the installer that exits once the root is
 * written and checked, and run it again in the same boot, row 19: the second
 * run must take the copy of the homes the first kept in /dev/shm.
 */
static void retry(struct resume *r)
{
    char stop[32] = "", gone[32] = "", status[32] = "", plain[64] = "", owner[32] = "";
    char keep[32] = "";
    char *text = NULL;
    struct drive d;

    repair_recovery(&d, r->c, "recovery");
    drive_send(&d, "%s",
               "mkdir -p /r && mount -t ext2 /dev/disk0p3 /r && echo this-homes-own > "
               "/r/home/test/plain && chown 1000:1000 /r/home/test/plain && umount /r && echo "
               "PLAIN\"\"PUT");
    drive_expect(&d, "PLAINPUT");
    drive_send(&d, "%s",
               "sed 's|^    say \"what reached the root is the image\"$|&; sync; exit 98|' "
               "/bin/rach2-install > /dev/shm/stop-install");
    drive_send(&d, "sh /dev/shm/stop-install --update /m/%s /dev/disk0; echo STOP\"\"_STATUS=$?",
               KEY_ROOT_NAME);
    drive_expect(&d, "STOP_STATUS=");
    drive_send(&d, "%s",
               "mount -t ext2 /dev/disk0p3 /r; test ! -e /r/home/test/plain; echo "
               "PLAIN\"\"GONE=$?; umount /r");
    drive_expect(&d, "PLAINGONE=");
    drive_send(&d, "rach2-install --update /m/%s /dev/disk0; echo RETRY\"\"_STATUS=$?",
               KEY_ROOT_NAME);
    drive_expect(&d, "RETRY_STATUS=");
    drive_send(&d, "%s",
               "mount -t ext2 /dev/disk0p3 /r; sed s/^/PLAIN=/ /r/home/test/plain; stat -c "
               "OWNER=%u:%g /r/home/test/plain; umount /r");
    drive_send(&d, "%s", "test ! -e /dev/shm/rach2-update-keep; echo KEEP\"\"GONE=$?");
    drive_expect(&d, "KEEPGONE=");
    drive_send(&d, "%s", "umount /m; echo RETRY\"\"DONE");
    drive_expect(&d, "RETRYDONE");
    text = console_of(&d);
    drive_stop(&d);
    if (drive_halted()) {
        free(text);
        return;
    }
    repair_assert(r->c, "retry-finished", !d.failed, d.why);
    repair_last_field(text, "STOP_STATUS=", stop, sizeof(stop));
    repair_last_field(text, "PLAINGONE=", gone, sizeof(gone));
    repair_last_field(text, "RETRY_STATUS=", status, sizeof(status));
    repair_last_field(text, "PLAIN=", plain, sizeof(plain));
    repair_last_field(text, "OWNER=", owner, sizeof(owner));
    repair_last_field(text, "KEEPGONE=", keep, sizeof(keep));
    repair_assert(r->c, "retry-stopped", strcmp(stop, "98") == 0 && strcmp(gone, "0") == 0,
                  "the first update did not stop with the image's root written");
    repair_assert(r->c, "retry-restored",
                  strcmp(status, "0") == 0 && text != NULL &&
                      strstr(text, "an update of this boot stopped after it wrote the root; "
                                   "taking the configuration it kept") != NULL &&
                      strcmp(plain, "this-homes-own") == 0 && strcmp(owner, "1000:1000") == 0 &&
                      strcmp(keep, "0") == 0,
                  "the update run again in the same boot lost the homes the first one kept");
    free(text);
}

/**
 * Run the rows in order on the installed disk, each step a boot or a change
 * made on the host.
 *
 * Each stopped state is made at a point the program or the host chooses,
 * never by time: a planted mark and root on the host, or a copy of the
 * installer that exits at a line of its own. A row whose state the rest
 * build on stops them when it fails, as the suite stopped there, and the
 * assertions not reached fail with the reason.
 */
static void plant_legacy(struct resume *r)
{
    struct etude_case *c = r->c;
    uint64_t len;
    const char *p;
    if ((p = etude_private_copy(c, "disk")) != NULL)
        snprintf(r->disk, sizeof(r->disk), "%s", p);
    if (p == NULL || repair_entry(c, "mkgpt", r->disk, 2, &r->rec, &len) != 0 ||
        repair_entry(c, "mkgpt", r->disk, 3, &r->rootat, &len) != 0)
        snprintf(r->why, sizeof(r->why),
                 "the recovery or the root partition's start could not be read");
    r->rec /= 512;
    r->rootat /= 512;
    if (r->why[0] == '\0')
        plant(r, "legacy");
}

int etude_scenario(struct etude_case *c)
{
    struct resume r;

    memset(&r, 0, sizeof(r));
    r.c = c;
    if (drive_channel(c) != 0 || etude_step(c, "keys") != ETUDE_OK)
        return 1;
    if (repair_password(c, r.password, sizeof(r.password)) != 0)
        snprintf(r.why, sizeof(r.why), "the demo password could not be read");
    else
        make_keys(&r);
    if (drive_halted())
        return 1;
    if (r.why[0] != '\0') {
        rest(&r, "keys-recorded");
        goto done;
    }

    if (etude_step(c, "record") != ETUDE_OK)
        return 1;
    record(&r);
    if (drive_halted())
        return 1;
    repair_assert(c, "keys-recorded", r.why[0] == '\0', r.why);
    if (r.why[0] != '\0') {
        rest(&r, "payload");
        goto done;
    }

    if (etude_step(c, "payload") != ETUDE_OK)
        return 1;
    payload(&r);
    if (drive_halted())
        return 1;
    repair_assert(c, "payload", r.why[0] == '\0', r.why);
    if (r.why[0] != '\0') {
        rest(&r, "key-names");
        goto done;
    }

    if (etude_step(c, "archive-keys") != ETUDE_OK)
        return 1;
    archive_keys(&r);
    if (drive_halted())
        return 1;
    if (r.why[0] != '\0') {
        rest(&r, "legacy-planted");
        goto done;
    }

    if (etude_step(c, "plant-legacy") != ETUDE_OK)
        return 1;
    plant_legacy(&r);
    if (drive_halted())
        return 1;
    repair_assert(c, "legacy-planted", r.why[0] == '\0', r.why);
    if (r.why[0] != '\0') {
        rest(&r, "legacy-archive");
        goto done;
    }

    if (etude_step(c, "archive-stop") != ETUDE_OK)
        return 1;
    archive_stop(&r);
    if (drive_halted())
        return 1;
    if (r.why[0] != '\0') {
        rest(&r, "slot-planted");
        goto done;
    }

    if (etude_step(c, "plant-slot") != ETUDE_OK)
        return 1;
    plant(&r, "slot");
    if (drive_halted())
        return 1;
    repair_assert(c, "slot-planted", r.why[0] == '\0', r.why);
    if (r.why[0] != '\0') {
        rest(&r, "stopped-update");
        goto done;
    }

    if (etude_step(c, "stopped-update") != ETUDE_OK)
        return 1;
    stopped_update(&r);
    if (drive_halted())
        return 1;
    if (etude_step(c, "stopped-boot") != ETUDE_OK)
        return 1;
    stopped_boot(&r);
    if (drive_halted())
        return 1;
    if (etude_step(c, "retry") != ETUDE_OK)
        return 1;
    retry(&r);
    if (drive_halted())
        return 1;
done:
    free(r.before);
    repair_finish(c);
    return 0;
}
