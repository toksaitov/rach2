/*
 * dhcp_scenario.c: the scenario of the cases of tests/cases/machine.dhcp.json:
 * the machine taking an address it was not told, with both of its clients,
 * BusyBox's `udhcpc`, which `ifup` runs on os-real and the metal image, and
 * os-simple's own `dhcpc`, which its `netup` runs.
 *
 * A boot that is to take a lease has `rach2.ip=none`, so IPv4 is unset and
 * nothing is configured until userland does it, and its card's user network
 * is one of its own, 192.168.90.0/24 or 10.90.0.0/16, so that the address
 * that arrives cannot be the one the machine would have had anyway. What the
 * machine sends is read from QEMU's capture of the card once the machine has
 * ended, outside the guest: pcapcheck requires the packets sent from the
 * leased address, which only a machine holding it can send, and a packet for
 * an address on no subnet of the machine's addressed to the gateway's
 * hardware address, which is what a default route means.
 *
 * Each case is a boot of its own, or two of one disk, chosen by the case's
 * identity. At os-real's shell, which takes the quotes out of a word, each
 * command ends by printing its exit status and a marker written with an
 * empty pair of quotes inside it; os-simple's shell does not take them out,
 * so its cases wait for a line that only the program prints.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The machine of every case. */
#define MACHINE "guest"

/* os-real's and the metal image's root prompt, and os-simple's banner after
 * a login. */
#define ROOT_PROMPT "# "
#define ISH_BANNER  "The ISH shell"

/* The stanza that gives os-real a fixed address on the /16 of os-simple's
 * network, as `ifup` reads it. */
#define REAL_STATIC                                                              \
    "printf 'auto eth0\\niface eth0 inet static\\naddress 10.90.0.50\\nnetmask " \
    "255.255.0.0\\ngateway 10.90.0.2\\n' > /etc/network/interfaces"

/* What one attempt shares among its steps. */
struct dhcp_case {
    struct net_case u;
    struct net_boot b;
    char password[128]; /* root's, the demo password */
};

/** Say whether `text` has `what` in it; NULL has nothing. */
static int has(const char *text, const char *what)
{
    return text != NULL && strstr(text, what) != NULL;
}

/** Boot the machine as boot `n` and log in as root at its console, waiting
 * for `prompt`. */
static void boot_login(struct dhcp_case *s, unsigned n, const char *prompt)
{
    net_boot(&s->b, &s->u, MACHINE, n, NULL);
    net_login(&s->b, "root", s->password, prompt);
}

/**
 * Run `command` at os-real's shell, give its exit status, or -1 when the
 * shell did not say, and give in `*text`, when `text` is not NULL, what the
 * console showed from the typed line to the status, which the caller frees.
 */
static long status_of(struct dhcp_case *s, const char *command, char **text)
{
    char line[480];
    char *said = NULL;
    long status;

    if (text != NULL)
        *text = NULL;
    if (strlen(command) + 40 > sizeof(line)) {
        net_fail(&s->u, "the command is too long: %.40s", command);
        return -1;
    }
    snprintf(line, sizeof(line), "%s; echo STATUS=$?; echo STATUS\"\"END", command);
    if (net_command(&s->b, line, "STATUSEND", &said) != ETUDE_OK)
        return -1;
    status = net_number_after(said, "STATUS=");
    if (text != NULL)
        *text = said;
    else
        free(said);
    return status;
}

/** Type `command` at os-simple's shell and wait for `line`, which only the
 * program prints. */
static void ish(struct dhcp_case *s, const char *command, const char *line)
{
    net_line(&s->b, command);
    net_wait(&s->b, line);
}

/**
 * Type `command`, a ping of four packets, and give the count of replies its
 * summary line reports, or -1 when it printed none.
 */
