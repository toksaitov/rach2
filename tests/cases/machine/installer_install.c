/*
 * installer_install.c: the scenario of machine.installer.install: the
 * installer on a machine with two patterned disks, the machines it writes,
 * and the recovery system's key on them.
 *
 * The test stick installs onto the second disk after its refusals, and onto
 * the first, given a key for the recovery system; the second disk then
 * boots with the accounts the install wrote, the first answers SSH with
 * them, and each boots its recovery system from its own menu with the
 * recovery image of a tree with no key: the first must take the key the
 * install placed and let root in with it alone, and the second, installed
 * with none, must start no SSH server. The steps share the two disks, as
 * rows 3 to 6c of the old suite did.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib_installer.h"

/* The size of each patterned disk, as the old suite made them. */
#define DISK_BYTES 268435456u

/* A root shell's command that waits until the SSH server listens on port
 * 22, by the kernel's table of TCP sockets, and then prints `MARK`. */
#define LISTENING(MARK) \
    "until grep -q ':0016 00000000:0000 0A ' /proc/net/tcp; do :; done; echo " MARK

/* What the steps share: the paths of the keys, the demo password, the
 * recovery key's public half, and whether each install completed. */
struct state {
    char rkey_file[4096];
    char demo_key[4096];
    char rkey[512];
    char password[128];
    int keys_made;
    int second_installed;
    int first_installed;
};

/* Row 3, up to the refusals: the installer names both disks, refuses an
 * answer that is not one of them and a confirmation that is not the disk's
 * own name, and stops when told to or when its answers end. */
static const struct inst_line refusals[] = {
    {0, "recovery image"},
    {0, "# "},
    {1, "echo '-----BEGIN OPENSSH PRIVATE KEY-----' > /tmp/private.key; "
        "printf '\\043 a comment\\n' > /tmp/comments.key"},
    {0, "# "},
    {1, "rach2-install"},
    {0, "disk shall Rach2 be written to"},
    {1, ""},
    {0, "Nothing was typed"},
    {0, "disk shall Rach2 be written to"},
    {1, "/dev/disk9"},
    {0, "is not in the list"},
    {0, "disk shall Rach2 be written to"},
    {1, "2"},
    {0, "the password for root"},
    {1, INST_NEWPASS},
    {0, "again"},
    {1, "different"},
    {0, "Those two were different"},
    {0, "the password for root"},
    {1, ""},
    {0, "again"},
    {1, ""},
    {0, "An empty password will not do"},
    {0, "the password for root"},
    {1, INST_NEWPASS},
    {0, "again"},
    {1, INST_NEWPASS},
    {0, "The name of the first account"},
    {1, "ROOT"},
    {0, "names are lowercase"},
    {0, "The name of the first account"},
    {1, "root"},
    {0, "root is made by this install already"},
    {0, "The name of the first account"},
    {1, INST_USER},
    {0, "the password for " INST_USER},
    {1, INST_USERPASS},
    {0, "again"},
    {1, INST_USERPASS},
    {0, "uthorized keys"},
    {1, "/no/such/key"},
    {0, "is not a file"},
    {0, "uthorized keys"},
    {1, ""},
    {0, "key for the recovery system"},
    {1, "/no/such/key"},
    {0, "is not a file"},
    {0, "key for the recovery system"},
    {1, "/tmp/private.key"},
    {0, "holds a private key"},
    {0, "key for the recovery system"},
    {1, "/tmp/comments.key"},
    {0, "has no line that is a key"},
    {0, "key for the recovery system"},
    {1, ""},
    {0, "row the root"},
    {1, "no"},
    {0, "Check this before you go on"},
    {0, "Key for the recovery system: none"},
    {0, "Root partition: left at the size"},
    {0, "to go on, or stop to stop"},
    {1, "yes"},
    {0, "That is not /dev/disk1"},
    {0, "to go on, or stop to stop"},
    {1, "stop"},
    {0, "nothing was written"},
    {0, "# "},
    {1, "printf '/dev/disk1\\n' | rach2-install"},
    {0, "the answers ended before the questions did; nothing was written"},
    {0, "# "},
    {0, NULL},
};

