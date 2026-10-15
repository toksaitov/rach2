#!/usr/bin/env sh
#
# programs.sh: write man/programs.md, the list of the programs on each system
# and of the vendored projects, to standard output.
#
# Usage: programs.sh [APPLETS]
#
# Every fact comes from a file the build reads: the programs from the image
# manifests and utils/programs.mk, the BusyBox applets from APPLETS, the
# applet table of the BusyBox build (build/vendor/busybox/applet_tables-main.h
# unless named), the pages from man/, the GNU ports from their recipes, and
# the versions from the build scripts, the GNU layer's source lists and the
# GRUB manifest. The licenses and the purposes of the five GNU programs that
# are not ports, which no file states in a form a script can read, are the
# tables LICENSES and GNU_TOOLS below, taken from the projects' Readmes.
# `make programs-md` runs this script into man/programs.md, and `tests/run
# style` fails when the committed file differs from its output. The C locale
# makes the order of the rows the same on every host.
#

set -eu
LC_ALL=C
export LC_ALL

cd "$(dirname "$0")/.." || exit 1
APPLETS=${1:-build/vendor/busybox/applet_tables-main.h}
if [ ! -r "$APPLETS" ]; then
    echo "programs: $APPLETS: cannot read the applet table; build BusyBox first" >&2
    exit 1
fi

# Each vendored project as NAME|README|LICENSE, in the order the table has.
LICENSES='musl|vendor/musl/Readme.md|MIT
BusyBox|vendor/busybox/Readme.md|GPL 2 only
Dropbear|vendor/dropbear/Readme.md|MIT-style
TinyCC|vendor/tcc/Readme.md|LGPL 2.1
pdpmake|vendor/pdpmake/Readme.md|public domain
e2fsprogs|vendor/e2fsprogs/Readme.md|GPL 2; its libraries LGPL 2, BSD-style and MIT-style
GRUB|vendor/grub/Readme.md|GPL 3 or later
doomgeneric|ports/doom/Readme.md|GPL 2 or later, with Nuked OPL3 under LGPL 2.1 or later
GNU ports|vendor/gnu/Readme.md|each port its own, which its tarball has
binutils, GCC, GDB|vendor/gnu/native/Readme.md|GPL 3 or later
SpiderMonkey|vendor/gnu/spidermonkey/Readme.md|MPL 2.0
chromium-headless-shell|vendor/gnu/chromium/Readme.md|BSD 3-clause
termbox2|krffs/Readme.md|MIT
Multiboot header|rach2/vendor/multiboot.h|MIT-style'

# The GNU programs besides the ports, as NAME|README|PURPOSE.
GNU_TOOLS='binutils|vendor/gnu/native/Readme.md|the assembler, the linker and the object file tools, run on Rach2
GCC|vendor/gnu/native/Readme.md|the C and C++ compiler, run on Rach2
GDB|vendor/gnu/native/Readme.md|the debugger, with gdbserver
SpiderMonkey|vendor/gnu/spidermonkey/Readme.md|js, the shell of Firefox'"'"'s JavaScript engine, with its JIT
chromium-headless-shell|vendor/gnu/chromium/Readme.md|Debian'"'"'s Chromium without a display, with D-Bus'"'"'s system bus'