static long received(struct dhcp_case *s, const char *command)
{
    uint64_t from;
    char *text;
    long n;

    if (net_line(&s->b, command) != ETUDE_OK)
        return -1;
    from = s->b.at;
    if (net_wait(&s->b, " packets transmitted, ") != ETUDE_OK ||
        net_wait(&s->b, " received") != ETUDE_OK)
        return -1;
    text = net_text(&s->b, from, s->b.at);
    n = net_number_after(text, " packets transmitted, ");
    free(text);
    return n;
}

/** Make the machine's QEMU quit and wait for its end, so that its captures
 * are whole. */
static void quit(struct dhcp_case *s)
{
    struct etude_termination end;
    char *reply = NULL;

    if (net_ok(&s->u) && etude_qmp(s->b.m, "quit", NULL, &reply) == ETUDE_FAILED)
        net_fail(&s->u, "QEMU refused to quit");
    free(reply);
    if (net_ok(&s->u) && etude_wait_end(s->b.m, &end) == ETUDE_LOST)
        net_fail(&s->u, "the machine was lost before it ended");
}

/** Power the machine off at os-real's shell with `poweroff -f` and wait
 * for its end, which must be QEMU's shutdown. */
static void poweroff(struct dhcp_case *s)
{
    struct etude_termination end;

    end.end = ETUDE_END_LOST;
    if (net_line(&s->b, "poweroff -f") != ETUDE_OK)
        return;
    if (etude_wait_end(s->b.m, &end) == ETUDE_LOST)
        net_fail(&s->u, "the machine was lost before it ended");
    else if (end.end != ETUDE_END_SHUTDOWN)
        net_fail(&s->u, "the machine did not power off: it ended as %d", (int)end.end);
}

/**
 * Run `pcapcheck bcast` on the capture of card `card` with `args`, ended by
 * NULL, keeping its output as evidence `output`, and say whether it passed.
 */
static int on_wire(struct dhcp_case *s, const char *card, const char *output,
                   const char *const args[])
{
    char *argv[16] = {"pcapcheck", "bcast"};
    const char *capture;
    struct net_run r;
    size_t i;
    int ok;

    if (!net_ok(&s->u))
        return 0;
    capture = etude_capture(s->b.m, card);
    if (capture == NULL) {
        net_fail(&s->u, "the frames of %s were not recorded", card);
        return 0;
    }
    argv[2] = (char *)capture;
    for (i = 0; args[i] != NULL && i + 4 < sizeof(argv) / sizeof(argv[0]); i++)
        argv[3 + i] = (char *)args[i];
    argv[3 + i] = NULL;
    net_host(&s->u, argv, output, &r);
    ok = net_exited0(&r);
    net_run_free(&r);
    return ok;
}

/**
 * Wait until the guest's SSH server answers on the case's port `i`, as
 * sshpeer judges it, and say whether it did.
 */
static int answers(struct dhcp_case *s, unsigned i, const char *output)
{
    char port[16];
    char *argv[] = {"sshpeer", "wait", port, NULL};
    struct net_run r;
    int ok;

    snprintf(port, sizeof(port), "%u", etude_port(s->u.c, i));
    net_host(&s->u, argv, output, &r);
    ok = net_exited0(&r) && has(r.out, "sshpeer: answered");
    net_run_free(&r);
    return ok;
}

/**
 * The `lease` case, on the RTL8139: os-real finds eth0 unset, `ifup -a` takes
 * the lease of 192.168.90.20 with its mask, its broadcast address and its name
 * server, and the machine reaches the gateway by IP and by ARP. An arping at an
 * address nothing answers must end at its deadline with status 1: the run ends
 * in the SIGALRM handler, so it needs the interval timer (setitimer(2)).
 * Nothing answers 10.9.9.9, and nothing needs to: the capture says where the
 * ping went.
 */
