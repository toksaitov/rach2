/*
 * menu_scenario.c: the scenario of machine.menu.staged
 * (tests/cases/machine.menu.json): the boot menu that bootctl rewrites in
 * place, and the stub menu that it cannot, in four boots of one GRUB disk.
 *
 * GRUB reads two files on the disk. /boot/grub/grub.cfg, the stub, is
 * written once when the disk is made; it defines every entry the machine
 * needs and then sources /boot/boot.cfg, the slot that `bootctl config`
 * overwrites in place. The stub's entries are defined first, so they keep
 * the low numbers whatever the slot holds, and `set fallback` takes a
 * number. The first boot stages a menu of two entries the stub lacks,
 * round-trips a grubenv key and destroys the candidate kernel; the second
 * must fall back to the stub's own entry with both staged ones listed after
 * the stub's; the third, with the slot torn from the host, must still boot
 * through the stub; the fourth reads the slot's extent back.
 *
 * No boot lets the staged file choose the default, and it cannot: a slot
 * overwritten in place is not written atomically, so a torn file must not
 * be able to point the machine at anything.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The bytes of the slot that bootctl overwrites. */
#define SLOT_BYTES 8192

/* The first line of the staged menu, which the host finds on the disk. */
#define STAGED_HEAD "menuentry stagedone --id stagedone"

/* The staged menu, in two lines typed apart, since one line holding both
 * would overrun the buffer of the console's line discipline; printf's %b
 * turns each \n into a newline. */
#define MENU_ONE                                        \
    "menuentry stagedone --id stagedone {\\nmultiboot " \
    "/boot/fallback rach2.root=disk rach2.menu=stagedone\\nboot\\n}\\n"
#define MENU_TWO                                        \
    "menuentry stagedtwo --id stagedtwo {\\nmultiboot " \
    "/boot/fallback rach2.root=disk rach2.menu=stagedtwo\\nboot\\n}\\n"

/* The titles of the whole menu in the order of their numbers: the stub's
 * three, then the two staged. */
static const char *const titles[] = {"Start Rach2", "Start Rach2 with the previous kernel",
                                     "Start the Rach2 recovery system (not yet built)", "stagedone",
                                     "stagedtwo"};

/* What one attempt shares among its steps. */
struct menu_case {
    struct net_case u;
    char password[128];
    char slot[200]; /* boot.cfg's extent as bootctl status gave it first */
};

/** Give the text after `first sector ` on the line of `text` that has
 * `tag`, in `buf` of `size` bytes; an empty text when there is none. */
static const char *extent(const char *text, const char *tag, char *buf, size_t size)
{
    const char *line = net_find(text, tag), *at;
    char copy[400];

    buf[0] = '\0';
    if (line == NULL)
        return buf;
    net_copy_line(line, copy, sizeof(copy));
    at = strstr(copy, "first sector ");
    if (at != NULL)
        snprintf(buf, size, "%s", at + strlen("first sector "));
    return buf;
}

/**
 * Say whether the console's text has `Falling back to` and then the stub's
 * fallback title in quotes, the closing quote included, so that a title
 * that begins with the same words cannot match.
 */
static int fell_back_to_stub(const char *text)
{
    static const char want[] = "Start Rach2 with the previous kernel'";
    const char *p;

    for (p = strstr(text, "Falling back to "); p != NULL; p = strstr(p + 1, "Falling back to ")) {
        p += strlen("Falling back to ");
        if (*p != '\0' && strncmp(p + 1, want, strlen(want)) == 0)
            return 1;
    }
    return 0;
}

/**
 * Say whether the titles GRUB drew on the console `text` are `titles`, in
 * that order, each kept once. GRUB draws one title to a row of the serial
 * console, after the cursor escape `ESC [ ROW ;3H`, and marks the
 * highlighted one with a leading asterisk. Give the titles drawn, separated
 * by `|`, in `seen` of `size` bytes.
 */
