/**
 * Host test for netup's configuration reader (utils/netcfg.c).
 *
 * host-deps: utils/netcfg.c utils/ipcheck.c
 *
 * A machine brings itself up from this file, so a file read wrongly is a
 * machine that comes up wrong or does not come up at all, and the interesting
 * files are the ones nobody would keep in `/etc`: a line with no end, a method
 * this does not implement, a value that is not an address, more interfaces
 * than there is room for. Each is written here and read back through the
 * library. The judgement of a static stanza is checked the same way, because
 * applying half of one leaves a machine with neither the address it had nor
 * the one it was given.
 *
 * Returns the failed-check count (0 == pass).
 */

#include <stdio.h>
#include <string.h>

#include "netcfg.h"

#define FIXDIR "/tmp/rach2_verify"
#define PATH   FIXDIR "/interfaces"

static int fails;

#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

#define IP(a, b, c, d) (((unsigned long)(a) << 24) | ((b) << 16) | ((c) << 8) | (unsigned long)(d))

/** Write one configuration file, and say so if the host will not take it. */
static int put(const char *text)
{
    FILE *f = fopen(PATH, "w");

    if (!f) {
        fails++;
        fprintf(stderr, "  FAIL: cannot write %s\n", PATH);
        return -1;
    }
    fputs(text, f);
    fclose(f);
    return 0;
}

/** Read the file just written, with nothing named on the command line. */
static int read_auto(struct netcfg_iface *want, int max)
{
    memset(want, 0, sizeof(*want) * (size_t)max);
    return netcfg_read(PATH, want, 0, max, 1);
}

static void check_ordinary(void)
{
    struct netcfg_iface want[4];
    int n;

    if (put("auto eth0\n"
            "iface eth0 inet dhcp\n") != 0) {
        return;
    }
    n = read_auto(want, 4);
    CHECK(n == 1, "one auto interface is one interface");
    CHECK(strcmp(want[0].name, "eth0") == 0, "and it is the one named");
    CHECK(want[0].found && want[0].dhcp && !want[0].bad, "read as a dhcp stanza");

    /* The stanza before the line that asks for it: the file does not have to
       be in a helpful order, and a machine that read it in one pass would
       bring up nothing here. */
    if (put("# the network\n"
            "\n"
            "iface eth0 inet static\n"
            "    address 192.168.0.105\n"
            "    netmask 255.255.255.0\n"
            "    gateway 192.168.0.1\n"
            "\n"
            "auto eth0\n") != 0) {
        return;
    }
    n = read_auto(want, 4);
    CHECK(n == 1, "a stanza before its auto line is still brought up");
    CHECK(want[0].found && !want[0].dhcp && !want[0].bad, "read as a static stanza");
    CHECK(want[0].address == IP(192, 168, 0, 105), "the address is read");
    CHECK(want[0].netmask == IP(255, 255, 255, 0), "the netmask is read");
    CHECK(want[0].gateway == IP(192, 168, 0, 1), "the gateway is read");

    /* Two interfaces, one of each, and an `allow-hotplug` line in place of an
       `auto` line, which is what a Debian file often has. */
    if (put("auto eth0\n"
            "allow-hotplug eth1\n"
            "iface eth0 inet dhcp\n"
            "iface eth1 inet static\n"
            "    address 10.0.0.2\n"
            "    netmask 255.0.0.0\n") != 0) {
        return;
    }
    n = read_auto(want, 4);
    CHECK(n == 2, "both interfaces are brought up");
    CHECK(want[0].dhcp && !want[1].dhcp, "each keeps its own method");
    CHECK(want[1].address == IP(10, 0, 0, 2), "the second stanza's address is read");

    /* A named interface is brought up whether or not `auto` names it, and the
       auto line must not add it a second time. */
    memset(want, 0, sizeof(want));
    netcfg_set_name(want[0].name, "eth0");
    n = netcfg_read(PATH, want, 1, 4, 1);
    CHECK(n == 2, "an interface named twice is one interface");

    /* And with add_auto off, only what was named is brought up. */
    memset(want, 0, sizeof(want));
    netcfg_set_name(want[0].name, "eth1");
    n = netcfg_read(PATH, want, 1, 4, 0);
    CHECK(n == 1, "without -a only the interface named is brought up");
    CHECK(want[0].found && want[0].address == IP(10, 0, 0, 2), "and it is read");
}