static void lease_case(struct dhcp_case *s)
{
    static const char *const routed[] = {"--routed-via", "192.168.90.2", "--routed-dst", "10.9.9.9",
                                         NULL};
    char *text = NULL;
    long st;

    net_step(&s->u, "boot");
    boot_login(s, 1, ROOT_PROMPT);
    net_step(&s->u, "unset");
    st = status_of(s, "ifconfig eth0 | grep -q 'addr:0.0.0.0'", NULL);
    net_assert(&s->u, "unset", st == 0, "ifconfig did not show eth0 without an address");

    net_step(&s->u, "ifup");
    st = status_of(s, "ifup -a", &text);
    if (st == 0)
        net_seen(&s->b, "net: eth0: IPv4 192.168.90.20/");
    net_assert(&s->u, "lease", st == 0 && has(text, "lease of 192.168.90.20 obtained"),
               "ifup -a exited %ld without the lease of 192.168.90.20", st);
    free(text);
    st = status_of(s,
                   "ifconfig eth0 > /tmp/if; grep -q 'addr:192.168.90.20' /tmp/if && "
                   "grep -q 'Bcast:192.168.90.255' /tmp/if && "
                   "grep -q 'Mask:255.255.255.0' /tmp/if && "
                   "grep -q '192.168.90.3' /etc/resolv.conf",
                   NULL);
    net_assert(&s->u, "lease-applied", st == 0,
               "the address, broadcast address, mask or name server is not the lease's");

    net_step(&s->u, "reach");
    st = status_of(s, "ping -c 1 192.168.90.2 > /dev/null", NULL);
    net_assert(&s->u, "peer", st == 0, "the gateway did not answer a ping");
    st = status_of(s, "arping -f -w 5 192.168.90.2 2>&1", &text);
    net_assert(&s->u, "arping-reply", st == 0 && has(text, "Received 1 response"),
               "arping of the gateway exited %ld without its one response", st);
    free(text);
    st = status_of(s, "arping -w 2 192.168.90.9 2>&1", &text);
    net_assert(&s->u, "arping-silent", st == 1 && has(text, "Received 0 response"),
               "arping of an address nothing answers exited %ld, not 1 at its deadline", st);
    free(text);
    st = status_of(s, "arp -n | grep -q '(192.168.90.2)'", NULL);
    net_assert(&s->u, "arp-cached", st == 0, "the gateway is not in the ARP cache");
    status_of(s, "ping -c 1 -W 1 10.9.9.9 > /dev/null 2>&1", NULL);

    net_step(&s->u, "capture");
    quit(s);
    net_assert(&s->u, "routed-on-wire", on_wire(s, "net0", "routed", routed),
               "the packet for 10.9.9.9 did not go to the gateway's hardware address");
}

/**
 * The `disk` case: two boots of one copy of os-real's disk, where /var/run
 * survives a reboot. `ifup` records there which interfaces it has
 * configured, and the second boot must take a lease rather than read the
 * first boot's record and decide that it is configured already. Init
 * empties that directory, and this is what says so.
 */
static void disk_case(struct dhcp_case *s)
{
    static const char *const boot[] = {"boot-1", "boot-2"};
    static const char *const lease[] = {"lease-1", "lease-2"};
    static const char *const stop[] = {"poweroff-1", "poweroff-2"};
    static const char *const held[] = {"first-lease", "second-lease"};
    unsigned n;
    long st;

    for (n = 0; n < 2; n++) {
        net_step(&s->u, boot[n]);
        boot_login(s, n + 1, ROOT_PROMPT);
        net_step(&s->u, lease[n]);
        st = status_of(s, "ifup -a", NULL);
        if (st == 0)
            net_seen(&s->b, "net: eth0: IPv4 192.168.90.20/");
        if (st == 0)
            st = status_of(s, "ifconfig eth0 | grep -q 'addr:192.168.90.20'", NULL);
        net_assert(&s->u, held[n], st == 0, "boot %u of the disk did not hold the lease", n + 1);
        net_step(&s->u, stop[n]);
        poweroff(s);
    }
}

/**
 * The `unattended` case: the metal image, which is how the sealed machine
 * comes up, with nothing typed at all. Its init runs `ifup -a` itself,
 * after the console login and before the SSH server, so the lease must
 * arrive with no console session in it. What init starts writes to
 * /var/log/services and not to the console, so the console can say only
 * that the address is in force and the machine reached a login. QEMU's user
 * network accepts the host's side of a connection before it knows whether
 * the guest is there, so the server's answer through the forward is what
 * says the guest serves, and the capture must then hold packets sent from
 * the leased address and the client's broadcast to port 67, without which a
 * machine given the address by hand would pass.
 */