static int menu_in_order(const char *text, char *seen, size_t size)
{
    const char *kept[32];
    size_t lens[32], nkept = 0, i, n, used = 0;
    const char *p, *s;
    int same;

    seen[0] = '\0';
    for (p = strchr(text, '\033'); p != NULL; p = strchr(p + 1, '\033')) {
        s = p + 1;
        if (*s++ != '[' || *s < '0' || *s > '9')
            continue;
        while (*s >= '0' && *s <= '9')
            s++;
        if (strncmp(s, ";3H", 3) != 0)
            continue;
        s += 3;
        s += strspn(s, " *");
        n = strcspn(s, "\033\r\n");
        while (n > 0 && s[n - 1] == ' ')
            n--;
        if (n == 0)
            continue;
        for (i = 0; i < nkept && (lens[i] != n || memcmp(kept[i], s, n) != 0); i++)
            ;
        if (i < nkept || nkept == sizeof(kept) / sizeof(kept[0]))
            continue;
        kept[nkept] = s;
        lens[nkept++] = n;
        if (used + n + 2 < size) {
            used += (size_t)snprintf(seen + used, size - used, "%s%.*s", nkept > 1 ? "|" : "",
                                     (int)n, s);
        }
    }
    same = nkept == sizeof(titles) / sizeof(titles[0]);
    for (i = 0; same && i < nkept; i++)
        same = lens[i] == strlen(titles[i]) && memcmp(kept[i], titles[i], lens[i]) == 0;
    return same;
}

/** Run `command` at the root shell of `b` and give the console between the
 * typed line and `marker`, or NULL after the attempt failed. */
static char *run(struct net_boot *b, const char *command, const char *marker)
{
    char *text = NULL;

    net_command(b, command, marker, &text);
    return text;
}

/** End boot `b` with QMP's `quit` and wait for the machine to end. */
static void quit(struct net_boot *b)
{
    struct etude_termination end;
    char *reply = NULL;

    if (!net_ok(b->u))
        return;
    etude_qmp(b->m, "quit", NULL, &reply);
    free(reply);
    etude_wait_end(b->m, &end);
}

/** End boot `b` with `poweroff -f` typed at its shell, and require the
 * machine's shutdown. */
static void power_off(struct net_boot *b)
{
    struct etude_termination end;

    if (net_line(b, "poweroff -f") != ETUDE_OK)
        return;
    if (etude_wait_end(b->m, &end) != ETUDE_OK || end.end != ETUDE_END_SHUTDOWN)
        net_fail(b->u, "poweroff -f did not shut the machine down");
}

/**
 * Step `stage`: log in, note boot.cfg's extent, stage the two entries,
 * round-trip next_entry through bootctl env and bootctl cancel, and zero
 * the candidate kernel's first sector, so that the next boot must take the
 * fallback.
 */