static void check_ignored(void)
{
    struct netcfg_iface want[4];
    int n;

    if (put("auto eth0\n"
            "iface eth0 inet6 auto\n"
            "iface eth0 inet dhcp\n"
            "iface eth9 inet static\n"
            "    address 1.2.3.4\n") != 0) {
        return;
    }
    n = read_auto(want, 4);
    CHECK(n == 1, "a stanza for an interface nobody asked for is not brought up");
    CHECK(want[0].dhcp && !want[0].bad, "an inet6 stanza for the same name is stepped over");
    CHECK(want[0].address == 0, "and no other stanza's address leaks into it");

    /* A directive outside any stanza this file cares about must not be taken
       for part of the last one it read. */
    if (put("auto eth0\n"
            "iface eth0 inet static\n"
            "    address 10.1.1.1\n"
            "    netmask 255.255.255.0\n"
            "iface eth9 inet static\n"
            "    address 10.9.9.9\n") != 0) {
        return;
    }
    n = read_auto(want, 4);
    CHECK(n == 1 && want[0].address == IP(10, 1, 1, 1),
          "a later stanza does not overwrite the one before it");
}

static void check_refusals(void)
{
    struct netcfg_iface want[2];
    char big[NETCFG_LINE_MAX + 64];
    int n;

    if (put("auto eth0\n"
            "iface eth0 inet manual\n") != 0) {
        return;
    }
    n = read_auto(want, 2);
    CHECK(n == 1 && want[0].bad, "a method this does not implement is refused");

    if (put("auto eth0\n"
            "iface eth0 inet static\n"
            "    address 192.168.0.300\n"
            "    netmask 255.255.255.0\n") != 0) {
        return;
    }
    n = read_auto(want, 2);
    CHECK(n == 1 && want[0].bad, "a value that is not an address is refused");
    CHECK(want[0].address == 0, "and is not half read");

    if (put("auto eth0\n"
            "iface eth0 inet static\n"
            "    address 192.168.0.1.5\n") != 0) {
        return;
    }
    n = read_auto(want, 2);
    CHECK(n == 1 && want[0].bad, "a dotted quintet is not an address");

    /* A line longer than the buffer would otherwise be split, and the second
       half read as a directive of its own. */
    memset(big, 'x', sizeof(big));
    big[sizeof(big) - 1] = '\0';
    {
        char text[NETCFG_LINE_MAX + 128];

        snprintf(text, sizeof(text), "auto eth0\n# %s\niface eth0 inet dhcp\n", big);
        if (put(text) != 0) {
            return;
        }
    }
    n = read_auto(want, 2);
    CHECK(n == -1, "a line longer than the buffer is refused, not split");

    if (put("auto eth0 eth1 eth2\n") != 0) {
        return;
    }
    n = read_auto(want, 2);
    CHECK(n == -1, "more interfaces than there is room for is refused");

    n = netcfg_read(FIXDIR "/no-such-file", want, 0, 2, 1);
    CHECK(n == -1, "a file that is not there is refused");

    if (put("# nothing but a comment\n") != 0) {
        return;
    }
    n = read_auto(want, 2);
    CHECK(n == 0, "a file that names no interface brings none up");
}

static void check_names(void)
{
    struct netcfg_iface want[2];
    char name[NETCFG_NAME_MAX];
    int n;

    netcfg_set_name(name, "abcdefghijklmnopqrstuvwxyz");
    CHECK(strlen(name) == NETCFG_NAME_MAX - 1, "a long name is cut to the field");
    netcfg_set_name(name, "eth0");
    CHECK(strcmp(name, "eth0") == 0, "a short one is copied whole");

    CHECK(!netcfg_name_ok("abcdefghijklmnop"), "sixteen characters do not fit the field");
    CHECK(netcfg_name_ok("abcdefghijklmno"), "fifteen do");
    CHECK(!netcfg_name_ok(""), "and a name of nothing is not a name");

    /* A name longer than the field would be stored cut and looked for whole,
       so its stanza would never be found. No interface can have such a name,
       so the file is wrong and says so. */
    if (put("auto verylonginterfacename0\n"
            "iface verylonginterfacename0 inet dhcp\n") != 0) {
        return;
    }
    n = read_auto(want, 2);
    CHECK(n == -1, "a name longer than the field is refused");

    if (put("auto eth0\n"
            "iface verylonginterfacename0 inet dhcp\n") != 0) {
        return;
    }
    n = read_auto(want, 2);
    CHECK(n == -1, "and refused on the stanza as well as the auto line");
}