static void unattended_case(struct dhcp_case *s)
{
    static const char *const asked[] = {"--port", "67", "--source", "192.168.90.20", NULL};

    net_step(&s->u, "boot");
    net_boot(&s->b, &s->u, MACHINE, 1, NULL);
    net_seen(&s->b, "net: eth0: IPv4 192.168.90.20/");
    net_assert(&s->u, "lease", 1, "");
    net_seen(&s->b, "rach2 login:");
    net_assert(&s->u, "login-prompt", 1, "");
    net_step(&s->u, "server");
    net_assert(&s->u, "server-answers", answers(s, 0, "sshpeer"),
               "the SSH server did not answer through the forward");
    net_step(&s->u, "capture");
    quit(s);
    net_assert(&s->u, "lease-on-wire", on_wire(s, "net0", "lease", asked),
               "the capture has no broadcast to port 67 or no packet from 192.168.90.20");
}

/**
 * The `no-lease` case: the metal image on a card that reaches no server,
 * which is a cable plugged in later or a switch that has not started
 * forwarding. The client must go to the background and keep asking rather
 * than give up, because a machine that gave up is one nobody can reach until
 * somebody walks to it, and the console login must still be there, so that a
 * network that never comes up does not take the local way in with it. The
 * client's giving up is read from the service log, since init gives what it
 * starts /var/log/services, so that a client retrying for minutes does not
 * write over the login prompt.
 */
static void no_lease_case(struct dhcp_case *s)
{
    long st;

    net_step(&s->u, "boot");
    boot_login(s, 1, ROOT_PROMPT);
    net_assert(&s->u, "console-login", 1, "");
    net_step(&s->u, "background");
    st = status_of(s,
                   "until grep -q 'no lease, forking to background' /var/log/services; "
                   "do sleep 1; done",
                   NULL);
    net_assert(&s->u, "client-backgrounded", st == 0,
               "the service log does not say that the client went to the background");
}

/**
 * The `kept` case: the metal image told its address on the command line, on
 * a network with a server ready to hand out another. Init must not run
 * `ifup` there at all: a machine given an address is configured already, and
 * a client would begin by taking that address off and might never put one
 * back. The machine is asked at the console once init has started the SSH
 * server, which it does after the network, because what is tested is that
 * nothing happened.
 */
static void kept_case(struct dhcp_case *s)
{
    long st;

    net_step(&s->u, "boot");
    boot_login(s, 1, ROOT_PROMPT);
    net_step(&s->u, "kept");
    st = status_of(s,
                   "until pidof dropbearmulti > /dev/null; do sleep 1; done; "
                   "ifconfig eth0 | grep -q 'addr:192.168.90.50'",
                   NULL);
    net_assert(&s->u, "address-kept", st == 0,
               "eth0 does not hold 192.168.90.50 once the SSH server runs");
}

/**
 * The `simple` case: os-simple, which carries none of BusyBox, reads the
 * same /etc/network/interfaces with `netup`, and its own `dhcpc` takes the
 * lease, on a /16. That is what makes the netmask provable: the classful
 * mask of a 10.x address is /8, so a client that took the address and not
 * the mask would reach 10.200.0.1 by asking ARP for it directly, and the
 * capture requires that packet to have gone to the gateway instead.
 *
 * Then the way back off a lease. A client holds an address for as long as
 * it renews it, so the address is taken off by stopping its client: `dhcpc
 * -k` signals the one that holds the interface and waits for the claim to
 * go. The ping that follows must fail, which says that the address came off
 * and not just the process; the `netup -a` after it must take a lease again,
 * which says that the claim was released; and the ping after that must
 * answer. A ping that must answer passes on any of its four replies, since
 * the kernel waits a few seconds of the machine's time for each.
 */