/* Row 3, the install onto the second disk, which grows its root. */
static const struct inst_line install_second[] = {
    {0, "# "},
    {1, "rach2-install"},
    {0, "disk shall Rach2 be written to"},
    {1, "/dev/disk1"},
    {0, "the password for root"},
    {1, INST_NEWPASS},
    {0, "again"},
    {1, INST_NEWPASS},
    {0, "The name of the first account"},
    {1, INST_USER},
    {0, "the password for " INST_USER},
    {1, INST_USERPASS},
    {0, "again"},
    {1, INST_USERPASS},
    {0, "uthorized keys"},
    {1, ""},
    {0, "key for the recovery system"},
    {1, ""},
    {0, "row the root"},
    {1, ""},
    {0, "Check this before you go on"},
    {0, "to go on, or stop to stop"},
    {1, "/dev/disk1"},
    {0, "the image on the stick is the one the manifest describes"},
    {0, "what reached the disk is the image"},
    {0, "the table is the one the image carries"},
    {0, "the accounts are root and " INST_USER},
    {0, "growing the root into the rest of the disk"},
    {0, "installed:"},
    {1, "echo INSTALL\"\"DONE"},
    {0, "INSTALLDONE"},
    {0, NULL},
};

/* Row 3b, the install onto the first disk, after the recovery key's public
 * half is written to /tmp/recovery.pub. */
static const struct inst_line install_first[] = {
    {0, "# "},
    {1, "rach2-install"},
    {0, "disk shall Rach2 be written to"},
    {1, "/dev/disk0"},
    {0, "the password for root"},
    {1, INST_NEWPASS},
    {0, "again"},
    {1, INST_NEWPASS},
    {0, "The name of the first account"},
    {1, INST_USER},
    {0, "the password for " INST_USER},
    {1, INST_USERPASS},
    {0, "again"},
    {1, INST_USERPASS},
    {0, "uthorized keys"},
    {1, "/root/.ssh/authorized_keys"},
    {0, "key for the recovery system"},
    {1, "/tmp/recovery.pub"},
    {0, "row the root"},
    {1, "no"},
    {0, "Key for the recovery system:"},
    {0, "to go on, or stop to stop"},
    {1, "/dev/disk0"},
    {0, "the image on the stick is the one the manifest describes"},
    {0, "what reached the disk is the image"},
    {0, "takes the key in /tmp/recovery.pub"},
    {0, "installed:"},
    {1, "echo OTHER\"\"DONE"},
    {0, "OTHERDONE"},
    {0, NULL},
};

/* Row 4, after the image's own password was refused: the new one is taken,
 * and the policy, the home and the root are as the install left them. */
static const struct inst_line installed_up[] = {
    {0, "rach2 login:"},
    {1, "root"},
    {0, "Password:"},
    {1, INST_NEWPASS},
    {0, "# "},
    {1, "id -u; grep -c . /etc/passwd"},
    {0, "0"},
    {1, "grep -q '^root-password-login yes' /etc/rach2/sshd.conf && echo POLICY\"\"SET"},
    {0, "POLICYSET"},
    {1, "test -d /home/" INST_USER " && test ! -d /root/.ssh && echo HOME\"\"MADE"},
    {0, "HOMEMADE"},
    {1, "df -k / | tail -1"},
    {0, "/dev/"},
    {0, NULL},
};

/* Row 4, the archive of the accounts on the recovery partition. */
static const struct inst_line archived[] = {
    {1, "mkdir -p /r && mount -t vfat /dev/disk0p2 /r && "
        "cmp /r/rach2-config-a/etc/shadow /etc/shadow && "
        "grep -c ^" INST_USER ": /r/rach2-config-a/etc/passwd; umount /r"},
    {0, "1"},
    {1, "echo INSTALLED\"\"UP"},
    {0, "INSTALLEDUP"},
    {0, NULL},
};

/* Row 6, the login at the console of the first disk's machine, until its
 * SSH server listens. */
static const struct inst_line ssh_up[] = {
    {0, "rach2 login:"}, {1, "root"}, {0, "Password:"},
    {1, INST_NEWPASS},   {0, "# "},   {1, LISTENING("SSH\"\"READY")},
    {0, "SSHREADY"},     {0, NULL},
};