/* The refusal must be the one the case is about: a mutation that trips a
   different rule would otherwise pass for having refused at all. */
static char refusal[96];

static int refused(const struct netcfg_iface *s, const char *substring)
{
    refusal[0] = '\0';
    if (netcfg_check_static(s, refusal, sizeof(refusal)) == 0) {
        return 0;
    }
    return strstr(refusal, substring) != NULL;
}

static void check_static_judgement(void)
{
    struct netcfg_iface s;

    memset(&s, 0, sizeof(s));
    netcfg_set_name(s.name, "eth0");
    s.address = IP(192, 168, 0, 105);
    s.netmask = IP(255, 255, 255, 0);
    s.gateway = IP(192, 168, 0, 1);
    CHECK(netcfg_check_static(&s, refusal, sizeof(refusal)) == 0,
          "an ordinary static stanza is applied");

    s.gateway = 0;
    CHECK(netcfg_check_static(&s, refusal, sizeof(refusal)) == 0,
          "a stanza with no gateway is applied");

    s.address = 0;
    CHECK(refused(&s, "no address"), "a static stanza with no address is refused");

    s.address = IP(192, 168, 0, 105);
    s.netmask = 0;
    CHECK(refused(&s, "no netmask"), "a static stanza with no netmask is refused");

    s.netmask = IP(255, 255, 0, 255);
    CHECK(refused(&s, "run of ones"), "a netmask that is not a run of ones is refused");

    s.netmask = 0xFFFFFFFFUL;
    CHECK(refused(&s, "run of ones"), "a netmask with no room for a host is refused");

    s.netmask = IP(255, 255, 255, 0);
    s.address = IP(192, 168, 0, 0);
    CHECK(refused(&s, "network or broadcast"), "the subnet's own address is not a host");

    s.address = IP(192, 168, 0, 255);
    CHECK(refused(&s, "network or broadcast"), "the subnet's broadcast address is not a host");

    s.address = IP(192, 168, 0, 105);
    s.gateway = IP(192, 168, 1, 1);
    CHECK(refused(&s, "gateway is not a host"), "a gateway on another subnet is refused");

    s.gateway = s.address;
    CHECK(refused(&s, "gateway is not a host"), "a gateway that is this machine is refused");

    s.gateway = IP(192, 168, 0, 0);
    CHECK(refused(&s, "gateway is not a host"),
          "a gateway that is the subnet's address is refused");

    s.gateway = IP(192, 168, 0, 255);
    CHECK(refused(&s, "gateway is not a host"),
          "a gateway that is the broadcast address is refused");

    /* The addresses no machine can hold, which the kernel refuses too. A wide
       enough mask puts one of them on the same subnet as the address, so the
       on-subnet rule alone would let it through. */
    s.address = IP(10, 0, 0, 5);
    s.netmask = IP(128, 0, 0, 0);
    s.gateway = IP(127, 0, 0, 1);
    CHECK(refused(&s, "not an address a machine can hold"),
          "a loopback gateway is refused however wide the mask");
    s.gateway = IP(224, 0, 0, 1);
    CHECK(refused(&s, "not an address a machine can hold"), "a multicast gateway too");
    s.gateway = IP(10, 0, 0, 6);
    s.address = IP(127, 0, 0, 2);
    CHECK(refused(&s, "not one a machine can hold"), "and a loopback address");
    s.address = IP(0, 1, 2, 3);
    CHECK(refused(&s, "not one a machine can hold"), "and one on network zero");

    /* A /31 has no host addresses at all, and a /30 has two. */
    s.address = IP(10, 0, 0, 1);
    s.netmask = IP(255, 255, 255, 254);
    s.gateway = 0;
    CHECK(refused(&s, "network or broadcast"), "a subnet with no room for a host is refused");
    s.netmask = IP(255, 255, 255, 252);
    CHECK(netcfg_check_static(&s, refusal, sizeof(refusal)) == 0,
          "a four-address subnet has two hosts in it");
}

int main(void)
{
    check_ordinary();
    check_ignored();
    check_refusals();
    check_names();
    check_static_judgement();
    return fails > 120 ? 120 : fails;
}