static void simple_case(struct dhcp_case *s)
{
    static const char *const routed[] = {
        "--source", "10.90.0.20", "--routed-via", "10.90.0.2", "--routed-dst", "10.200.0.1", NULL};
    long n;

    net_step(&s->u, "boot");
    boot_login(s, 1, ISH_BANNER);
    net_step(&s->u, "lease");
    ish(s, "netup -a", "lease of 10.90.0.20 netmask 255.255.0.0");
    net_assert(&s->u, "lease", 1, "");
    n = received(s, "ping -c 4 10.90.0.2");
    net_assert(&s->u, "peer", n >= 1, "the gateway answered %ld of four pings", n);
    ish(s, "cat /etc/resolv.conf", "nameserver 10.90.0.3");
    net_assert(&s->u, "resolver", 1, "");
    ish(s, "netup -a", "is already holding it");
    net_assert(&s->u, "held", 1, "");
    n = received(s, "ping -c 4 -W 1 10.200.0.1");
    net_assert(&s->u, "off-subnet", n == 0, "10.200.0.1 answered %ld pings", n);

    net_step(&s->u, "release");
    ish(s, "dhcpc -i eth0 -k", "net: eth0: IPv4 none");
    net_wait(&s->b, "stopped process");
    net_assert(&s->u, "released", 1, "");
    n = received(s, "ping -c 4 -W 1 10.90.0.2");
    net_assert(&s->u, "address-gone", n == 0,
               "the gateway answered %ld pings after the client stopped", n);
    ish(s, "netup -a", "lease of 10.90.0.20 netmask 255.255.0.0");
    net_assert(&s->u, "lease-again", 1, "");
    n = received(s, "ping -c 4 10.90.0.2");
    net_assert(&s->u, "peer-again", n >= 1, "the gateway answered %ld of four pings", n);

    net_step(&s->u, "capture");
    quit(s);
    net_assert(&s->u, "routed-on-wire", on_wire(s, "net0", "routed", routed),
               "the capture has no packet from 10.90.0.20, or 10.200.0.1's did not go to "
               "the gateway");
}

/**
 * The `simple-static` case: the fixed address of the file's other method,
 * which is also the sequence an administrator runs to move a machine off
 * DHCP. The configurator must refuse while a client holds the interface,
 * the client must let go when it is stopped, and the fixed address must
 * then be applied and work. os-simple copies the stanza it stages, since it
 * has no editor. The capture says that the address is the fixed one and not
 * the leased one: packets sent from 10.90.0.50, and the packet for an
 * address off the subnet addressed to the gateway's hardware address, the
 * route the stanza asked for. Neither is possible from a machine that still
 * holds the lease.
 */
static void simple_static_case(struct dhcp_case *s)
{
    static const char *const routed[] = {
        "--source", "10.90.0.50", "--routed-via", "10.90.0.2", "--routed-dst", "10.200.0.1", NULL};
    long n;

    net_step(&s->u, "boot");
    boot_login(s, 1, ISH_BANNER);
    net_step(&s->u, "lease");
    ish(s, "netup -a", "lease of 10.90.0.20 netmask 255.255.0.0");
    net_assert(&s->u, "lease", 1, "");

    net_step(&s->u, "static");
    net_line(&s->b, "cp /etc/network/interfaces.static /etc/network/interfaces");
    ish(s, "netup -a", "is holding it with a lease");
    net_assert(&s->u, "refused-while-held", 1, "");
    ish(s, "dhcpc -i eth0 -k", "net: eth0: IPv4 none");
    net_wait(&s->b, "stopped process");
    net_assert(&s->u, "released", 1, "");
    ish(s, "netup -a", "configured from /etc/network/interfaces");
    net_assert(&s->u, "static-applied", 1, "");
    n = received(s, "ping -c 4 10.90.0.2");
    net_assert(&s->u, "peer", n >= 1, "the gateway answered %ld of four pings", n);
    n = received(s, "ping -c 4 -W 1 10.200.0.1");
    net_assert(&s->u, "off-subnet", n == 0, "10.200.0.1 answered %ld pings", n);

    net_step(&s->u, "capture");
    quit(s);
    net_assert(&s->u, "routed-on-wire", on_wire(s, "net0", "routed", routed),
               "the capture has no packet from 10.90.0.50, or 10.200.0.1's did not go to "
               "the gateway");
}