/* Row 6b, the keyed disk's recovery system until its SSH server listens. */
static const struct inst_line keyed_up[] = {
    {0, "recovery image"}, {0, "# "}, {1, LISTENING("RKEY\"\"READY")}, {0, "RKEYREADY"}, {0, NULL},
};

/* Row 6c, the unkeyed disk's recovery system. Two console sessions are
 * ended, so that init has tested twice for a key by the token. */
static const struct inst_line unkeyed_up[] = {
    {0, "recovery image"},
    {0, "# "},
    {1, "exit"},
    {0, "# "},
    {1, "exit"},
    {0, "# "},
    {1, "echo NOSERVER\"\"READY"},
    {0, "NOSERVERREADY"},
    {0, NULL},
};

/** Give the length of entry 3 of the GUID table of `image`, the root, or 0. */
static uint64_t root_bytes(struct etude_case *c, const char *image)
{
    uint64_t first, bytes;

    return inst_gpt_entry(c, image, 3, &first, &bytes) == 0 ? bytes : 0;
}

/**
 * Make the recovery key with ssh-keygen in the evidence directory, and read
 * the demo key and password the install's accounts are checked with.
 */
static void make_keys(struct etude_case *c, struct state *s)
{
    char pub[4200], *text;
    char *argv[] = {"ssh-keygen", "-q",       "-t", "ed25519",    "-N", "",
                    "-C",         "recovery", "-f", s->rkey_file, NULL};
    int status;

    snprintf(s->rkey_file, sizeof(s->rkey_file), "%s", etude_evidence(c, "recovery.key"));
    snprintf(pub, sizeof(pub), "%s.pub", s->rkey_file);
    inst_made(s->rkey_file);
    inst_made(pub);
    text = inst_host_text(c, argv, "ssh-keygen", &status);
    free(text);
    text = evidence_text(c, "recovery.key.pub");
    if (status == 0 && text != NULL) {
        text[strcspn(text, "\r\n")] = '\0';
        snprintf(s->rkey, sizeof(s->rkey), "%s", text);
    }
    free(text);
    s->keys_made =
        s->rkey[0] != '\0' &&
        inst_private_file(c, "demo_key", "demo.key", s->demo_key, sizeof(s->demo_key)) == 0 &&
        inst_artifact_line(c, "password", s->password, sizeof(s->password)) == 0;
}

/**
 * Row 3: the refusals and the install onto the second of the two patterned
 * disks, and the first disk compared whole with the pattern afterwards.
 */
static int row_install_second(struct etude_case *c, struct state *s)
{
    char blank[65] = "", other[65] = "", detail[200];
    struct drive d;
    char *text = NULL;
    int refused;

    make_keys(c, s);
    if (inst_pattern_disk(c, "one", DISK_BYTES, blank) != 0 ||
        inst_pattern_disk(c, "two", DISK_BYTES, other) != 0 || strcmp(blank, other) != 0 ||
        !s->keys_made) {
        snprintf(detail, sizeof(detail), "%s",
                 s->keys_made ? "the patterned disks could not be made"
                              : "the keys or the demo password could not be made or read");
        inst_fail(c, "refusals", detail);
        inst_fail(c, "blank-after-refusals", detail);
        inst_fail(c, "install-second", detail);
        inst_fail(c, "other-untouched", detail);
        inst_fail(c, "table-published", detail);
        return 0;
    }
    drive_boot(&d, c, "installer", (const char *const[]){"stick-overlay", "one", "two", NULL});
    inst_exchange(&d, refusals);
    refused = !d.failed;
    drive_send(&d,
               "a=$(sha256sum < /dev/disk0); b=$(sha256sum < /dev/disk1); [ \"$a\" = \"$b\" ] && "
               "[ \"${a%%%% *}\" = %s ] && echo BOTH\"\"BLANK || echo BOTH\"\"CHANGED; "
               "echo COMPARE\"\"D",
               blank);
    drive_expect(&d, "COMPARED");
    if (!d.failed && (text = drive_console(&d)) != NULL &&
        (strstr(text, "BOTHCHANGED") != NULL || strstr(text, "BOTHBLANK") == NULL))
        inst_refuse(&d, "a disk changed while the installer refused to write");
    free(text);
    text = NULL;
    inst_assert(c, "refusals", refused, d.why);
    inst_assert(c, "blank-after-refusals", !d.failed, d.why);
    inst_exchange(&d, install_second);
    if (!d.failed)
        text = drive_console(&d);
    drive_stop(&d);
    if (drive_halted()) {
        free(text);
        return 1;
    }
    s->second_installed = !d.failed;
    inst_assert(c, "install-second", !d.failed, d.why);
    if (file_digest(etude_private_copy(c, "one"), 0, UINT64_MAX, other) != 0)
        other[0] = '\0';
    inst_assert(c, "other-untouched", strcmp(other, blank) == 0, "the other disk was written");
    inst_assert(c, "table-published", text != NULL && text_lines(text, " part 1 ") >= 2,
                text == NULL ? d.why : "the new table was not published");
    free(text);
    return 0;
}

