/*
 * virtionet_scenario.c: the scenario of the virtio network cases
 * (../machine.virtionet.json): the virtio network driver on QEMU's
 * virtio-net-pci in each form the device takes, each case a boot of the
 * test image.
 *
 * Every case reads the boot log for the driver's lines, which say how the
 * device interrupts and what was negotiated. The cases with the device
 * alone then run vnetprobe at the debug shell and take its report from the
 * console: the interface's link files, a ping, and the driver let go of
 * and bound again through /sys with a ping after. The `probe` case has the
 * device as QEMU makes it, with a vector for each queue, mergeable
 * receive buffers, the event index and the control queue; `intx` takes its
 * vectors away, so the device raises the function's line; `plain` takes
 * away the three features, so each frame fills one buffer, the queues are
 * silenced by their flags and no receive mode is set; `legacy` has the
 * device of the legacy interface alone, without mergeable buffers, so the
 * transport drives it through its I/O BAR with rings of 256 entries and
 * each frame has the 10-byte header; and `speed` has a device that gives a
 * speed, a duplex and a largest MTU, which the interface's files must
 * read.
 *
 * The `probe` case then pulls the cable out and puts it back through QMP's
 * set_link: the device raises its configuration interrupt at each, the
 * driver logs the link's state, and the debug shell's ping gets a reply
 * once it is back.
 *
 * The `beside` case boots an RTL8139 and the virtio device together. The
 * driver table has the virtio driver last, so the RTL8139 is eth0 and the
 * virtio device eth1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console_report.h"
#include "netcase.h"

/* A row of the boot log: its assertion, the text a line of the log has, and
 * the detail when no line has it. */
struct log_row {
    const char *id;
    const char *text;
    const char *why;
};

/* A case: its identity, the rows of its boot log, the command that runs
 * vnetprobe or NULL, whether no feature may follow the driver's count of
 * its buffers, and whether the cable is pulled. */
struct vnet_case {
    const char *id;
    const struct log_row *rows;
    size_t nrows;
    const char *probe;
    int bare;
    int cable;
};

#define ROWS(r) (r), sizeof(r) / sizeof((r)[0])

/* The driver's line of its buffers, which the features taken follow. */
#define BUFFERS "virtio_net: virtio0: rx 128, tx 64 buffers"

static const struct log_row probe_rows[] = {
    {"eth0", "net: eth0: virtio_net, station ", "the virtio device did not become eth0"},
    {"vectors", "virtio: interrupts: virtio0: msi-x, a vector for each of 3 queues",
     "the device did not get a vector for each queue"},
    {"features", BUFFERS ", mergeable, event index, control queue",
     "the driver did not take mergeable buffers, the event index and the control queue"},
};

static const struct log_row intx_rows[] = {
    {"eth0", "net: eth0: virtio_net, station ", "the virtio device did not become eth0"},
    {"line", "virtio: interrupts: virtio0: intx line ", "the device did not take its line"},
};

static const struct log_row plain_rows[] = {
    {"eth0", "net: eth0: virtio_net, station ", "the virtio device did not become eth0"},
    {"buffers", BUFFERS, "the driver did not print its buffers"},
};

static const struct log_row legacy_rows[] = {
    {"eth0", "net: eth0: virtio_net, station ", "the virtio device did not become eth0"},
    {"legacy", " net, legacy, io 0x",
     "the transport did not take the function by its legacy interface"},
    {"vectors", "virtio: interrupts: virtio0: msi-x, a vector for each of 3 queues",
     "the device did not get a vector for each queue"},
    {"features", BUFFERS ", event index, control queue",
     "the driver did not take the event index and the control queue alone"},
};

static const struct log_row speed_rows[] = {
    {"eth0", "net: eth0: virtio_net, station ", "the virtio device did not become eth0"},
    {"speed", "virtio_net: link 1000 Mb/s, full duplex",
     "the driver did not print the device's speed and duplex"},
};

static const struct log_row beside_rows[] = {
    {"rtl8139-eth0", "net: eth0: RTL8139", "the RTL8139 did not become eth0"},
    {"virtio-eth1", "net: eth1: virtio_net, station ", "the virtio device did not become eth1"},
};