static void stage(struct menu_case *s)
{
    struct net_boot b;
    char *config = NULL, *armed = NULL, *cancelled = NULL, *zeroed = NULL, *status;

    if (net_step(&s->u, "stage") == ETUDE_OK && net_boot(&b, &s->u, "menu", 1, NULL) == ETUDE_OK &&
        net_login(&b, "root", s->password, "# ") == ETUDE_OK) {
        status = run(&b,
                     "bootctl status | busybox grep 'boot.cfg' | busybox sed s/^/SLOTA\"\"=/; "
                     "echo SLOTA\"\"DONE",
                     "SLOTADONE");
        extent(status, "SLOTA=", s->slot, sizeof(s->slot));
        free(status);
        free(run(&b, "busybox printf '%b' '" MENU_ONE "' > /tmp/m.cfg; echo PART\"\"ONE",
                 "PARTONE"));
        config = run(&b,
                     "busybox printf '%b' '" MENU_TWO "' >> /tmp/m.cfg; "
                     "bootctl config /tmp/m.cfg && echo STAGED\"\"OK; echo CONFIG\"\"DONE",
                     "CONFIGDONE");
        armed = run(&b,
                    "bootctl env next_entry candidate > /dev/null; "
                    "bootctl env next_entry | busybox sed s/^/ENVA\"\"=/; echo ENVA\"\"DONE",
                    "ENVADONE");
        cancelled = run(&b,
                        "bootctl cancel > /dev/null && "
                        "bootctl env next_entry | busybox sed s/^/ENVB\"\"=/; echo ENVB\"\"DONE",
                        "ENVBDONE");
        zeroed = run(&b,
                     "S=$(bootctl status | busybox awk '/^bootctl: candidate: first sector/"
                     "{print $5}' | busybox tr -d ,); busybox dd if=/dev/zero of=/dev/bootpart "
                     "bs=512 seek=$S count=1 2>/dev/null && echo CORRUPT\"\"OK; "
                     "echo CORRUPT\"\"DONE",
                     "CORRUPTDONE");
        power_off(&b);
    }
    net_assert(&s->u, "staged", net_find(config, "STAGEDOK") != NULL,
               "bootctl config did not take the menu");
    net_assert(&s->u, "env-armed", net_find(armed, "ENVA=*candidate") != NULL,
               "bootctl env did not read back the armed value");
    net_assert(&s->u, "env-cancelled", net_find(cancelled, "ENVB=*absent") != NULL,
               "bootctl cancel did not remove the trial key, so it left the consumed marker");
    net_assert(&s->u, "corrupted", net_find(zeroed, "CORRUPTOK") != NULL,
               "the candidate's first sector was not zeroed through /dev/bootpart");
    free(config);
    free(armed);
    free(cancelled);
    free(zeroed);
}

/**
 * Step `fallback`: the candidate no longer loads, so GRUB takes the
 * fallback number. That number indexes the merged list, which is why the
 * stub's entries are defined before the slot is sourced: entry 1 must
 * still be the stub's own fallback and not whatever a staged menu put
 * there.
 */
static void fallback(struct menu_case *s)
{
    struct net_boot b;
    char *text = NULL, seen[400] = "";
    int ordered = 0;

    if (net_step(&s->u, "fallback") == ETUDE_OK &&
        net_boot(&b, &s->u, "menu", 2, NULL) == ETUDE_OK &&
        net_wait(&b, "rach2 login:") == ETUDE_OK) {
        quit(&b);
        text = net_file(b.console);
        ordered = menu_in_order(text, seen, sizeof(seen));
    }
    net_assert(&s->u, "staged-entries",
               text != NULL && strstr(text, "stagedone") != NULL &&
                   strstr(text, "stagedtwo") != NULL,
               "the staged menu's entries are not both in the menu");
    net_assert(&s->u, "fallback-stub", text != NULL && fell_back_to_stub(text),
               "the fallback number did not select the stub's own fallback entry");
    net_assert(&s->u, "menu-order", ordered, "the menu drawn was %s", seen);
    net_assert(&s->u, "fallback-not-staged",
               text != NULL && net_find(text, "Falling back to*staged") == NULL,
               "the fallback number selected a staged entry");
    free(text);
}

/**
 * Give the offset of the first `STAGED_HEAD` in the file at `path`, and its
 * size in `size`; -1 when it is not there or the file cannot be read.
 */
static long find_slot(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    static char buf[1 << 20];
    size_t want = strlen(STAGED_HEAD), keep = 0, n;
    long base = 0, at = -1;
    char *p;

    *size = -1;
    if (f == NULL)
        return -1;
    while (at < 0 && (n = fread(buf + keep, 1, sizeof(buf) - keep, f)) > 0) {
        n += keep;
        for (p = buf; p + want <= buf + n; p++) {
            if (memcmp(p, STAGED_HEAD, want) == 0) {
                at = base + (p - buf);
                break;
            }
        }
        keep = n < want - 1 ? n : want - 1;
        memmove(buf, buf + n - keep, keep);
        base += (long)(n - keep);
    }
    if (fseek(f, 0, SEEK_END) == 0)
        *size = ftell(f);
    fclose(f);
    return at;
}

