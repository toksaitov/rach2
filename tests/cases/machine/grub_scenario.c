/*
 * grub_scenario.c: the scenario of machine.grub.replace
 * (tests/cases/machine.grub.json): the machine replacing its own boot
 * loader, in three boots of one GRUB disk through SeaBIOS.
 *
 * The gap after the MBR has two tails for GRUB's core image, and the gap's
 * first sector, the entry sector, selects one. `bootctl grub` writes the
 * tail that is not in use, reads it back, and only then rewrites the entry
 * sector; sector 0, which has the partition table, is never written. The
 * disk is made with an unmarked core, and its root stages two marked ones,
 * which print GRUBCORE=A or GRUBCORE=B on the serial line as GRUB starts.
 * The first boot installs B, the second must run B, installs A back and
 * tries /dev/grubcore with two openers, and the third must run A. The marker
 * comes from inside the core that ran, so it names the image that ran and
 * not the pointer that selected it: a tool that wrote the pointer and
 * nothing else would pass a read-back of the pointer.
 *
 * QEMU runs with -no-reboot, so `reboot -f` ends the first boot with a
 * reset and `poweroff -f` the second with a shutdown, and each boot is a
 * QEMU of its own on the same copy of the disk.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The case's one machine. */
#define MACHINE "grub"

/* What the console says when the boot is ready for a login. */
#define LOGIN "rach2 login:"

/* What a marked core prints as GRUB starts, before its letter. */
#define MARKER "GRUBCORE="

/* What bootctl says before the first sector of the tail it wrote. */
#define TAIL "in the tail at LBA "

/* What one attempt shares among its steps. */
struct grub_case {
    struct net_case u;
    char password[128];
    long tail_b; /* the tail core B was written to, or -1 */
};

/** Count the occurrences of `word` in `text`; 0 for NULL. */
static unsigned occurrences(const char *text, const char *word)
{
    unsigned n = 0;

    for (text = text != NULL ? strstr(text, word) : NULL; text != NULL;
         text = strstr(text + 1, word))
        n++;
    return n;
}

/** Give the letter after the first `MARKER` of `text`, or `-` when it has
 * none. */
static char first_core(const char *text)
{
    const char *at = text != NULL ? strstr(text, MARKER) : NULL;

    return at != NULL && at[strlen(MARKER)] != '\0' ? at[strlen(MARKER)] : '-';
}

/** Run `command` at the root shell of `b` and give the console between the
 * typed line and `marker`, or NULL after the attempt failed. */
static char *run(struct net_boot *b, const char *command, const char *marker)
{
    char *text = NULL;

    net_command(b, command, marker, &text);
    return text;
}

/** End boot `b` with `command` typed at its shell, and require the end
 * `want`, which `what` names. */