static const struct vnet_case cases[] = {
    {"machine.virtionet.probe", ROWS(probe_rows), "exec /vnetprobe", 0, 1},
    {"machine.virtionet.intx", ROWS(intx_rows), "exec /vnetprobe", 0, 0},
    {"machine.virtionet.plain", ROWS(plain_rows), "exec /vnetprobe", 1, 0},
    {"machine.virtionet.legacy", ROWS(legacy_rows), "exec /vnetprobe", 0, 0},
    {"machine.virtionet.speed", ROWS(speed_rows), "exec /vnetprobe speed=1000 duplex=full mtu=1400",
     0, 0},
    {"machine.virtionet.beside", ROWS(beside_rows), NULL, 0, 0},
};

/** Report the rows of case `k` against the console of `b` from the boot's
 * start to the offset read so far, and for a case whose device has no
 * feature of its own, that none follows the driver's count of its
 * buffers. */
static void log_rows(struct net_boot *b, const struct vnet_case *k)
{
    char *log = net_text(b, 0, b->at);
    size_t i;

    for (i = 0; i < k->nrows; i++)
        net_assert(b->u, k->rows[i].id, net_find(log, k->rows[i].text) != NULL, "%s",
                   k->rows[i].why);
    if (k->bare)
        net_assert(b->u, "no-features", net_find(log, BUFFERS ",") == NULL,
                   "the driver took a feature the device does not have");
    free(log);
}

/** Run `command`, which starts vnetprobe, at the debug shell, which has
 * greeted, make the report it writes on the console the case's own, and
 * wait for the shell's next greeting. */
static void probe(struct net_boot *b, const char *command)
{
    uint64_t typed = b->at, at;

    net_line(b, command);
    if (!net_ok(b->u))
        return;
    if (console_report(b->u->c, b->m, "guest", 1, typed, &at) != ETUDE_OK)
        net_fail(b->u, "vnetprobe: its report was not read whole");
    else
        b->at = at;
    net_wait(b, NET_SHELL);
}

/** Plug the card's cable in, `on`, or pull it out, through QMP. */
static void cable(struct net_boot *b, int on)
{
    char args[64];
    char *reply = NULL;

    if (!net_ok(b->u))
        return;
    snprintf(args, sizeof(args), "{\"name\": \"net0\", \"up\": %s}", on ? "true" : "false");
    if (etude_qmp(b->m, "set_link", args, &reply) != ETUDE_OK)
        net_fail(b->u, "QMP set_link of the cable: %s", reply != NULL ? reply : "no reply");
    free(reply);
}

/** The cable pulled out and put back: the driver's line at each, and the
 * debug shell's ping answered once it is back. */
static void cable_steps(struct net_boot *b)
{
    cable(b, 0);
    net_wait(b, "virtio_net: link: virtio0: down");
    net_assert(b->u, "link-down", 1, "");
    cable(b, 1);
    net_wait(b, "virtio_net: link: virtio0: up");
    net_assert(b->u, "link-up", 1, "");
    net_line(b, "netping");
    net_wait(b, "ping 10.0.2.2: reply in ");
    net_assert(b->u, "ping-after", 1, "");
}

/**
 * Boot the machine `guest` of the case the identity names to the debug
 * shell and run the case's steps; refuse an identity that is no case of
 * the family.
 */
int etude_scenario(struct etude_case *c)
{
    const struct vnet_case *k = NULL;
    struct net_case u;
    struct net_boot b;
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (strcmp(etude_case_id(c), cases[i].id) == 0)
            k = &cases[i];
    }
    if (k == NULL) {
        fprintf(stderr, "virtionet_scenario: %s is no case of machine.virtionet\n",
                etude_case_id(c));
        return 1;
    }
    net_case(&u, c);
    net_step(&u, "boot");
    net_boot(&b, &u, "guest", 1, NULL);
    net_wait(&b, NET_SHELL);
    net_step(&u, "log");
    log_rows(&b, k);
    if (k->probe != NULL) {
        net_step(&u, "vnetprobe");
        probe(&b, k->probe);
    }
    if (k->cable) {
        net_step(&u, "cable");
        cable_steps(&b);
    }
    return net_end(&u);
}