/**
 * Row 3b: the install onto the first disk, given the recovery key, which
 * must leave the second disk as row 3 left it; then the boot code of both
 * and the size of each root.
 */
static int row_install_first(struct etude_case *c, struct state *s)
{
    const char *one = etude_private_copy(c, "one"), *two = etude_private_copy(c, "two");
    char before[65] = "", after[65] = "", boot1[65] = "", boot2[65] = "", bootimg[65] = "";
    char detail[300];
    uint64_t grown, kept;
    struct drive d;

    if (file_digest(two, 0, UINT64_MAX, before) != 0)
        before[0] = '\0';
    drive_boot(&d, c, "installer", (const char *const[]){"stick-overlay", "one", "two", NULL});
    if (!s->keys_made)
        inst_refuse(&d, "the recovery key was not made");
    drive_expect(&d, "recovery image");
    drive_expect(&d, "# ");
    drive_send(&d, "echo '%s' > /tmp/recovery.pub", s->rkey);
    inst_exchange(&d, install_first);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    s->first_installed = !d.failed;
    inst_assert(c, "install-first", !d.failed, d.why);
    if (file_digest(two, 0, UINT64_MAX, after) != 0)
        after[0] = '\0';
    inst_assert(c, "second-kept", before[0] != '\0' && strcmp(before, after) == 0,
                "the install onto the first disk changed the second");
    if (file_digest(one, 0, 440, boot1) != 0 || file_digest(two, 0, 440, boot2) != 0 ||
        file_digest(etude_artifact(c, "image"), 0, 440, bootimg) != 0)
        bootimg[0] = '\0';
    snprintf(detail, sizeof(detail), "the first disk begins %s and the second %s, and the image %s",
             boot1, boot2, bootimg);
    inst_assert(c, "boot-code",
                bootimg[0] != '\0' && strcmp(boot1, bootimg) == 0 && strcmp(boot2, bootimg) == 0,
                detail);
    grown = root_bytes(c, two);
    kept = root_bytes(c, one);
    snprintf(detail, sizeof(detail),
             "the grown root is %" PRIu64 " bytes and the one left alone is %" PRIu64, grown, kept);
    inst_assert(c, "grown", grown > 0 && kept > 0 && grown > kept, detail);
    return 0;
}

/**
 * Row 4: the second disk boots on its own, refuses the image's own
 * password, takes the new one, and has the archive of its accounts on its
 * recovery partition.
 */
static int row_boot_second(struct etude_case *c, struct state *s)
{
    struct drive d;
    int booted;

    drive_boot(&d, c, "installed", NULL);
    if (!s->second_installed)
        inst_refuse(&d, "the second disk was not installed");
    drive_expect(&d, "rach2 login:");
    drive_send(&d, "root");
    drive_expect(&d, "Password:");
    drive_send(&d, "%s", s->password);
    drive_expect(&d, "incorrect");
    inst_exchange(&d, installed_up);
    booted = !d.failed;
    inst_exchange(&d, archived);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    inst_assert(c, "installed-boots", booted, d.why);
    inst_assert(c, "accounts-archived", !d.failed, d.why);
    return 0;
}

/**
 * Row 6: the accounts the install onto the first disk wrote, over SSH, with
 * the server init starts. Root's password is the one typed at the install,
 * the account takes the keys it was given, the recovery system's own, and
 * the image's demonstration key is refused for root. The last attempt
 * writes a token on the console, so that the machine is stopped only after
 * the attempts are over.
 */