static void end_with(struct net_boot *b, const char *command, enum etude_end want, const char *what)
{
    struct etude_termination end;

    if (net_line(b, command) != ETUDE_OK)
        return;
    end.end = ETUDE_END_LOST;
    if (etude_wait_end(b->m, &end) != ETUDE_OK || end.end != want)
        net_fail(b->u, "%s did not end the boot with a %s", command, what);
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

/**
 * Step `install-b`: the disk's own core, which prints no marker, boots;
 * root installs core B with `bootctl grub` and reboots.
 */
static void install_b(struct grub_case *s)
{
    struct net_boot b;
    char *text = NULL, *console = NULL;

    if (net_step(&s->u, "install-b") == ETUDE_OK &&
        net_boot(&b, &s->u, MACHINE, 1, NULL) == ETUDE_OK &&
        net_login(&b, "root", s->password, "# ") == ETUDE_OK) {
        text = run(&b, "bootctl grub /core-b.img && echo GRUBB\"\"OK; echo GRUBB\"\"DONE",
                   "GRUBBDONE");
        s->tail_b = net_number_after(text, TAIL);
        end_with(&b, "reboot -f", ETUDE_END_RESET, "reset");
        console = net_file(b.console);
    }
    net_assert(&s->u, "first-unmarked", console != NULL && occurrences(console, MARKER) == 0,
               "the boot of the disk as made printed %u GRUBCORE markers",
               occurrences(console, MARKER));
    net_assert(&s->u, "installed-b", net_find(text, "GRUBBOK") != NULL,
               "bootctl grub did not install core B");
    free(text);
    free(console);
}

/**
 * Step `install-a`: the boot must run core B; root installs core A back,
 * and /dev/grubcore must refuse a second opener while the first holds it
 * and take an opener once the holder has exited.
 */
static void install_a(struct grub_case *s)
{
    struct net_boot b;
    char *text = NULL, *busy = NULL, *again = NULL, *console = NULL;
    long tail_a = -1;

    if (net_step(&s->u, "install-a") == ETUDE_OK &&
        net_boot(&b, &s->u, MACHINE, 2, NULL) == ETUDE_OK &&
        net_login(&b, "root", s->password, "# ") == ETUDE_OK) {
        text = run(&b, "bootctl grub /core-a.img && echo GRUBA\"\"OK; echo GRUBA\"\"DONE",
                   "GRUBADONE");
        tail_a = net_number_after(text, TAIL);
        busy = run(&b,
                   "( exec 7</dev/grubcore; exec 8</dev/grubcore ) 2>&1 | "
                   "busybox grep -qi busy && echo SECOND\"\"BUSY; echo BUSY\"\"DONE",
                   "BUSYDONE");
        again = run(&b, "( exec 7</dev/grubcore ) && echo AGAIN\"\"OK; echo AGAIN\"\"DONE",
                    "AGAINDONE");
        end_with(&b, "poweroff -f", ETUDE_END_SHUTDOWN, "shutdown");
        console = net_file(b.console);
    }
    net_assert(&s->u, "runs-b",
               console != NULL && occurrences(console, MARKER) == 1 && first_core(console) == 'B',
               "the boot after core B was installed printed %u GRUBCORE markers, the first %c",
               occurrences(console, MARKER), first_core(console));
    net_assert(&s->u, "installed-a", net_find(text, "GRUBAOK") != NULL,
               "bootctl grub did not install core A back");
    net_assert(&s->u, "tails-distinct", s->tail_b >= 0 && tail_a >= 0 && tail_a != s->tail_b,
               "core B went to the tail at LBA %ld and core A to the tail at LBA %ld", s->tail_b,
               tail_a);
    net_assert(&s->u, "second-busy", net_find(busy, "SECONDBUSY") != NULL,
               "a second open of /dev/grubcore was not refused as busy");
    net_assert(&s->u, "again-ok", net_find(again, "AGAINOK") != NULL,
               "/dev/grubcore stayed held after its holder exited");
    free(text);
    free(busy);
    free(again);
    free(console);
}

/** Step `back`: the boot after core A was installed back must run core A,
 * to the login prompt. */
static void back(struct grub_case *s)
{
    struct net_boot b;
    char *console = NULL;

    if (net_step(&s->u, "back") == ETUDE_OK && net_boot(&b, &s->u, MACHINE, 3, NULL) == ETUDE_OK &&
        net_wait(&b, LOGIN) == ETUDE_OK) {
        quit(&b);
        console = net_file(b.console);
    }
    net_assert(&s->u, "runs-a",
               console != NULL && occurrences(console, MARKER) == 1 && first_core(console) == 'A',
               "the boot after core A was installed printed %u GRUBCORE markers, the first %c",
               occurrences(console, MARKER), first_core(console));
    free(console);
}

int etude_scenario(struct etude_case *c)
{
    struct grub_case s;

    memset(&s, 0, sizeof(s));
    s.tail_b = -1;
    net_case(&s.u, c);
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    install_b(&s);
    install_a(&s);
    back(&s);
    return net_end(&s.u);
}
