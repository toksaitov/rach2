#!/usr/bin/env python3
"""Check the sealed disk's fixed address against the script that falls back to it.

The sealed disk names its fixed address twice, and in two shapes. The two static
menu entries carry `rach2.ip=ADDRESS/PREFIX rach2.gateway=GATEWAY`, which is what
the kernel parses at boot. `krffs/rescue-static.script` has the same address
as a dotted mask and applies it from userland when a recovery boot gets no lease.
Two shapes of one fact drift, and a machine whose menu and fallback disagree is
one that is unreachable by whichever of the two is wrong.

So this holds them together before a disk is written: the prefix and the mask
must describe the same subnet, the address must be a host in it, the gateway
must be a different host in the same subnet, and the script must RUN exactly
those numbers. What is compared is the script's commands and not its text: a
commented-out line is an absent one, and a match on part of an address would
accept a script that configures a different machine.

Usage: check_sealednet.py ADDRESS PREFIX MASK GATEWAY SCRIPT
"""

import re
import sys


def quad(text):
    parts = text.split(".")
    if len(parts) != 4:
        return None
    value = 0
    for p in parts:
        if not p.isdigit() or len(p) > 3:
            return None
        n = int(p)
        if n > 255:
            return None
        value = (value << 8) | n
    return value


def network_commands(text, want):
    """The network commands the script would run, or a refusal.

    Every segment of every uncommented line, and not the first of each: a line
    can carry more than one command, and a checker that read only the first
    would pass a script whose second command configured something else.

    It fails closed rather than pattern-matching. A segment that mentions
    `ifconfig` or `route` at all must be one of three exact forms: the read of
    the current address that the fallback tests, and the two commands this
    machine is supposed to run. Anything else, including a command hidden
    behind `command`, `eval`, or a subshell, is refused rather than ignored,
    because a checker that ignores what it does not recognise is a checker that
    passes what it should stop.

    @return (commands, None) or (None, why).
    """
    read_form = ["ifconfig", "eth0"]
    found = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        for segment in re.split(r"\|\||&&|;|\||\bthen\b|\belse\b|\bdo\b", line):
            plain = re.sub(r"[(){}`]", " ", segment)
            if "ifconfig" not in plain and "route" not in plain:
                continue
            tokens = plain.split()
            if tokens == read_form:
                continue
            if tokens in want:
                found.append(tokens)
                continue
            return None, "has a network command this does not recognise: %r" % segment
    return found, None


def commands_disagree(text, address_s, mask_s, gateway_s):
    """Why the script does not configure exactly that, or None when it does."""
    want = [
        ["ifconfig", "eth0", address_s, "netmask", mask_s],
        ["route", "add", "default", "gw", gateway_s],
    ]
    found, why = network_commands(text, want)
    if why:
        return why
    # The `|| exit 1` each of them has is a separator away and never reaches
    # the tokens; anything else on the line is another command and is counted.
    if found != want:
        return "runs %r, not exactly %r" % (found, want)
    return None


CASES = [
    ("the script as it is",
     "ifconfig eth0 A netmask M || exit 1\nroute add default gw G || exit 1\n", True),
    ("comments and a pipeline around it",
     "# a comment\nifconfig eth0 | grep -q x || exit 0\n"
     "ifconfig eth0 A netmask M || exit 1\nroute add default gw G || exit 1\n", True),
    ("a second command after a semicolon",
     "ifconfig eth0 A netmask M; ifconfig eth0 B netmask M\n"
     "route add default gw G || exit 1\n", False),
    ("a second command after &&",
     "ifconfig eth0 A netmask M && ifconfig eth0 B netmask M\n"
     "route add default gw G || exit 1\n", False),
    ("the address commented out",
     "# ifconfig eth0 A netmask M\nroute add default gw G || exit 1\n", False),
    ("a different live gateway",
     "ifconfig eth0 A netmask M\nroute add default gw H\n", False),
    ("a trailing token",
     "ifconfig eth0 A netmask M mtu 1400\nroute add default gw G\n", False),
    ("a route removed as well",
     "ifconfig eth0 A netmask M\nroute add default gw G\nroute del default gw G\n", False),
    ("a command hidden behind `command`",
     "ifconfig eth0 A netmask M\nroute add default gw G\n"
     "command ifconfig eth0 B netmask M\n", False),
    ("a command in a subshell",
     "ifconfig eth0 A netmask M\nroute add default gw G\n"
     "(ifconfig eth0 B netmask M)\n", False),
    ("a command through eval",
     "ifconfig eth0 A netmask M\nroute add default gw G\n"
     "eval ifconfig eth0 B netmask M\n", False),
    ("a command in backticks",
     "ifconfig eth0 A netmask M\nroute add default gw G\n"
     "X=`ifconfig eth0 B netmask M`\n", False),
    ("nothing at all", "", False),
]


def selftest():
    bad = 0
    for name, text, want_ok in CASES:
        body = text.replace(" A ", " 10.0.0.5 ").replace(" M", " 255.0.0.0") \
                   .replace(" G", " 10.0.0.1").replace(" B ", " 10.0.0.6 ") \
                   .replace(" H", " 10.0.0.2")
        ok = commands_disagree(body, "10.0.0.5", "255.0.0.0", "10.0.0.1") is None
        if ok != want_ok:
            bad += 1
            print("check-sealednet: selftest: fail: %s" % name, file=sys.stderr)
    if bad:
        return 1
    print("check-sealednet: selftest: ok: %d accepted, %d refused"
          % (sum(1 for c in CASES if c[2]), sum(1 for c in CASES if not c[2])))
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--selftest":
        return selftest()
    if len(argv) != 6:
        sys.exit("usage: check_sealednet.py ADDRESS PREFIX MASK GATEWAY SCRIPT")
    address_s, prefix_s, mask_s, gateway_s, path = argv[1:]

    address = quad(address_s)
    mask = quad(mask_s)
    gateway = quad(gateway_s)
    if address is None:
        sys.exit("check-sealednet: %s is not an address" % address_s)
    if mask is None:
        sys.exit("check-sealednet: %s is not a netmask" % mask_s)
    if gateway is None:
        sys.exit("check-sealednet: %s is not an address" % gateway_s)
    if not prefix_s.isdigit() or not 1 <= int(prefix_s) <= 30:
        sys.exit("check-sealednet: %s is not a usable prefix length" % prefix_s)
    prefix = int(prefix_s)

    from_prefix = (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF
    if from_prefix != mask:
        sys.exit("check-sealednet: /%d is not %s; the menu and the recovery script "
                 "would put this machine on two different subnets" % (prefix, mask_s))
    inverted = ~mask & 0xFFFFFFFF
    if address in (address & mask, address | inverted):
        sys.exit("check-sealednet: %s is this subnet's network or broadcast address"
                 % address_s)
    if gateway == address:
        sys.exit("check-sealednet: the gateway is the machine's own address")
    if (gateway & mask) != (address & mask) or gateway in (gateway & mask,
                                                           gateway | inverted):
        sys.exit("check-sealednet: %s is not a host on %s/%d, so the kernel would "
                 "refuse the static entries" % (gateway_s, address_s, prefix))

    try:
        with open(path, "r", encoding="ascii") as f:
            text = f.read()
    except OSError as e:
        sys.exit("check-sealednet: cannot read %s: %s" % (path, e))

    why = commands_disagree(text, address_s, mask_s, gateway_s)
    if why:
        sys.exit("check-sealednet: %s %s" % (path, why))

    print("check-sealednet: OK (%s/%d through %s, and %s says the same)"
          % (address_s, prefix, gateway_s, path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