/**
 * The `real-static` case: the same stanza on os-real, through BusyBox's
 * `ifup`, which is why the file has this format: one description of a
 * network, two machines that read it. It is on the same /16 as os-simple's
 * cases, for the same reason: on a 192.168.x network the mask the stanza
 * asks for is the one the kernel would derive anyway, so a client that
 * skipped the netmask would pass there.
 */
static void real_static_case(struct dhcp_case *s)
{
    static const char *const routed[] = {
        "--source", "10.90.0.50", "--routed-via", "10.90.0.2", "--routed-dst", "10.200.0.1", NULL};
    long st;

    net_step(&s->u, "boot");
    boot_login(s, 1, ROOT_PROMPT);
    net_step(&s->u, "static");
    st = status_of(s, REAL_STATIC, NULL);
    if (st == 0)
        st = status_of(s, "ifup -a", NULL);
    if (st == 0)
        net_seen(&s->b, "net: eth0: IPv4 10.90.0.50/");
    net_assert(&s->u, "static-applied", st == 0, "ifup -a did not apply the fixed address");
    st = status_of(s,
                   "ifconfig eth0 > /tmp/if; grep -q 'addr:10.90.0.50' /tmp/if && "
                   "grep -q 'Mask:255.255.0.0' /tmp/if && ping -c 1 10.90.0.2 > /dev/null",
                   NULL);
    net_assert(&s->u, "address-and-mask", st == 0,
               "eth0 does not hold 10.90.0.50/16, or the gateway did not answer");
    status_of(s, "ping -c 1 -W 1 10.200.0.1 > /dev/null 2>&1", NULL);

    net_step(&s->u, "capture");
    quit(s);
    net_assert(&s->u, "routed-on-wire", on_wire(s, "net0", "routed", routed),
               "the capture has no packet from 10.90.0.50, or 10.200.0.1's did not go to "
               "the gateway");
}

/**
 * The `renew` case: the half of the client that only time reaches, holding
 * the lease. QEMU hands out a day, so `-l 20` caps what the client acts on,
 * and the renewal falls due ten seconds in. A renewal and a fresh lease
 * differ, and the difference is what this checks: the client must say that
 * it renewed, twice, and it must have asked for an address exactly once. A
 * client that let every lease lapse and discovered again would print the
 * asking line as often as the renewal.
 */
static void renew_case(struct dhcp_case *s)
{
    char *text;
    unsigned asked;

    net_step(&s->u, "boot");
    boot_login(s, 1, ISH_BANNER);
    net_step(&s->u, "renew");
    net_line(&s->b, "dhcpc -i eth0 -l 20");
    net_wait(&s->b, "renewed for 20 seconds");
    net_wait(&s->b, "renewed for 20 seconds");
    net_assert(&s->u, "renewed", 1, "");
    text = net_ok(&s->u) ? net_text(&s->b, 0, s->b.at) : NULL;
    asked = text != NULL ? net_count(text, "asking for an address") : 0;
    free(text);
    net_assert(&s->u, "one-exchange", asked == 1,
               "the client asked for an address %u times: it started over, not renewed", asked);
}

/**
 * The `two-networks` case: os-real with two cards on two networks, each
 * with a server of its own: a lease on each interface, asked for by name,
 * and the SSH server answering through both. The second client is `udhcpc
 * -i eth1`, which is how an administrator configures an interface that no
 * boot-time file names. Both cards are e1000s, the driver with a record per
 * card.
 *
 * The server's answer through each forward says only that a connection
 * reached it; the capture of each network must hold packets sent from the
 * address that network leased, which only a machine holding that address on
 * that card can send. A TCP that did not take the address a connection
 * arrived on would answer a connection to the second card from the first
 * card's address, and no capture would hold a packet from the second's.
 * The server's own record of who reached it says that the segments crossed
 * the interface, were taken by TCP as addressed to the address that card
 * holds, and reached the program listening.
 */