static int row_ssh(struct etude_case *c, struct state *s)
{
    struct drive d;
    char *out;
    int up;

    drive_boot(&d, c, "net", (const char *const[]){"one", NULL});
    if (!s->first_installed)
        inst_refuse(&d, "the first disk was not installed");
    inst_exchange(&d, ssh_up);
    up = !d.failed;
    inst_assert(c, "ssh-up", up, d.why);
    if (!up) {
        drive_stop(&d);
        if (drive_halted())
            return 1;
        inst_fail(c, "root-password", d.why);
        inst_fail(c, "account-key", d.why);
        inst_fail(c, "demo-key-refused", d.why);
        inst_fail(c, "ssh-told", d.why);
        return 0;
    }
    out = inst_ssh_password(c, "known_hosts.ssh", INST_NEWPASS, "id -u; echo ROOT\"\"PASSWORD",
                            "ssh.root-password");
    inst_assert(c, "root-password", out != NULL && strstr(out, "ROOTPASSWORD") != NULL,
                out != NULL ? out : "ssh's output cannot be read");
    free(out);
    out = inst_ssh_key(c, "known_hosts.ssh", s->demo_key, INST_USER, "id -un; echo KEY\"\"LOGIN",
                       "ssh.account-key");
    inst_assert(c, "account-key", out != NULL && strstr(out, "KEYLOGIN") != NULL,
                out != NULL ? out : "ssh's output cannot be read");
    free(out);
    out =
        inst_ssh_key(c, "known_hosts.ssh", s->demo_key, "root", "echo DEMO\"\"KEY", "ssh.demo-key");
    inst_assert(c, "demo-key-refused",
                out != NULL && strstr(out, "DEMOKEY") == NULL &&
                    strstr(out, "Permission denied") != NULL,
                out != NULL ? out : "ssh's output cannot be read");
    free(out);
    out = inst_ssh_password(c, "known_hosts.ssh", INST_NEWPASS,
                            "echo TOLD; echo SSH\"\"DONE > /dev/console", "ssh.told");
    if (out == NULL || strstr(out, "TOLD") == NULL)
        inst_refuse(&d, out != NULL ? out : "ssh's output cannot be read");
    drive_expect(&d, "SSHDONE");
    free(out);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    inst_assert(c, "ssh-told", !d.failed, d.why);
    return 0;
}

/**
 * Read `::/boot/recovery_authorized_keys` of the recovery partition of
 * `disk` with `mtype` into `buf`, without carriage returns or the newlines
 * at its end.
 *
 * @return mtype's exit status, or -1 when it did not run.
 */
static int placed_key(struct etude_case *c, const char *disk, const char *output, char *buf,
                      size_t size)
{
    char at[4200], *text, *p, *q;
    uint64_t first, bytes;
    char *argv[] = {"mtype", "-i", at, "::/boot/recovery_authorized_keys", NULL};
    int status;

    buf[0] = '\0';
    if (inst_gpt_entry(c, disk, 2, &first, &bytes) != 0)
        return -1;
    snprintf(at, sizeof(at), "%s@@%" PRIu64, disk, first);
    status = drive_host(c, argv, output);
    text = evidence_text(c, output);
    if (text != NULL) {
        for (p = q = text; *p != '\0'; p++)
            if (*p != '\r')
                *q++ = *p;
        while (q > text && q[-1] == '\n')
            q--;
        *q = '\0';
        snprintf(buf, size, "%s", text);
    }
    free(text);
    return status;
}

/** The assertions of row 6b, which a failure before the attempts fails. */
static const char *const keyed_rows[] = {
    "rkey-up", "rkey-taken", "rkey-login", "rkey-free", "rkey-other-refused", "rkey-told", NULL};

/**
 * Row 6b: the first disk, installed with the recovery key, boots its
 * recovery system with the keyless image. The key the install placed is
 * read on the host first; the recovery system must say it took the key off
 * its partition, let root in with that key and no other, and leave the disk
 * with no mount on it.
 */
