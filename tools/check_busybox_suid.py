#!/usr/bin/env python3
"""
check_busybox_suid.py: prove the os-real image's setuid busybox is confined.

/bin/busybox ships setuid root so a login shell can re-exec it for `su`. Every
BusyBox applet a non-root caller can reach through that binary is a potential
way to keep root: for an applet BusyBox marks BB_SUID_REQUIRE, its fallback
keeps the effective uid when the binary is setuid; for BB_SUID_MAYBE there is
no drop path at all. The only thing standing between that and a privilege
escalation is /etc/busybox.conf, which must name every such enabled applet and
grant the setuid bit to `su` alone.

This is easy to get wrong and easy to regress: enabling one more applet in
rach2.config silently reopens the hole. So this check recomputes the set of
enabled BB_SUID_REQUIRE and BB_SUID_MAYBE applets from the vendored source and
config, parses the [SUID] section staged into the os-real manifest, and fails
unless su is granted and every other such applet is present and dropped. The
os-real image targets depend on it, so an incomplete confinement fails the
build rather than shipping.

The check also fails if the staged conf would be unusable by BusyBox at all
(a mode/owner it refuses, or a mode string it cannot parse), since an ignored
conf confines nobody.

Usage: check_busybox_suid.py <busybox-src-dir> <rach2.config> <manifest>
"""
import re
import sys


def enabled_applets(config_path):
    """The set of applet config symbols set to y (e.g. {'SU', 'PING', ...})."""
    on = set()
    desktop = False
    for line in open(config_path):
        m = re.match(r"CONFIG_([A-Z0-9_]+)=y\s*$", line)
        if m:
            on.add(m.group(1))
            if m.group(1) == "DESKTOP":
                desktop = True
    return on, desktop


def applet_suid_classes(src_dir, on, desktop):
    """Map each enabled applet name to its effective SUID class, reading the
    //applet: declaration lines in the source. Handles the IF_DESKTOP(...)
    IF_NOT_DESKTOP(...) split on the SUID field."""
    import os

    decl = re.compile(
        r"//applet:.*APPLET(?:_NOEXEC|_NOFORK)?\(\s*([a-z0-9_]+)\s*,")
    classes = {}
    for root, _dirs, files in os.walk(src_dir):
        for fn in files:
            if not fn.endswith(".c"):
                continue
            for line in open(os.path.join(root, fn), errors="replace"):
                if "//applet:" not in line or "APPLET" not in line:
                    continue
                m = decl.search(line)
                if not m:
                    continue
                name = m.group(1)
                if "BB_SUID_REQUIRE" in line:
                    cls = "REQUIRE"
                elif "IF_DESKTOP(BB_SUID_MAYBE)" in line:
                    cls = "MAYBE" if desktop else "DROP"
                elif "BB_SUID_MAYBE" in line:
                    cls = "MAYBE"
                else:
                    cls = "DROP"
                classes[name] = cls
    # Keep only applets whose config symbol is enabled. The symbol is usually
    # the applet name upper-cased; a few differ, but the ones that matter here
    # (su, passwd, login, vlock, crontab, wall, ping, ping6, traceroute,
    # traceroute6, findfs, mount) follow the rule.
    return {n: c for n, c in classes.items()
            if n.upper().replace("-", "_") in on}


def staged_busybox_conf(manifest_path):
    """Find the manifest entry that stages /etc/busybox.conf and return its
    (mode_field, body). The entry is `text <mode> /etc/busybox.conf <body>`,
    where <mode> is octal permissions with an optional :uid:gid."""
    for line in open(manifest_path):
        s = line.strip()
        parts = s.split(None, 3)
        if len(parts) == 4 and parts[0] in ("text", "file") \
                and parts[2] == "/etc/busybox.conf":
            return parts[1], parts[3]
    sys.exit("check_busybox_suid: no /etc/busybox.conf entry in the manifest")


def conf_usable(mode_field):
    """BusyBox ignores /etc/busybox.conf unless it is a regular file owned by
    root and not group- or other-writable (appletlib.c). A conf it ignores
    drops nobody, so the whole confinement silently fails. Confirm the staged
    mode and owner satisfy that, returning a list of problems."""
    problems = []
    bits, _, owner = mode_field.partition(":")
    try:
        m = int(bits, 8)
    except ValueError:
        return ["/etc/busybox.conf mode %r is not octal" % bits]
    if m & 0o022:
        problems.append("/etc/busybox.conf mode %s is group- or other-writable; "
                        "busybox would ignore it" % bits)
    if owner and owner != "0:0":
        problems.append("/etc/busybox.conf owner %s is not root; busybox would "
                        "ignore it" % owner)
    return problems


def mode_syntax_ok(mode):
    """A [SUID] value is exactly three characters busybox accepts: user and
    group from {S,s,x,-}, other from {x,-}. A stray character is a parse error
    that makes busybox reject the entire file, not just the line."""
    return (len(mode) == 3 and mode[0] in "Ssx-"
            and mode[1] in "Ssx-" and mode[2] in "x-")


def conf_suid_section(body):
    """Parse the [SUID] lines of the conf body into {applet: mode_string}."""
    entries = {}
    in_suid = False
    for raw in body.split("\\n"):
        t = raw.strip()
        if not t or t.startswith("#"):
            continue
        if t.startswith("["):
            in_suid = (t.lower() == "[suid]")
            continue
        if in_suid and "=" in t:
            key, val = t.split("=", 1)
            entries[key.strip()] = val.strip()
    return entries


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: check_busybox_suid.py <src-dir> <config> <manifest>")
    src_dir, config_path, manifest_path = sys.argv[1:4]

    on, desktop = enabled_applets(config_path)
    classes = applet_suid_classes(src_dir, on, desktop)
    mode_field, body = staged_busybox_conf(manifest_path)
    conf = conf_suid_section(body)

    # Every enabled REQUIRE or MAYBE applet is one a setuid busybox would run
    # with root unless the config drops it. su is the sanctioned exception.
    privileged = {n for n, c in classes.items() if c in ("REQUIRE", "MAYBE")}

    # A conf busybox refuses (bad mode/owner) or cannot parse (bad mode syntax)
    # confines nobody, so check those before trusting the listed entries.
    problems = conf_usable(mode_field)
    for name, mode in sorted(conf.items()):
        if not mode_syntax_ok(mode):
            problems.append("%s mode %r is not a valid busybox SUID spec; the "
                            "whole conf would be rejected" % (name, mode))

    if "su" not in conf:
        problems.append("su is not granted the setuid bit in /etc/busybox.conf")
    elif "s" not in conf["su"].lower():
        problems.append("su's mode %r does not set the setuid bit" % conf["su"])

    for name in sorted(privileged):
        if name == "su":
            continue
        if name not in conf:
            problems.append(
                "%s (%s) is not confined in /etc/busybox.conf; a setuid busybox "
                "runs it as root" % (name, classes[name]))
        elif "s" in conf[name].lower():
            problems.append(
                "%s is listed but its mode %r grants a set-id bit"
                % (name, conf[name]))

    if problems:
        sys.stderr.write("check_busybox_suid: FAIL\n")
        for p in problems:
            sys.stderr.write("  - " + p + "\n")
        return 1

    kept = sorted(n for n in privileged if n != "su")
    print("check_busybox_suid: OK (su granted; %d applet(s) confined: %s)"
          % (len(kept), ", ".join(kept)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