static void two_networks_case(struct dhcp_case *s)
{
    static const char *const first[] = {"--source", "192.168.90.20", NULL};
    static const char *const second[] = {"--source", "10.0.7.20", NULL};
    char *text = NULL;
    long st;
    int ok;

    net_step(&s->u, "boot");
    boot_login(s, 1, ROOT_PROMPT);
    net_step(&s->u, "leases");
    st = status_of(s, "ifup -a", &text);
    if (st == 0)
        net_seen(&s->b, "net: eth0: IPv4 192.168.90.20/");
    net_assert(&s->u, "first-lease", st == 0 && has(text, "lease of 192.168.90.20 obtained"),
               "ifup -a exited %ld without the lease of 192.168.90.20", st);
    free(text);
    st = status_of(s, "udhcpc -i eth1 -n -q", &text);
    if (st == 0)
        net_seen(&s->b, "net: eth1: IPv4 10.0.7.20/");
    net_assert(&s->u, "second-lease", st == 0 && has(text, "lease of 10.0.7.20 obtained"),
               "udhcpc -i eth1 exited %ld without the lease of 10.0.7.20", st);
    free(text);
    st = status_of(s,
                   "ifconfig eth0 > /tmp/i0; ifconfig eth1 > /tmp/i1; "
                   "grep -q 'addr:192.168.90.20' /tmp/i0 && grep -q 'addr:10.0.7.20' /tmp/i1",
                   NULL);
    net_assert(&s->u, "two-leases", st == 0, "eth0 and eth1 do not hold their two leases");

    net_step(&s->u, "server");
    if (net_line(&s->b, "dropbearmulti dropbear -R -g -p 22") == ETUDE_OK)
        net_wait(&s->b, "Running in background");
    net_assert(&s->u, "dropbear", 1, "");
    ok = answers(s, 0, "sshpeer-0");
    if (ok)
        net_seen(&s->b, "Child connection from 192.168.90.2:");
    net_assert(&s->u, "first-answers", ok, "the server did not answer through the first card");
    ok = answers(s, 1, "sshpeer-1");
    if (ok)
        net_seen(&s->b, "Child connection from 10.0.7.2:");
    net_assert(&s->u, "second-answers", ok, "the server did not answer through the second card");

    net_step(&s->u, "capture");
    quit(s);
    net_assert(&s->u, "first-on-wire", on_wire(s, "net0", "first", first),
               "the first network's capture has no packet from 192.168.90.20");
    net_assert(&s->u, "second-on-wire", on_wire(s, "net1", "second", second),
               "the second network's capture has no packet from 10.0.7.20");
}

/* A case of the families: the end of its identity and its procedure. */
struct dhcp_kind {
    const char *name;
    void (*run)(struct dhcp_case *s);
};

static const struct dhcp_kind kinds[] = {
    {".lease", lease_case},
    {".disk", disk_case},
    {".unattended", unattended_case},
    {".no-lease", no_lease_case},
    {".kept", kept_case},
    {".simple", simple_case},
    {".simple-static", simple_static_case},
    {".real-static", real_static_case},
    {".renew", renew_case},
    {".two-networks", two_networks_case},
};

int etude_scenario(struct etude_case *c)
{
    const char *dot = strrchr(etude_case_id(c), '.');
    struct dhcp_case s;
    size_t i;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        if (dot != NULL && strcmp(dot, kinds[i].name) == 0)
            break;
    }
    if (i == sizeof(kinds) / sizeof(kinds[0])) {
        fprintf(stderr, "dhcp_scenario: no case %s\n", etude_case_id(c));
        return 1;
    }
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    kinds[i].run(&s);
    return net_end(&s.u);
}