static int row_keyed(struct etude_case *c, struct state *s)
{
    const char *one = etude_private_copy(c, "one");
    char placed[600], why[200], detail[800];
    const char *const *id;
    struct drive d;
    char *text, *out;

    placed_key(c, one, "mtype.keyed", placed, sizeof(placed));
    snprintf(detail, sizeof(detail),
             "the recovery partition's key file holds '%s', not the key given", placed);
    inst_assert(c, "rkey-placed", s->rkey[0] != '\0' && strcmp(placed, s->rkey) == 0, detail);
    if (!s->first_installed || inst_recovery_boot(c, one, why, sizeof(why)) != 0) {
        if (!s->first_installed)
            snprintf(why, sizeof(why), "the first disk was not installed");
        for (id = keyed_rows; *id != NULL; id++)
            inst_fail(c, *id, why);
        return 0;
    }
    drive_boot(&d, c, "net", (const char *const[]){"one", NULL});
    inst_exchange(&d, keyed_up);
    inst_assert(c, "rkey-up", !d.failed, d.why);
    if (d.failed) {
        drive_stop(&d);
        if (drive_halted())
            return 1;
        for (id = keyed_rows + 1; *id != NULL; id++)
            inst_fail(c, *id, d.why);
        return 0;
    }
    text = drive_console(&d);
    inst_assert(c, "rkey-taken",
                text != NULL &&
                    strstr(text, "init: recovery key: taken from /dev/disk0p2") != NULL &&
                    strstr(text, "no key was built into this recovery image, and its recovery "
                                 "partition's key turns SSH on") != NULL,
                "the recovery system did not say it took the key off its partition");
    free(text);
    out =
        inst_ssh_key(c, "known_hosts.keyed", s->rkey_file, "root",
                     "id -u; grep \"^identity 0 \" /proc/disk; echo RECOVERY\"\"KEY", "ssh.keyed");
    inst_assert(c, "rkey-login", out != NULL && strstr(out, "RECOVERYKEY") != NULL,
                out != NULL ? out : "ssh's output cannot be read");
    text = out != NULL ? strstr(out, "identity 0 ") : NULL;
    inst_assert(c, "rkey-free", text != NULL && strstr(text, " free ") != NULL,
                out != NULL ? out : "ssh's output cannot be read");
    free(out);
    out = inst_ssh_key(c, "known_hosts.keyed", s->demo_key, "root", "echo DEMO\"\"KEY",
                       "ssh.keyed-other");
    inst_assert(c, "rkey-other-refused",
                out != NULL && strstr(out, "DEMOKEY") == NULL &&
                    strstr(out, "Permission denied") != NULL,
                out != NULL ? out : "ssh's output cannot be read");
    free(out);
    out = inst_ssh_key(c, "known_hosts.keyed", s->rkey_file, "root",
                       "echo TOLD; echo RKEY\"\"DONE > /dev/console", "ssh.keyed-told");
    if (out == NULL || strstr(out, "TOLD") == NULL)
        inst_refuse(&d, out != NULL ? out : "ssh's output cannot be read");
    drive_expect(&d, "RKEYDONE");
    free(out);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    inst_assert(c, "rkey-told", !d.failed, d.why);
    return 0;
}

/** The assertions of row 6c after the key file's, which a failure before
 * the probe fails. */
static const char *const unkeyed_rows[] = {"nokey-up",     "nokey-off",  "nokey-no-server",
                                           "nokey-closed", "nokey-done", NULL};

/**
 * Say whether the output `out` of `ssh -v` shows that nothing answered SSH:
 * QEMU closed the forwarded connection when the guest refused it, or the
 * connection was refused. A server's identification line, a command that
 * ran or an authentication refused fail; anything else shows nothing.
 */
static int nothing_answered(const char *out, unsigned port)
{
    char refused[96];

    snprintf(refused, sizeof(refused), "connect to host 127.0.0.1 port %u: Connection refused",
             port);
    if (strstr(out, "Remote protocol version") != NULL || strstr(out, "NODOOR") != NULL ||
        strstr(out, "Permission denied") != NULL)
        return 0;
    return strstr(out, "Connection closed by") != NULL ||
           strstr(out, "Connection reset by") != NULL || strstr(out, refused) != NULL;
}

/**
 * Row 6c: the second disk, installed with no key for its recovery system,
 * has none on its recovery partition, and its recovery system with the
 * keyless image must say SSH is off, start no server by init's second test
 * for a key, and answer nothing on the port.
 */