# The functions every section shares: the pages and the projects.
# shellcheck disable=SC2016 # the dollars are awk's and the backticks Markdown's
LIB='
function loadpages(   n, a, i, line, f) {
    n = split(PAGES, a, " ")
    for (i = 1; i <= n; i++) {
        havepage[a[i]] = 1
    }
    while ((getline line < "man/aliases") > 0) {
        if (line ~ /^#/ || split(line, f, " ") != 3) {
            continue
        }
        alias[f[1], f[2]] = f[3]
    }
    close("man/aliases")
}
# The page a program or applet name reaches in section 1 or 8, as a link.
function page(name,   s, p) {
    for (s = 1; s <= 8; s += 7) {
        p = "man" s "/" name "." s
        if (p in havepage) {
            return "[" name "(" s ")](" p ")"
        }
        if ((name, s) in alias) {
            p = alias[name, s]
            return "[" p "(" s ")](man" s "/" p "." s ")"
        }
    }
    return "none"
}
function base(path,   b) {
    b = path
    sub(/.*\//, "", b)
    return b
}
function project(host) {
    if (host ~ /^build\/vendor\/busybox\//) return "BusyBox"
    if (host ~ /^build\/vendor\/dropbear\//) return "Dropbear"
    if (host ~ /^build\/vendor\/tcc\//) return "TinyCC"
    if (host ~ /^build\/vendor\/pdpmake\//) return "pdpmake"
    if (host ~ /^build\/vendor\/e2fsprogs\//) return "e2fsprogs"
    if (host ~ /^build\/ports\/doom\//) return "Doom"
    if (host ~ /^build\/vendor\/gnu\/setid\//) {
        if (base(host) in setid) {
            return "[" setid[base(host)] "](../vendor/gnu/ports/" setid[base(host)] ".sh)"
        }
        return "a GNU port"
    }
    return "in-house"
}
# The project, and the file the build makes when it has another name.
function source(name, host) {
    if (base(host) == name) {
        return project(host)
    }
    return project(host) " (`" base(host) "`)"
}
function utilities(   line, n, f, i, on) {
    while ((getline line < "utils/programs.mk") > 0) {
        if (line ~ /^UTILITIES[ \t]*=/) {
            on = 1
            sub(/^[^=]*=/, "", line)
        }
        if (!on) {
            continue
        }
        n = split(line, f, " ")
        for (i = 1; i <= n; i++) {
            if (f[i] != "\\") {
                util[++nutil] = f[i]
            }
        }
        if (line !~ /\\$/) {
            on = 0
        }
    }
    close("utils/programs.mk")
}
function stage(img, guest, host, opt,   name) {
    name = base(guest)
    if (!((img, name) in hostof)) {
        names[img, ++count[img]] = name
    }
    hostof[img, name] = host
    pathof[img, name] = guest
    optional[img, name] = opt
}
# The programs a manifest stages in /bin, /sbin, /usr/bin and /usr/sbin; an
# exec-dir of build/utils is the utilities programs.mk builds there.
function manifest(img, path,   line, f, i) {
    while ((getline line < path) > 0) {
        if (line ~ /^#/ || split(line, f, " ") < 4) {
            continue
        }
        if (f[1] == "exec-dir") {
            if (f[4] != "build/utils") {
                print "programs: " path ": an exec-dir of " f[4] > "/dev/stderr"
                exit 1
            }
            for (i = 1; i <= nutil; i++) {
                stage(img, f[3] "/" util[i], "build/utils/" util[i], 0)
            }
        } else if ((f[1] == "file" || f[1] == "optional-file") &&
                   f[3] ~ /^\/(usr\/)?s?bin\/[^\/]+$/) {
            stage(img, f[3], f[4], f[1] == "optional-file")
        }
    }
    close(path)
}
function sortnames(img, out,   n, i, j, t) {
    n = count[img]
    for (i = 1; i <= n; i++) {
        out[i] = names[img, i]
        for (j = i; j > 1 && out[j - 1] > out[j]; j--) {
            t = out[j]; out[j] = out[j - 1]; out[j - 1] = t
        }
    }
    return n
}
function words(list, n,   i, s) {
    s = list[1]
    for (i = 2; i <= n; i++) {
        s = s (i == n ? " and " : ", ") list[i]
    }
    return s
}
# The setid programs of the GNU ports, by the recipe that moves each there.
function setids(   cmd, path, line, f, n, i, port, w) {
    cmd = "ls vendor/gnu/ports/*.sh"
    while ((cmd | getline path) > 0) {
        port = base(path)
        sub(/\.sh$/, "", port)
        while ((getline line < path) > 0) {
            if (line ~ /^[ \t]*for p in /) {
                w = line
                sub(/^[ \t]*for p in /, "", w)
                sub(/;.*/, "", w)
                n = split(w, f, " ")
                for (i = 1; i <= n; i++) {
                    candidate[f[i]] = port
                }
            }
            if (line ~ /mv "\$DEST\/usr\/bin\/[a-z]/) {
                w = line
                sub(/.*mv "\$DEST\/usr\/bin\//, "", w)
                sub(/".*/, "", w)
                candidate[w] = port
            }
            if (line ~ /"\$OUT\/setid\/\$p"/) {
                for (w in candidate) {
                    if (candidate[w] == port) setid[w] = port
                }
            } else if (line ~ /"\$OUT\/setid\/[a-z]/) {
                w = line
                sub(/.*"\$OUT\/setid\//, "", w)
                sub(/".*/, "", w)
                setid[w] = port
            }
        }
        close(path)
    }
    close(cmd)
}
'

PAGES=$(cd man && echo man1/*.1 man8/*.8)

cat <<'EOF'
# The programs of Rach2

Rach2 has two systems. The musl system is the static programs this repository builds against the ported musl libc: the in-house tools, BusyBox, Dropbear, TinyCC, pdpmake, e2fsprogs and Doom. `make` builds it on every supported host, and every image stages it or a part of it. The GNU system is a dynamically linked root of glibc and the programs ported to it, installed over the musl system in an ext2 image; it builds on an x86-64 Linux host only.

`man/programs.sh` writes this file from the image manifests, the recipes and the build scripts, and `tests/run style` fails when the two differ. `make programs-md` writes it again.

EOF

awk -v PAGES="$PAGES" "$LIB"'
BEGIN {
    loadpages()
    utilities()
    manifest("real", "krffs/manifests/os-real.manifest")
    manifest("simple", "krffs/manifests/os-simple.manifest")
    manifest("rec", "krffs/manifests/rach2-recovery.manifest")
    print "## The musl system"
    print ""
    print "### Programs"
    print ""
    print "The programs of os-real, as [its manifest](../krffs/manifests/os-real.manifest) stages them, with the page of each and the project it comes from. A program built under another name has that name beside its project. The os-simple and recovery columns say yes where [os-simple](../krffs/manifests/os-simple.manifest) or [the recovery image](../krffs/manifests/rach2-recovery.manifest) stages the same program, give the project where it stages another program under the name, and are empty where it stages none."
    print ""
    print "| Program | Page | Project | os-simple | Recovery |"
    print "| --- | --- | --- | --- | --- |"
    n = sortnames("real", r)
    nopt = 0
    for (i = 1; i <= n; i++) {
        name = r[i]
        printf "| `%s` | %s | %s | %s | %s |\n", name, page(name),
            source(name, hostof["real", name]), other("simple", name), other("rec", name)
        if (optional["real", name]) opt[++nopt] = "`" name "`"
    }
    print ""
    if (nopt > 0) {
        print "os-real stages " words(opt, nopt) " only when the build has made " (nopt == 1 ? "it" : "them") "."
        print ""
    }
    print "### Programs os-real does not have"
    print ""
    print "The programs os-simple or the recovery image stages that os-real does not, with the project each comes from."
    print ""
    print "| Program | Page | os-simple | Recovery |"
    print "| --- | --- | --- | --- |"
    for (img = 1; img <= 2; img++) {
        key = img == 1 ? "simple" : "rec"
        for (i = 1; i <= count[key]; i++) {
            name = names[key, i]
            if (!(("real", name) in hostof)) extra[name] = 1
        }
    }
    m = 0
    for (name in extra) {
        e[++m] = name
        for (j = m; j > 1 && e[j - 1] > e[j]; j--) {
            t = e[j]; e[j] = e[j - 1]; e[j - 1] = t
        }
    }
    for (i = 1; i <= m; i++) {
        printf "| `%s` | %s | %s | %s |\n", e[i], page(e[i]), cell("simple", e[i]), cell("rec", e[i])
    }
    print ""
}
function other(img, name) {
    if (!((img, name) in hostof)) return ""
    if (hostof[img, name] == hostof["real", name]) return "yes"
    return source(name, hostof[img, name])
}
function cell(img, name) {
    if (!((img, name) in hostof)) return ""
    return source(name, hostof[img, name])
}'

awk -v PAGES="$PAGES" -v APPLETS="$APPLETS" "$LIB"'
BEGIN {
    loadpages()
    utilities()
    manifest("real", "krffs/manifests/os-real.manifest")
    manifest("simple", "krffs/manifests/os-simple.manifest")
    manifest("rec", "krffs/manifests/rach2-recovery.manifest")
    manifest("gnu", "krffs/manifests/os-real-gnu.manifest")
    while ((getline line < APPLETS) > 0) {
        while (match(line, /"[^"]*"/)) {
            name = substr(line, RSTART + 1, RLENGTH - 2)
            line = substr(line, RSTART + RLENGTH)
            if (name != "" && name !~ /[^a-z0-9_.\[-]/) {
                p = page(name)
                if (!(p in members)) order[++npages] = p
                members[p] = (p in members) ? members[p] ", `" name "`" : "`" name "`"
                napplets++
            }
        }
    }
    close(APPLETS)
    nimg = 0
    if (has("real")) img[++nimg] = "os-real"
    if (has("simple")) img[++nimg] = "os-simple"
    if (has("rec")) img[++nimg] = "the recovery image"
    if (has("gnu")) img[++nimg] = "the GNU root"
    print "### BusyBox applets"
    print ""
    print "The `/bin/busybox` of " words(img, nimg) " has " napplets " applets. Its ash finds an applet before it searches `PATH`, so a KRFFS image, which has no symbolic links, needs no file for each applet ([BusyBox](../vendor/busybox/Readme.md)); on the GNU root, where `/bin/sh` is bash, each applet that no port provides is a link to `/bin/busybox`. Each row is a page and the applets it covers."
    print ""
    print "| Page | Applets |"
    print "| --- | --- |"
    for (i = 1; i <= npages; i++) {
        for (j = i; j > 1 && order[j - 1] > order[j]; j--) {
            t = order[j]; order[j] = order[j - 1]; order[j - 1] = t
        }
    }
    for (i = 1; i <= npages; i++) {
        printf "| %s | %s |\n", order[i], members[order[i]]
    }
    print ""
}
function has(key,   i) {
    for (i = 1; i <= count[key]; i++) {
        if (hostof[key, names[key, i]] == "build/vendor/busybox/busybox") return 1
    }
    return 0
}'

TOOLS="$GNU_TOOLS" awk -v PAGES="$PAGES" "$LIB"'
BEGIN {
    loadpages()
    setids()
    utilities()
    manifest("real", "krffs/manifests/os-real.manifest")
    manifest("gnu", "krffs/manifests/os-real-gnu.manifest")
    while ((getline line < "vendor/gnu/sources.txt") > 0) {
        split(line, f, " ")
        if (line !~ /^#/) version[f[1]] = f[2]
    }
    close("vendor/gnu/sources.txt")
    while ((getline line < "vendor/gnu/sources-extra.txt") > 0) {
        split(line, f, " ")
        if (line !~ /^#/) version[f[1]] = f[2]
    }
    close("vendor/gnu/sources-extra.txt")
    while ((getline line < "vendor/gnu/chromium/fetch_chromium.sh") > 0) {
        if (line ~ /^SNAPSHOT=/) {
            sub(/^SNAPSHOT=/, "", line)
            snapshot = line
        }
        if (line ~ /^SUITES="/) {
            split(substr(line, 9), f, ":")
            suite = f[2]
        }
    }
    close("vendor/gnu/chromium/fetch_chromium.sh")
    version["SpiderMonkey"] = version["firefox"]
    version["chromium-headless-shell"] = "Debian " suite " at " snapshot
    version["GCC"] = version["gcc"]
    version["GDB"] = version["gdb"]

    print "## The GNU system"
    print ""
    print "The [GNU layer](../vendor/gnu/Readme.md) builds a cross toolchain with glibc, the ports below, binutils, GCC and GDB to run on Rach2, the SpiderMonkey shell, and Debian'"'"'s chromium-headless-shell, and installs them over the musl system in the os-real-gnu root, an ext2 image. The path finds a port'"'"'s program before a program of the musl system of the same name, and the ports'"'"' pages are on the machine but not in this repository; [the layer'"'"'s Readme](../vendor/gnu/Readme.md#on-the-machine) has the details."
    print ""
    print "### The root over os-real"
    print ""
    print "Besides the ports, [the root'"'"'s manifest](../krffs/manifests/os-real-gnu.manifest) stages these programs that os-real does not."
    print ""
    print "| Program | Path | Page | Project |"
    print "| --- | --- | --- | --- |"
    n = sortnames("gnu", g)
    for (i = 1; i <= n; i++) {
        name = g[i]
        if (("real", name) in hostof) continue
        h = hostof["gnu", name]
        printf "| `%s` | `%s` | %s | %s |\n", name, pathof["gnu", name],
            h ~ /^build\/vendor\/gnu\// ? "on the machine" : page(name), source(name, h)
    }
    print ""
    nmiss = 0
    nsame = 0
    m = sortnames("real", r)
    for (i = 1; i <= m; i++) {
        name = r[i]
        if (!(("gnu", name) in hostof)) {
            miss[++nmiss] = "`" pathof["real", name] "`"
        } else if (hostof["gnu", name] != hostof["real", name]) {
            same[++nsame] = "`" pathof["gnu", name] "` is `" base(hostof["gnu", name]) "`, where os-real has `" base(hostof["real", name]) "`"
        }
    }
    if (nmiss > 0) {
        print "It does not stage " words(miss, nmiss) ", which os-real does" (nsame > 0 ? "; and " words(same, nsame) : "") "."
        print ""
    }

    print "### Compilers, debugger and browser"
    print ""
    print "| Program | Version | Purpose |"
    print "| --- | --- | --- |"
    nt = split(ENVIRON["TOOLS"], t, "\n")
    for (i = 1; i <= nt; i++) {
        split(t[i], f, "|")
        printf "| [%s](../%s) | %s | %s |\n", f[1], f[2], version[f[1]], f[3]
    }
    print ""
}'

awk '
FNR == 1 {
    if (port != "") row()
    port = FILENAME
    sub(/.*\//, "", port)
    sub(/\.sh$/, "", port)
    text = ""
    ver = ""
    head = 1
    next
}
head && /^#/ {
    line = $0
    sub(/^# ?/, "", line)
    text = text (text == "" ? "" : " ") line
    next
}
{ head = 0 }
/^VERSION=/ {
    ver = $0
    sub(/^VERSION=/, "", ver)
    gsub(/"/, "", ver)
}
END {
    row()
    print ""
}
function row(   i) {
    if (!printed) {
        print "### The ports"
        print ""
        print "Each port is a recipe of [`vendor/gnu/ports/`](../vendor/gnu/ports), with the version it builds and what it is: the first sentence of its comment, up to a semicolon."
        print ""
        print "| Port | Version | Purpose |"
        print "| --- | --- | --- |"
        printed = 1
    }
    i = index(text, ". ")
    if (i > 0) text = substr(text, 1, i)
    i = index(text, "; ")
    if (i > 0) text = substr(text, 1, i - 1) "."
    printf "| [%s](../vendor/gnu/ports/%s.sh) | %s | %s |\n", port, port, ver, text
}' vendor/gnu/ports/*.sh

LICENSES="$LICENSES" awk '
BEGIN {
    version["musl"] = var("vendor/musl/build_libc.sh", "MUSL_VERSION")
    version["BusyBox"] = var("vendor/busybox/build_busybox.sh", "VERSION")
    version["Dropbear"] = var("vendor/dropbear/build_dropbear.sh", "VERSION")
    version["TinyCC"] = var("vendor/tcc/build_tcc.sh", "VERSION")
    version["pdpmake"] = var("vendor/pdpmake/build_pdpmake.sh", "VERSION")
    version["e2fsprogs"] = var("vendor/e2fsprogs/build_e2fsprogs.sh", "VERSION")
    version["GRUB"] = grab("vendor/grub/manifest.txt", "^tool grub-mkimage \\(GRUB\\) ", "")
    version["doomgeneric"] = "commit " grab("ports/doom/Readme.md", ".* at commit `", "`.*")
    version["GNU ports"] = "[each its own](#the-ports)"
    version["binutils, GCC, GDB"] = grab("vendor/gnu/sources.txt", "^binutils ", " .*") ", " \
        grab("vendor/gnu/sources.txt", "^gcc ", " .*") ", " grab("vendor/gnu/sources.txt", "^gdb ", " .*")
    version["SpiderMonkey"] = grab("vendor/gnu/sources-extra.txt", "^firefox ", " .*")
    version["chromium-headless-shell"] = "Debian " grab("vendor/gnu/chromium/fetch_chromium.sh", "^SUITES=\"[a-z-]*:", ":.*") \
        " at " grab("vendor/gnu/chromium/fetch_chromium.sh", "^SNAPSHOT=", "")
    version["termbox2"] = grab("krffs/vendor/termbox2.h", "^#define TB_VERSION_STR \"", "\".*")
    version["Multiboot header"] = "none given"
    print "## The vendored projects"
    print ""
    print "The third-party code of the repository, each with the version the build pins, its upstream license and the document that describes it. The repository declares no license for its own code."
    print ""
    print "| Project | Version | License | Readme |"
    print "| --- | --- | --- | --- |"
    n = split(ENVIRON["LICENSES"], row, "\n")
    for (i = 1; i <= n; i++) {
        split(row[i], f, "|")
        if (version[f[1]] == "" || version[f[1]] ~ /^(commit |Debian )?$/) {
            print "programs: no version for " f[1] > "/dev/stderr"
            exit 1
        }
        printf "| %s | %s | %s | [%s](../%s) |\n", f[1], version[f[1]], f[3], f[2], f[2]
    }
}
# The value of NAME= in a script, without its quotes.
function var(path, name,   line) {
    while ((getline line < path) > 0) {
        if (index(line, name "=") == 1) {
            close(path)
            sub(/^[^=]*=/, "", line)
            gsub(/"/, "", line)
            return line
        }
    }
    close(path)
    return ""
}
# The first line of a file that matches HEAD, with HEAD and TAIL removed.
function grab(path, headre, tailre,   line) {
    while ((getline line < path) > 0) {
        if (line ~ headre) {
            close(path)
            sub(headre, "", line)
            if (tailre != "") sub(tailre, "", line)
            return line
        }
    }
    close(path)
    return ""
}'
