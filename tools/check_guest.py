#!/usr/bin/env python3
"""Hold the guest makefiles equal to the host build they mirror.

The guest builds (rach2/guest.mk, utils/guest.mk, ish/guest.mk) name their
sources by hand, because pdpmake reads none of the GNU makefiles that are the
host build's authority. A list that drifts builds a kernel missing a file or
a program missing its companion source, and the failure would surface inside
a booted guest, late and hard to read. This guard runs on the host, inside
the verify gate, and has the four contracts:

  1. rach2/guest.mk's OBJS equals the host ASM_SRC plus C_SRC, object for
     object, and the kernel.objs recipe writes exactly that list, loader.o
     first, no duplicates, with the link reading @kernel.objs plus the tcc
     runtime at the pinned text address.
  2. utils/guest.mk's per-program source map equals the host utils/Makefile
     map, including the companion sources (pwauth.c, resolv.c) and the two
     -D builds (init-login, adduser-crash).
  3. ish/guest.mk's object list equals the host ish/Makefile's OBJECTS.
  4. The build fits the machine: the deepest kernel include chain leaves
     room in the 16-descriptor table for the compiler's own files, and no
     guest recipe line approaches the 3072-byte exec string budget.
  5. The os-real manifest stages all three source trees at /src, root-owned
     and read-only, so the machine actually has what the makefiles
     build, and each staged tree has its guest.mk.

Usage: check_guest.py [REPO_ROOT]
"""

import os
import re
import subprocess
import sys

FD_LIMIT = 16
FD_RESERVED = 4  # stdin, stdout, stderr, and the output file
# The kernel's execve budget is 3072 bytes across the argv and environment
# strings together. A recipe reaches the kernel as `sh -c <expanded text>`
# plus the build environment, so the check below expands each recipe the way
# pdpmake will and has the result under the budget minus this allowance for
# the environment a login shell exports.
EXEC_BUDGET = 3072
ENV_ALLOWANCE = 500


def fail(reason):
    print("check-guest: fail: %s" % reason, file=sys.stderr)
    sys.exit(1)


def make_list(repo, target):
    out = subprocess.run(["make", "-s", "-C", os.path.join(repo, "rach2"),
                          target], capture_output=True, text=True)
    if out.returncode != 0:
        fail("make %s: %s" % (target, out.stderr.strip()))
    return out.stdout.split()


def read_macro(text, name):
    """Return the whitespace-split value of a backslash-continued macro."""
    lines = text.split("\n")
    for index, line in enumerate(lines):
        match = re.match(r"%s\s*=\s*(.*)$" % re.escape(name), line)
        if match:
            break
    else:
        fail("no %s macro" % name)
    value = []
    current = match.group(1)
    while True:
        continued = current.endswith("\\")
        if continued:
            current = current[:-1]
        value.extend(current.split())
        if not continued:
            break
        index += 1
        current = lines[index].strip()
    return value


def rules_of(text):
    """Return `{target: (prerequisites, recipe_lines)}` for explicit rules."""
    rules = {}
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        if (line and not line.startswith("\t") and not line.startswith("#")
                and ":" in line and "=" not in line.split(":")[0]):
            target, _, rest = line.partition(":")
            target = target.strip()
            prereqs = rest.split()
            recipe = []
            j = i + 1
            while j < len(lines) and (lines[j].startswith("\t")
                                      or not lines[j]):
                if lines[j].startswith("\t"):
                    recipe.append(lines[j][1:])
                j += 1
            if not target.startswith("."):
                rules[target] = (prereqs, recipe)
            i = j
        else:
            i += 1
    return rules