/**
 * Tear the staged slot in the disk's copy: its last entry keeps its first
 * line and loses its closing brace, and the rest of the slot is `#`, so the
 * file keeps its size and nothing of the FAT changes. Report `slot-found`,
 * and give 1 when the slot was written.
 */
static int tear(struct menu_case *s)
{
    const char *copy = etude_private_copy(s->u.c, "menu-disk");
    char slot[SLOT_BYTES];
    long at, size = -1;
    size_t n;
    FILE *f;
    int ok = 0;

    at = copy != NULL ? find_slot(copy, &size) : -1;
    net_assert(&s->u, "slot-found", at >= 0 && at + SLOT_BYTES <= size,
               at < 0 ? "the staged menu is not on the disk" : "the menu slot runs past the image");
    if (!net_ok(&s->u) || at < 0 || at + SLOT_BYTES > size)
        return 0;
    memset(slot, '#', sizeof(slot));
    n = strlen(STAGED_HEAD " {\n");
    memcpy(slot, STAGED_HEAD " {\n", n);
    f = fopen(copy, "r+b");
    if (f != NULL) {
        ok = fseek(f, at, SEEK_SET) == 0 && fwrite(slot, 1, sizeof(slot), f) == sizeof(slot);
        ok = fclose(f) == 0 && ok;
    }
    if (!ok)
        net_fail(&s->u, "the slot could not be written in the disk's copy");
    return ok;
}

/** Say whether `text` has `word` in any case. */
static int has_any_case(const char *text, const char *word)
{
    size_t i, n = strlen(word);

    for (; *text != '\0'; text++) {
        for (i = 0; i < n && text[i] != '\0' && tolower((unsigned char)text[i]) == word[i]; i++)
            ;
        if (i == n)
            return 1;
    }
    return 0;
}

/** Step `torn`: GRUB must report the parse error and the machine must still
 * boot, since the stub defined its own entries before it read the slot. */
static void torn(struct menu_case *s)
{
    struct net_boot b;
    char *text = NULL;
    int booted = 0;

    if (net_step(&s->u, "torn") != ETUDE_OK)
        net_assert(&s->u, "slot-found", 0, "");
    else if (tear(s) && net_boot(&b, &s->u, "menu", 3, NULL) == ETUDE_OK &&
             net_wait(&b, "rach2 login:") == ETUDE_OK) {
        booted = 1;
        quit(&b);
        text = net_file(b.console);
    }
    net_assert(&s->u, "torn-boots", booted, "the machine did not boot with the slot torn");
    net_assert(&s->u, "torn-error", text != NULL && has_any_case(text, "error"),
               "a torn menu gave no loader error, so GRUB did not read it");
    net_assert(&s->u, "torn-not-staged",
               text != NULL && net_find(text, "Falling back to*staged") == NULL,
               "a torn menu still reached one of its own entries");
    free(text);
}

/** Step `after`: the slot's extent after all of it is the one before, so
 * the chain found when the volume opened is the chain still there. */
static void after(struct menu_case *s)
{
    struct net_boot b;
    char *status = NULL, slot[200] = "";

    if (net_step(&s->u, "after") == ETUDE_OK && net_boot(&b, &s->u, "menu", 4, NULL) == ETUDE_OK &&
        net_login(&b, "root", s->password, "# ") == ETUDE_OK) {
        status = run(&b,
                     "bootctl status | busybox grep 'boot.cfg' | busybox sed s/^/SLOTB\"\"=/; "
                     "echo SLOTB\"\"DONE",
                     "SLOTBDONE");
        extent(status, "SLOTB=", slot, sizeof(slot));
        power_off(&b);
    }
    net_assert(&s->u, "slot-in-place", s->slot[0] != '\0' && strcmp(s->slot, slot) == 0,
               "the slot's extent changed across a staged write: [%s] then [%s]", s->slot, slot);
    free(status);
}

int etude_scenario(struct etude_case *c)
{
    struct menu_case s;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    stage(&s);
    fallback(&s);
    torn(&s);
    after(&s);
    return net_end(&s.u);
}