static int row_unkeyed(struct etude_case *c, struct state *s)
{
    const char *two = etude_private_copy(c, "two");
    char placed[600], why[200], hosts[4200], port[16];
    const char *const *id;
    struct drive d;
    char *text, *out;
    int status;
    char *argv[] = {"ssh",
                    "-v",
                    "-o",
                    "StrictHostKeyChecking=no",
                    "-o",
                    hosts,
                    "-o",
                    "BatchMode=yes",
                    "-o",
                    "IdentitiesOnly=yes",
                    "-o",
                    "PreferredAuthentications=publickey",
                    "-o",
                    "PasswordAuthentication=no",
                    "-i",
                    s->rkey_file,
                    "-p",
                    port,
                    "root@127.0.0.1",
                    "echo NO\"\"DOOR",
                    NULL};

    status = placed_key(c, two, "mtype.unkeyed", placed, sizeof(placed));
    inst_assert(c, "nokey-none", s->second_installed && status > 0,
                s->second_installed ? "a disk installed with no key for its recovery system has one"
                                    : "the second disk was not installed");
    if (!s->second_installed || inst_recovery_boot(c, two, why, sizeof(why)) != 0) {
        if (!s->second_installed)
            snprintf(why, sizeof(why), "the second disk was not installed");
        for (id = unkeyed_rows; *id != NULL; id++)
            inst_fail(c, *id, why);
        return 0;
    }
    drive_boot(&d, c, "net", (const char *const[]){"two", NULL});
    inst_exchange(&d, unkeyed_up);
    inst_assert(c, "nokey-up", !d.failed, d.why);
    if (d.failed) {
        drive_stop(&d);
        if (drive_halted())
            return 1;
        for (id = unkeyed_rows + 1; *id != NULL; id++)
            inst_fail(c, *id, d.why);
        return 0;
    }
    text = drive_console(&d);
    inst_assert(
        c, "nokey-off",
        text != NULL &&
            strstr(text, "init: no key was built into this recovery image, so SSH is off") !=
                NULL &&
            strstr(text, "init: recovery key: taken from") == NULL,
        "the recovery system of a disk with no key did not say SSH is off");
    inst_assert(c, "nokey-no-server", text != NULL && strstr(text, "init: sshd: started") == NULL,
                text == NULL ? "the console cannot be read"
                             : "init started an SSH server on a recovery system with no key");
    free(text);
    snprintf(hosts, sizeof(hosts), "UserKnownHostsFile=%s",
             etude_evidence(c, "known_hosts.unkeyed"));
    inst_made(etude_evidence(c, "known_hosts.unkeyed"));
    snprintf(port, sizeof(port), "%u", etude_port(c, 0));
    out = inst_host_text(c, argv, "ssh.unkeyed", &status);
    inst_assert(c, "nokey-closed", out != NULL && nothing_answered(out, etude_port(c, 0)),
                out != NULL ? out : "ssh's output cannot be read");
    free(out);
    drive_send(&d, "echo NOSERVER\"\"DONE");
    drive_expect(&d, "NOSERVERDONE");
    drive_stop(&d);
    if (drive_halted())
        return 1;
    inst_assert(c, "nokey-done", !d.failed, d.why);
    return 0;
}

/** Run the rows in order, each a step of the manifest. */
int etude_scenario(struct etude_case *c)
{
    struct state s;

    memset(&s, 0, sizeof(s));
    if (etude_step(c, "install-second") != ETUDE_OK || row_install_second(c, &s) != 0)
        return 1;
    if (etude_step(c, "install-first") != ETUDE_OK || row_install_first(c, &s) != 0)
        return 1;
    if (etude_step(c, "boot-second") != ETUDE_OK || row_boot_second(c, &s) != 0)
        return 1;
    if (etude_step(c, "ssh") != ETUDE_OK || row_ssh(c, &s) != 0)
        return 1;
    if (etude_step(c, "recovery-keyed") != ETUDE_OK || row_keyed(c, &s) != 0)
        return 1;
    if (etude_step(c, "recovery-unkeyed") != ETUDE_OK || row_unkeyed(c, &s) != 0)
        return 1;
    inst_finish();
    return 0;
}