def check_kernel_lists(repo):
    sources = make_list(repo, "print-asm-src") + make_list(repo,
                                                           "print-c-src")
    expected = [re.sub(r"\.[cs]$", ".o", s) for s in sources]

    text = open(os.path.join(repo, "rach2", "guest.mk")).read()
    objs = read_macro(text, "OBJS")
    if objs != expected:
        missing = sorted(set(expected) - set(objs))
        extra = sorted(set(objs) - set(expected))
        fail("rach2/guest.mk OBJS differs from the host list "
             "(missing: %s; extra: %s; or the order differs)"
             % (", ".join(missing) or "none", ", ".join(extra) or "none"))

    rules = rules_of(text)
    if "kernel.objs" not in rules:
        fail("rach2/guest.mk has no kernel.objs rule")
    if "guest.mk" not in rules["kernel.objs"][0]:
        fail("kernel.objs does not depend on guest.mk, so an edited object "
             "list would link through the stale response file")
    recipe = rules["kernel.objs"][1]
    if recipe[-1] != "mv kernel.objs.new kernel.objs":
        fail("kernel.objs is not published by renaming the temporary file")
    written = []
    for number, line in enumerate(recipe[:-1]):
        wanted = ("> kernel.objs.new" if number == 0
                  else ">> kernel.objs.new")
        match = re.match(r"printf '%s\\n' (.*?) (>>?) kernel\.objs\.new$",
                         line)
        if not match or (match.group(2) + " kernel.objs.new") != wanted:
            fail("kernel.objs recipe line %d must end with '%s': %s"
                 % (number + 1, wanted, line))
        written.extend(match.group(1).split())
    if written != objs:
        fail("kernel.objs writes a different list than OBJS")
    if written[0] != "loader.o":
        fail("kernel.objs does not start with loader.o")
    if len(written) != len(set(written)):
        fail("kernel.objs writes a duplicate object")

    link = " ".join(rules.get("kernel", ([], []))[1])
    for needed in ("@kernel.objs", "-Wl,-Ttext=0x100074",
                   "/lib/tcc/libtcc1.a", "-nostdlib", "-static",
                   "-o kernel.new", "mv kernel.new kernel"):
        if needed not in link:
            fail("the kernel link line lacks %s" % needed)
    return sources


def host_utils_map(repo):
    text = open(os.path.join(repo, "utils", "Makefile")).read()
    programs = read_macro(text, "UTILITIES") + read_macro(text, "SBIN_PROGS")
    mapping = {}
    for target, (prereqs, _recipe) in rules_of(text).items():
        match = re.match(r"\$\(BUILD\)/(?:sbin/)?([\w-]+)$", target)
        if match:
            for token in prereqs:
                if "$" in token:
                    fail("utils/Makefile %s has a prerequisite this guard "
                         "cannot resolve (%s); teach the guard before "
                         "using make syntax there" % (target, token))
            mapping[match.group(1)] = [p for p in prereqs
                                       if p.endswith(".c")]
    result = {}
    for program in programs:
        result[program] = mapping.get(program) or [program + ".c"]
    return result


def check_utils(repo):
    expected = host_utils_map(repo)
    text = open(os.path.join(repo, "utils", "guest.mk")).read()
    rules = rules_of(text)
    progs = read_macro(text, "PROGS")
    if sorted(progs) != sorted(expected):
        fail("utils/guest.mk PROGS differs from the host program list "
             "(host has %d, guest has %d)" % (len(expected), len(progs)))
    for program, sources in sorted(expected.items()):
        if program not in rules:
            fail("utils/guest.mk has no rule for %s" % program)
        got = [p for p in rules[program][0] if p.endswith(".c")]
        if sorted(got) != sorted(sources):
            fail("utils/guest.mk %s builds from %s; the host builds from %s"
                 % (program, ", ".join(got), ", ".join(sources)))
        compiled = [t for t in " ".join(rules[program][1]).split()
                    if t.endswith(".c")]
        if sorted(compiled) != sorted(sources):
            fail("utils/guest.mk %s compiles %s in its recipe; the host "
                 "compiles %s" % (program, ", ".join(compiled) or "nothing",
                                  ", ".join(sources)))
    for program, define in (("init-login", "-DINIT_CONSOLE_LOGIN"),
                            ("adduser-crash", "-DADDUSER_CRASH_POINT")):
        if define not in " ".join(rules[program][1]):
            fail("utils/guest.mk %s lacks %s" % (program, define))
    return len(expected)


def check_ish(repo):
    text = open(os.path.join(repo, "ish", "Makefile")).read()
    match = re.search(r"OBJECTS = \$\(addprefix \$\(BUILD\)/, (.*?)\)",
                      text, re.S)
    if not match:
        fail("could not read ish/Makefile OBJECTS")
    expected = match.group(1).replace("\\", " ").split()
    guest = read_macro(open(os.path.join(repo, "ish", "guest.mk")).read(),
                       "OBJS")
    if sorted(guest) != sorted(expected):
        fail("ish/guest.mk OBJS differs from the host list")
    return len(expected)


def include_depth(repo):
    """The deepest quoted-include chain in the kernel sources."""
    root = os.path.join(repo, "rach2")
    edges = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != "build"]
        for name in filenames:
            if not name.endswith((".c", ".h")):
                continue
            path = os.path.join(dirpath, name)
            includes = []
            for line in open(path, errors="replace"):
                match = re.match(r'\s*#\s*include\s*"([^"]+)"', line)
                if match:
                    resolved = os.path.normpath(
                        os.path.join(dirpath, match.group(1)))
                    includes.append(resolved)
            edges[os.path.normpath(path)] = includes

    depths = {}

    def depth_of(path, trail):
        if path in trail:
            return 0  # a cycle; the compiler's guards end it
        if path in depths:
            return depths[path]
        best = 0
        for child in edges.get(path, []):
            best = max(best, depth_of(child, trail | {path}))
        depths[path] = best + 1
        return depths[path]

    return max((depth_of(p, frozenset()) for p in edges if p.endswith(".c")),
               default=0)


def check_limits(repo):
    depth = include_depth(repo)
    if depth + FD_RESERVED > FD_LIMIT:
        fail("the deepest kernel include chain is %d files; with the %d "
             "descriptors a compile has besides its includes that exceeds "
             "the %d-descriptor table" % (depth, FD_RESERVED, FD_LIMIT))
    for name in ("rach2/guest.mk", "utils/guest.mk", "ish/guest.mk"):
        text = open(os.path.join(repo, name)).read()
        macros = {}
        for line in text.split("\n"):
            match = re.match(r"(\w+)\s*=", line)
            if match:
                macros[match.group(1)] = " ".join(
                    read_macro(text, match.group(1)))
        for target, (prereqs, recipe) in rules_of(text).items():
            for line in recipe:
                expanded = line
                for _pass in range(3):
                    for key, value in macros.items():
                        expanded = expanded.replace("$(%s)" % key, value)
                expanded = expanded.replace("$@", target)
                expanded = expanded.replace(
                    "$<", prereqs[0] if prereqs else "")
                if len(expanded) + ENV_ALLOWANCE > EXEC_BUDGET:
                    fail("%s: the %s recipe expands to %d bytes, which with "
                         "the %d-byte environment allowance exceeds the "
                         "%d-byte exec budget"
                         % (name, target, len(expanded), ENV_ALLOWANCE,
                            EXEC_BUDGET))
    return depth


def check_staging(repo):
    manifest = open(os.path.join(
        repo, "krffs", "manifests", "os-real.manifest")).read()
    expected = {"tree 0644 /src/rach2 rach2",
                "tree 0644 /src/utils utils",
                "tree 0644 /src/ish ish"}
    seen = set()
    for raw in manifest.split("\n"):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) < 3:
            continue
        guest_path = fields[2]
        if guest_path == "/src" or guest_path.startswith("/src/"):
            if line in expected:
                seen.add(line)
            elif line in ("dir 0755 /src", "dir 0755 /src/utils",
                          "dir 0755 /src/rach2", "dir 0755 /src/rach2/sys",
                          "dir 0755 /src/ish"):
                pass
            else:
                fail("os-real.manifest directive '%s' touches /src; only "
                     "the three root-owned read-only source trees may live "
                     "there, or a later line could hand the shared source "
                     "to another owner" % line)
    for line in sorted(expected - seen):
        fail("os-real.manifest does not stage '%s' (root-owned, 0644)"
             % line)
    for host_dir in ("rach2", "utils", "ish"):
        if not os.path.isfile(os.path.join(repo, host_dir, "guest.mk")):
            fail("%s/guest.mk is missing or not a regular file, so the "
                 "staged tree could not be built in the guest" % host_dir)


def main():
    repo = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))
    sources = check_kernel_lists(repo)
    programs = check_utils(repo)
    ish_objects = check_ish(repo)
    depth = check_limits(repo)
    check_staging(repo)
    print("check-guest: ok: %d kernel sources, %d programs, %d ish objects, "
          "include depth %d, three /src trees staged"
          % (len(sources), programs, ish_objects, depth))


if __name__ == "__main__":
    main()
