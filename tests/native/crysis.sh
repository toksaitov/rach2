#!/bin/sh
# crysis.sh: Crysis's acceptance on the Lenovo as the native cases games/crysis*
# of gpu.cases run it, through crysis.py (tests/system/games), on a private
# copy of the game that 80-6 installed on the data partition.
#
#   crysis.sh flyby ROUTE
#   crysis.sh load SAVE
#   crysis.sh shm
#   crysis.sh check
#   crysis.sh session ran|none
#   crysis.sh links ROOT [PROTON]
#   crysis.sh seal OUT [COMMAND...]
#
# Run as root by etude-guest in the case's workspace, which holds crysis.py,
# wm.py and the empty directory `private` of the private fixture, in a mount
# namespace of the case's own, on the GNU root whose desktop supervisor runs.
# The checks:
#
#   flyby  Linux's island flyby through ROUTE, umu or direct, its first two
#          loops, each judged by crysis.py against 24 frames a second beside
#          Linux's
#   load   SAVE, a save that crysis.py's `save` recorded in an earlier boot
#          before the root was updated, such as default_accept_s2.CRYSISJMSF
#          of 80-9, loaded through a copy of the game's own prefix with umu
#          and through a new prefix of the case's own, directly: the load
#          after a restart and an update, through the existing prefix and a
#          new one
#   shm    the umu flyby on the bed's shared-memory card. The launcher
#          supplies a private mount namespace and an already mounted /data.
#   check  no game: the private game built as for the others, and the check
#          of `seal`'s namespace alone, with no session and no crysis.py;
#          with /data mounted from the partition, as on the installed
#          system, it needs a private namespace as `shm` does
#   session
#          no check: crysis-shm.sh's end, which starts the session again on
#          the files put back when it ran before the launcher, and otherwise
#          ends it, and waits as the case's own end does
#   links  no check of the game: the check of the links in ROOT alone, as
#          any user and read-only, with PROTON, /opt/proton by default, as
#          Proton's tree; prints the count of links of each kind it accepts
#          and the links it refuses, and exits 1 when it refuses one
#   seal   no check of the game: COMMAND, as bed-shm.sh runs crysis.py's
#          flyby, in the namespace that `seal` below makes, with OUT, the
#          case's output, writable there
#
# The data partition, /dev/disk0p6, and all it holds are the owner's; the
# case never writes it. It mounts the partition read-only on /data/owner
# within a /data of its own, the directory `private` of the workspace bound
# over /data in the namespace, and builds /data/crysis there for the game's
# own paths, which crysis.sh of the root fixes under /data/crysis:
#
#   game     a tree of the case's directories whose files are symbolic links
#            to the owner's, read-only, which the game reads through; a file
#            the game or crysis.py writes, such as Game.log, system.cfg or
#            the benchmark's log, replaces its link with a file of the case
#   compat, saves, steam, cache, accept.json
#            copies owned by the desktop account, which runs the game: its
#            prefix, its profile and saves, Proton's Steam client directory,
#            Mesa's shader cache and the saves' record
#   logs     a new directory for Proton's logs
#
# and /data/home, the desktop account's HOME and XDG directories while the
# game runs, in which umu unpacks the root's Steam Linux Runtime afresh.
#
# What keeps the owner's files safe is the namespace in which crysis.py's
# commands after `prepare`, and so the game, Wine and umu, run (`seal`):
# every mount there, a bind of the namespace's own under a root moved onto
# a recursive bind of /, is read-only but /data, /tmp, /var/tmp, /dev/shm,
# the session's runtime directory and the case's output, so that no link in the
# copy, present or made later by Proton or Wine, can carry a write to a file
# of the owner's, in /home/desktop or on the partition. Before the game,
# `seal`'s checks run alone in such a namespace: statvfs must give the
# partition, /home/desktop and Proton's tree as read-only, and each kind of
# write must fail there with EROFS on sentinels the script makes just before
# and removes once the check has ended, in /home/desktop and, when the
# partition is mounted read-write on /data, on the partition, outside
# /data/crysis, directly and through links in the copy, and succeed in the
# writable paths. No write of the check aims at a file it did not make. Each
# namespace a game's command runs in checks statvfs and the writable paths
# again. Before that, links_check refuses a link in the copy that names a
# file outside it and the read-only trees, as an early and readable error;
# `crysis.sh links` runs it alone, read-only, on a tree such as the owner's
# /data/crysis. The script's own writes as root before the namespace, the
# copies, `chown -R -h`, which follows no link, and the sentinels, touch only
# the private /data and the sentinels.
#
# The load's new prefix is /data/native/crysis-compat, in the same /data. On
# every exit the script ends the game's mounts, puts back the session's
# weston.ini and rach2-session, which crysis.py's `prepare` writes, and leaves
# the session as it found it: started again by SIGHUP, with the files put back,
# when it ran, and otherwise ended by SIGUSR2, and waits for the new Weston or
# for none. `private`, all the case makes on the root besides `out`, is the
# fixture's: etude-guest removes it once every process of the case has ended,
# after a pass, a failure or a kill that skips this trap alike.
#
# Prints crysis.py's lines and `crysis-case: result: pass` or `fail: why`, and
# exits 0 on a pass.
set -eu

here=$(pwd)
data=/dev/disk0p6
private=$here/private
owner=/data/owner
base=/data/crysis
fresh=/data/native/crysis-compat
ini=/etc/xdg/weston/weston.ini
session=/usr/libexec/rach2-session
pidfile=/run/rach2-desktop.pid
uid=1001
bound=0 mounted=0 kept=0 failed='' found=0 proton=''
source_bound=0
home=/data/home
# The check's sentinels (sentinels_make) and the partition's writable bind
# on which the one on the partition is made and removed.
rw=$here/partition rw_made=0 rw_bound=0 probe_home='' probe_part='' part_rel=''

say() { echo "crysis-case: $*"; }

# mounted_at DIR MODE: whether /proc/mounts has a mount on DIR whose options
# begin with MODE, rw or ro, and, for rw, whose type is ext2, as crysis.sh
# of the root requires of /data.
mounted_at()
{
    awk -v dir="$1" -v mode="$2" '$2 == dir && $4 ~ "^" mode "(,|$)" &&
        (mode == "ro" || $3 == "ext2") { ok = 1 } END { exit !ok }' /proc/mounts
}

# session_restore: have the supervisor start the session again, on the files
# put back, when it ran, and otherwise end it, and wait, 600 tries of 0.1 s
# at most, for a Weston other than the one that ran, or for none, so that
# the next case does not draw in a session about to end. The end is sent
# even with no Weston, since a start the supervisor has taken may not have
# reached Weston yet. rach2-desktop reads a signal's flags at the start of
# a turn of its loop and ends each turn with `sleep 1`; a turn may take ten
# seconds to end a session and then begin a start it had already chosen.
# The end is done once a turn begun after the signal has run whole, which
# the second `sleep 1` seen since the signal shows, and neither Weston nor
# the session's script is left.
session_restore()
{
    old=$(pgrep -x weston | tr '\n' ' ')
    [ -f "$pidfile" ] || return 1
    supervisor=$(cat "$pidfile")
    if [ "$found" = 1 ]; then
        kill -HUP "$supervisor" 2> /dev/null || return 1
    else
        kill -USR2 "$supervisor" 2> /dev/null || return 1
    fi
    tries=0 turn='' turns=0
    while :; do
        now=$(pgrep -x weston | tr '\n' ' ')
        if [ "$found" = 1 ]; then
            [ -z "$now" ] || [ "$now" = "$old" ] || [ ! -S "/run/user/$uid/wayland-0" ] || return 0
        else
            pause=$(pgrep -P "$supervisor" -fx 'sleep 1' | head -n 1)
            if [ -n "$pause" ] && [ "$pause" != "$turn" ]; then
                turn=$pause turns=$((turns + 1))
            fi
            # A supervisor that has exited ended the session in its trap.
            { [ "$turns" -ge 2 ] || ! kill -0 "$supervisor" 2> /dev/null; } &&
                [ -z "$now" ] && ! pgrep -f "$session" > /dev/null && return 0
        fi
        tries=$((tries + 1))
        [ "$tries" -lt 600 ] || return 1
        sleep 0.1
    done
}

# sentinel_fill DIR: put in DIR, a directory of the check's own, the file
# `file` and the link `link` to it.
sentinel_fill()
{
    echo crysis-check > "$1/file" && ln -s file "$1/link"
}

# sentinels_make: make the seal check's sentinels, in the desktop account's
# home and, when the partition is mounted read-write on /data, as on the
# installed system, in its directory `native`, or its root, through the
# partition's writable bind $rw, outside /data/crysis, whose metadata
# crysis-shm.sh compares. Each variable is set as soon as its `mkdir`
# succeeds, before the directory is filled, so that the removal takes a
# directory whose filling failed and touches nothing the check did not
# make; a `mkdir` that fails, as on a name already there, sets nothing. A
# partition mounted read-only, as when the case mounts it, has no sentinel:
# its mount refuses every write, and the check takes statvfs's word for it.
sentinels_make()
{
    desk=$(awk -F: -v uid="$uid" '$3 == uid { print $6; exit }' /etc/passwd)
    [ -n "$desk" ] && [ -d "$desk" ] || return 1
    mkdir -m 0700 "$desk/.crysis-check-$$" || return 1
    probe_home=$desk/.crysis-check-$$
    sentinel_fill "$probe_home" || return 1
    [ "$rw_bound" = 1 ] || return 0
    part_rel=.crysis-check-$$
    [ ! -d "$rw/native" ] || [ -L "$rw/native" ] || part_rel=native/$part_rel
    mkdir -m 0700 "$rw/$part_rel" || return 1
    probe_part=$rw/$part_rel
    sentinel_fill "$probe_part" || return 1
}

# sentinels_remove: remove the sentinels made, once no process of the check
# runs, check that they are gone, and unmount the partition's writable bind;
# returns 1 if anything is left.
sentinels_remove()
{
    for probe in $probe_home $probe_part; do
        rm -rf "$probe"
        if [ -e "$probe" ] || [ -L "$probe" ]; then
            say "check: sentinel $probe is left" >&2
            return 1
        fi
    done
    probe_home='' probe_part=''
    if [ "$rw_bound" = 1 ]; then
        umount "$rw" || return 1
        rw_bound=0
    fi
    if [ "$rw_made" = 1 ]; then
        rmdir "$rw" || return 1
        rw_made=0
    fi
}

# seal_check: run `seal`'s checks alone, in a namespace of their own made as
# the game's is, with the sentinels made just before and removed once the
# check's process has ended, whether it passed or not.
seal_check()
{
    sentinels_make || { sentinels_remove || :; fail "the check's sentinels"; }
    sealed_ok=0
    CRYSIS_SENTINELS="home=$probe_home${probe_part:+ owner=$owner/$part_rel}" \
        unshare --mount --propagation private python3 -c "$seal_py" "$here/out" ||
        sealed_ok=1
    sentinels_remove || fail "the check's sentinels are left"
    [ "$sealed_ok" = 0 ] || fail check
    say "check: sentinels removed"
}

# shellcheck disable=SC2329 # the exit trap runs it
cleanup()
{
    status=$?
    trap - EXIT
    sentinels_remove || failed="$failed sentinels"
    if [ "$kept" = 1 ]; then
        cp -p "$here/weston.ini" "$ini" && cp -p "$here/rach2-session" "$session" ||
            failed="$failed session-files"
        session_restore || say "session: not left as it was found" >&2
    fi
    # A process of the game's that has not yet ended may hold either mount;
    # the namespace's mounts go with the case's last process.
    [ "$mounted" = 0 ] || umount "$owner" 2> /dev/null || umount -l "$owner" ||
        failed="$failed umount-owner"
    [ "$bound" = 0 ] || umount /data 2> /dev/null || umount -l /data || failed="$failed umount"
    [ "$source_bound" = 0 ] || umount "$private/owner" 2> /dev/null ||
        umount -l "$private/owner" || failed="$failed umount-source"
    if [ -n "$failed" ]; then
        say "result: fail:$failed"
        exit 1
    fi
    if [ "$status" != 0 ]; then
        say "result: fail: status $status"
        exit "$status"
    fi
    say "result: pass"
}
# links_check ROOT PROTON OWNER: refuse a link under ROOT, the private copy,
# whose resolved path lies outside ROOT and outside PROTON and OWNER, the
# read-only trees of Proton and of the owner's partition, each empty or named
# by its resolved path, unless it is one of Wine's drive links,
# compat/pfx/dosdevices/[a-z]:, which name the running system, as z: -> /
# does. It gives an early and readable error for a copy that names files
# outside it; what keeps the owner's files safe is `seal`'s namespace. Prints
# the count of each kind, (a) in a read-only tree, (b) drive links and (c)
# inside ROOT, and the first links refused, and returns 1 when one is.
links_check()
{
    python3 - "$@" << 'PYTHON'
import os, re, sys

root, trees = sys.argv[1], [tree for tree in sys.argv[2:4] if tree]
drive = re.compile(r"compat/pfx/dosdevices/[a-z]:\Z")


def inside(path, top):
    return path == top or path.startswith(top + "/")


def failed(error):
    raise error


if not os.path.isdir(root) or os.path.realpath(root) != root:
    sys.exit("crysis-case: links: %s is not a directory named by its resolved path" % root)
counts = {"a": 0, "b": 0, "c": 0}
refused = []
for top, dirs, files in os.walk(root, onerror=failed):
    for name in dirs + files:
        path = os.path.join(top, name)
        if not os.path.islink(path):
            continue
        rel = path[len(root) + 1:]
        end = os.path.realpath(path)
        if drive.match(rel):
            counts["b"] += 1
        elif any(inside(end, tree) for tree in trees):
            counts["a"] += 1
        elif inside(end, root):
            counts["c"] += 1
        else:
            refused.append("%s -> %s" % (rel, os.readlink(path)))
print("crysis-case: links: %d accepted, (a) %d, (b) %d, (c) %d; %d refused"
      % (sum(counts.values()), counts["a"], counts["b"], counts["c"], len(refused)))
for line in refused[:20]:
    print("crysis-case: links: refused: " + line)
sys.exit(1 if refused else 0)
PYTHON
}

# seal OUT [COMMAND...]: run COMMAND, as root, in a mount namespace of its
# own in which every mount is read-only but the paths WRITABLE names, OUT
# among them, once the checks below have passed; with no COMMAND, run only
# the checks. The mounts the namespace starts with are the parent's own
# records, whose flags a remount may not change while the parent sees them
# (EBUSY, mount(2)), so the namespace first moves its root onto a recursive
# bind of / made in a directory of the private /data, whose mounts are
# binds of its own, with pivot_root, and lets the old root go; it then
# checks that each mount point shows what it showed before, by the device,
# the root within it, the type and the source of its top mount. Each of
# those mounts of a type that holds files is remounted read-only, with its
# nosuid, nodev and noexec kept; proc, sysfs and the device file systems
# are left as they are, since their writes are the kernel's controls, such
# as a user namespace's uid_map, and a read-only mount opens a device node
# for writing all the same, as Linux's does. Each writable path then has
# its own mount made writable again, or a recursive bind of itself over
# itself made writable, which leaves a mount below it, /data/owner under
# /data, read-only.
#
# The checks, as root, so that only the mount refuses. First, before any
# write, statvfs must give /data/owner, the desktop account's home and
# Proton's tree as read-only. Then, when CRYSIS_SENTINELS names the check's
# sentinels, as `home=DIR owner=DIR`, a write, a write through the link,
# a truncate and a chown of the sentinel file and a create, mkdir, symlink,
# rename and unlink in each sentinel directory, and the same through a link
# in /data/crysis to each directory, must each fail with EROFS; no write aims
# at a file the check did not make. A sentinel is a directory .crysis-check-*
# that the caller made, holding the file `file` and the link `link` to it,
# and removes once this process has ended, since no write can remove it here.
# Last, in /data/crysis and each writable path a mkdir of a directory of the
# check's own, by mkdtemp, and in it the same writes of new names must
# succeed; no name already there is touched, a write whose file was not made
# is not tried, and the directory is taken back. Prints a line for each
# place and exits 1
# on a failure, before COMMAND runs. The process is the caller's: unshare and
# python3 exec in turn, so that a signal to it reaches COMMAND.
seal_py=$(cat << 'PYTHON'
import errno, os, pwd, re, subprocess, sys, tempfile

UID = 1001
BASE = "/data/crysis"
OWNER = "/data/owner"
KERNEL = {"proc", "sysfs", "devfs", "devtmpfs", "devpts", "debugfs", "tracefs",
          "securityfs", "mqueue", "pstore", "cgroup", "cgroup2", "bpf",
          "binfmt_misc", "configfs", "fusectl"}
KEPT = {"nosuid", "nodev", "noexec"}
WRITABLE = (
    # The private copy in /data/crysis, the game's HOME in /data/home, the
    # HUD's dump and the load's new prefix; /data/owner stays read-only.
    "/data",
    # Wine's server directory /tmp/.wine-UID and the runtime's temporaries.
    "/tmp",
    # The other temporary directory, which GLib and the runtime may use.
    "/var/tmp",
    # Wine's fsync and the runtime's shared memory.
    "/dev/shm",
    # XDG_RUNTIME_DIR: the sockets of Wayland and PulseAudio, and
    # pressure-vessel's own files.
    "/run/user/%d" % UID,
)


def say(text):
    print("crysis-case: " + text, flush=True)


def die(text):
    say("seal: failed: " + text)
    sys.exit(1)


def under(path, top):
    return path == top or path.startswith(top.rstrip("/") + "/")


def mountinfo():
    """Return each mount point, the last listed, which is on top, with the
    fields of its line: the device, the root within it, the options, the
    type and the source."""
    table = {}
    with open("/proc/self/mountinfo") as f:
        for line in f:
            fields = line.split()
            dash = fields.index("-")
            point = re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), fields[4])
            table[point] = (fields[2], fields[3], fields[5], fields[dash + 1], fields[dash + 2])
    return table


def mounts():
    """Return each mount point, the one on top, with its type and the
    options of KEPT it has."""
    return {point: (kind, [o for o in options.split(",") if o in KEPT])
            for point, (_, _, options, kind, _) in mountinfo().items()}


def shown(table):
    """Return what each mount point of TABLE, from `mountinfo`, shows: its
    device, its root within it, its type and its source."""
    return {point: (dev, root, kind, source)
            for point, (dev, root, _, kind, source) in table.items()}


def rebase():
    """Move the namespace's root onto a recursive bind of /, every mount of
    which is a bind this namespace made and alone sees, and let the old
    root go: the bind in a directory of the private /data that mkdtemp
    makes, pivot_root to it with the old root put on the same directory as
    the bind shows it, a lazy unmount of that, and the directory taken
    back; the working directory is then the same path in the new root.
    Each mount point must then show what it showed before."""
    here = os.getcwd()
    before = shown(mountinfo())
    stage = tempfile.mkdtemp(prefix=".crysis-root-", dir="/data")
    mount("--rbind", "/", stage)
    for args in (("pivot_root", stage, stage + stage), ("umount", "-l", stage)):
        run = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, check=False)
        if run.returncode:
            die("%s: %s" % (" ".join(args), run.stdout.strip()))
        os.chdir("/")
    attempt(lambda: os.rmdir(stage))
    os.chdir(here)
    after = shown(mountinfo())
    if after != before:
        lost = sorted(set(before) - set(after))
        new = sorted(set(after) - set(before))
        moved = sorted(p for p in set(before) & set(after) if before[p] != after[p])
        die("the bind of / does not show the mounts: lost %s, new %s, changed %s"
            % (lost, new, moved))
    say("seal: root moved onto a bind of /; %d mount points as before" % len(after))


def mount(*args):
    run = subprocess.run(("mount",) + args, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, check=False)
    if run.returncode:
        die("mount %s: %s" % (" ".join(args), run.stdout.strip()))


def writable(path):
    """Make PATH, named by its resolved path, writable again."""
    table = mounts()
    point = max((p for p in table if under(path, p)), key=len)
    if table[point][0] in KERNEL:
        return
    if point != path:
        mount("--rbind", path, path)
    mount("-o", ",".join(["remount", "bind", "rw"] + mounts()[path][1]), path)


def attempt(call):
    try:
        call()
    except OSError as error:
        return error.errno
    return 0


def remove(names):
    for path in names:
        if os.path.isdir(path) and not os.path.islink(path):
            attempt(lambda: os.rmdir(path))
        elif os.path.lexists(path):
            attempt(lambda: os.unlink(path))


def sentinels(home):
    """Return the sentinel directories CRYSIS_SENTINELS names, by label, each
    checked to be the check's own: a directory .crysis-check-* in the home
    or on the partition holding the regular file `file` and the link `link`
    to it."""
    tops = {"home": home, "owner": OWNER}
    found = {}
    for word in os.environ.get("CRYSIS_SENTINELS", "").split():
        label, _, folder = word.partition("=")
        if label not in tops or label in found or \
                not os.path.basename(folder).startswith(".crysis-check-") or \
                os.path.realpath(folder) != folder or not under(folder, tops[label]) or \
                folder == tops[label]:
            die("%s is no sentinel of the check's" % word)
        sentinel, link = os.path.join(folder, "file"), os.path.join(folder, "link")
        if not os.path.isfile(sentinel) or os.path.islink(sentinel) or \
                not os.path.islink(link) or os.readlink(link) != "file":
            die("%s holds no sentinel file and link" % folder)
        found[label] = folder
    return found


def refused(label, folder):
    """Return the writes in FOLDER, a sentinel directory or a link to one,
    that did not fail with EROFS. Each aims at the sentinel file, its link
    or a new name in the directory; a new name made is taken back, and the
    caller removes the rest."""
    sentinel = os.path.join(folder, "file")
    link = os.path.join(folder, "link")
    new = os.path.join(folder, "new")
    rows = [("write", lambda: os.close(os.open(sentinel, os.O_WRONLY))),
            ("write-link", lambda: os.close(os.open(link, os.O_WRONLY))),
            ("truncate", lambda: os.truncate(sentinel, 0)),
            ("chown", lambda: os.chown(sentinel, UID, UID, follow_symlinks=False)),
            ("create", lambda: os.close(os.open(new, os.O_CREAT | os.O_EXCL | os.O_WRONLY,
                                                0o600))),
            ("mkdir", lambda: os.mkdir(new)),
            ("symlink", lambda: os.symlink("file", new)),
            ("rename", lambda: os.rename(sentinel, new)),
            ("unlink", lambda: os.unlink(sentinel))]
    bad = []
    for op, call in rows:
        code = attempt(call)
        remove([new])
        if code != errno.EROFS:
            bad.append("%s %s: %s" % (label, op, errno.errorcode.get(code, "allowed")))
    say("check: read-only: %s %s: %d of %d writes failed with EROFS"
        % (label, folder, len(rows) - len(bad), len(rows)))
    return bad


def allowed(folder):
    """Return the writes of new names in FOLDER that failed. They run in a
    directory of their own that mkdtemp makes there, with a name no one
    else has, so that nothing in FOLDER but that directory is touched; a
    write of a file whose making failed is not tried, and the directory is
    removed with what it holds."""
    rows = 8
    try:
        own = tempfile.mkdtemp(prefix=".crysis-check-", dir=folder)
    except OSError as error:
        say("check: writable: %s: 0 of %d writes went through" % (folder, rows))
        return ["%s mkdir: %s" % (folder, errno.errorcode.get(error.errno, error.errno))] + \
            ["%s: no write tried in a directory not made" % folder]
    name, other, link = (os.path.join(own, leaf) for leaf in ("new", "renamed", "link"))
    steps = [("create", None, lambda: os.close(os.open(
                 name, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600))),
             ("rename", "create", lambda: os.rename(name, other)),
             ("write", "rename", lambda: os.close(os.open(other, os.O_WRONLY | os.O_NOFOLLOW))),
             ("truncate", "rename", lambda: os.truncate(other, 0)),
             ("chown", "rename", lambda: os.chown(other, UID, UID, follow_symlinks=False)),
             ("unlink", "rename", lambda: os.unlink(other)),
             ("symlink", None, lambda: os.symlink("crysis-check", link))]
    done = set()
    bad = []
    for op, needs, call in steps:
        if needs and needs not in done:
            bad.append("%s %s: not tried, since %s failed" % (folder, op, needs))
            continue
        code = attempt(call)
        if code:
            bad.append("%s %s: %s" % (folder, op, errno.errorcode.get(code, code)))
        else:
            done.add(op)
    remove((link, other, name, own))
    if os.path.lexists(own):
        bad.append("%s: %s is left" % (folder, own))
    say("check: writable: %s: %d of %d writes went through" % (folder, 1 + len(done), rows))
    return bad


out, command = os.path.realpath(sys.argv[1]), sys.argv[2:]
if os.geteuid() != 0:
    die("run as root")
try:
    home = pwd.getpwuid(UID).pw_dir
except KeyError:
    die("no account of uid %d" % UID)
if not os.path.isdir(out) or under(out, home) or under(out, OWNER):
    die("%s is no directory of the case's own" % out)
table = mounts()
if "/data" not in table or table.get(OWNER, ("", []))[0] == "" or \
        os.stat("/data").st_dev == os.stat(OWNER).st_dev or not os.path.isdir(BASE):
    die("no private /data with the partition on %s and the game in %s" % (OWNER, BASE))
probes = sentinels(home)
if probes and command:
    die("no command runs with the sentinels, which only the caller removes")

rebase()
count = 0
for point, (kind, kept) in sorted(mounts().items()):
    if kind not in KERNEL:
        mount("-o", ",".join(["remount", "bind", "ro"] + kept), point)
        count += 1
made = []
for path in WRITABLE + (out,):
    real = os.path.realpath(path)
    if os.path.isdir(real):
        writable(real)
        made.append(real)
    elif path in ("/data", out):
        die("no %s" % path)
say("seal: %d mounts read-only; writable: %s" % (count, " ".join(made)))

# The owner's places must read as read-only before any write is tried.
for place in [OWNER, home] + (["/opt/proton"] if os.path.isdir("/opt/proton") else []):
    try:
        flags = os.statvfs(place).f_flag
    except OSError as error:
        die("statvfs %s: %s" % (place, errno.errorcode.get(error.errno, error.errno)))
    if not flags & os.ST_RDONLY:
        die("%s is not read-only by statvfs; no write tried" % place)
    say("check: read-only by statvfs: %s" % place)

links = []
bad = []
try:
    places = []
    for label in ("home", "owner") if probes else ():
        if label not in probes:
            say("check: read-only: %s: no sentinel; statvfs alone" % label)
            continue
        link = os.path.join(BASE, ".crysis-check-link-%s-%d" % (label, os.getpid()))
        os.symlink(probes[label], link)
        links.append(link)
        places += [(label, probes[label]), ("link-" + label, link)]
    for label, folder in places:
        bad += refused(label, folder)
    for folder in [BASE] + made:
        bad += allowed(folder)
finally:
    remove(links)
for line in bad:
    say("check: failed: " + line)
if bad:
    die("%d writes not as required" % len(bad))
say("check: ok")
if command:
    os.execvp(command[0], command)
PYTHON
)

# `crysis.sh links ROOT [PROTON]`: the check alone, read-only, on a tree
# such as the owner's /data/crysis, with PROTON, /opt/proton by default,
# taken as read-only.
if [ "${1:-}" = links ] && { [ $# = 2 ] || [ $# = 3 ]; }; then
    links_root=$(realpath "$2") || exit 1
    links_proton=''
    [ ! -d "${3:-/opt/proton}" ] || links_proton=$(realpath "${3:-/opt/proton}") || exit 1
    links_check "$links_root" "$links_proton" ''
    exit
fi

# `crysis.sh seal OUT [COMMAND...]`, for bed-shm.sh: COMMAND in the read-only
# namespace, in this process.
if [ "${1:-}" = seal ] && [ $# -ge 2 ]; then
    shift
    exec unshare --mount --propagation private python3 -c "$seal_py" "$@"
fi

# `crysis.sh session ran|none`, for crysis-shm.sh: leave the session
# running on the files put back, or ended, and wait for it.
if [ "${1:-}:$#" = session:2 ]; then
    case $2 in
        ran) found=1 ;;
        none) found=0 ;;
        *) echo "usage: crysis.sh session ran|none" >&2; exit 2 ;;
    esac
    session_restore || { say "session: not left as it was found" >&2; exit 1; }
    exit 0
fi

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail()
{
    failed="$failed $*"
    exit 1
}

case ${1:-}:$# in
    flyby:2 | load:2 | shm:1 | check:1) ;;
    *)
        echo "usage: crysis.sh flyby umu|direct | crysis.sh load SAVE | crysis.sh shm |" \
            "crysis.sh check | crysis.sh session ran|none | crysis.sh links ROOT [PROTON] |" \
            "crysis.sh seal OUT [COMMAND...]" >&2
        exit 2
        ;;
esac
[ "$1" = check ] || { [ -f crysis.py ] && [ -f wm.py ]; } ||
    fail "no crysis.py and wm.py in the workspace"
[ "$1" = check ] || command -v crysis.sh > /dev/null || fail "no crysis.sh on this root"
[ -d /data ] && [ -b "$data" ] || fail "no /data or no $data"
[ -d "$private" ] && [ -z "$(ls -A "$private")" ] ||
    fail "no empty $private; the case runs with the private fixture"

# ondata: the partition is mounted on /data already, as on the installed
# system, which `shm` needs and `check` takes when it finds.
ondata=0
if [ "$1" = shm ] || { [ "$1" = check ] && awk -v dev="$data" \
    '$1 == dev && $2 == "/data" { ok = 1 } END { exit !ok }' /proc/mounts; }; then
    ondata=1
fi
if [ "$ondata" = 1 ]; then
    current_mountns=$(readlink /proc/self/ns/mnt) || fail "cannot read the mount namespace"
    [ -n "${CRYSIS_PARENT_MNTNS:-}" ] &&
        [ "$current_mountns" != "$CRYSIS_PARENT_MNTNS" ] ||
        fail "no private mount namespace"
    awk -v dev="$data" '$1 == dev && $2 == "/data" && $3 == "ext2" && $4 ~ /(^|,)rw(,|$)/ {
        ok = 1
    } END { exit !ok }' /proc/mounts || fail "$data is not mounted on /data"
    [ "$(stat -c %d "$private")" != "$(stat -c %d /data)" ] ||
        fail "the workspace is on the owner's partition"
else
    awk '$2 == "/data" { found = 1 } END { exit found }' /proc/mounts ||
        fail "/data is mounted already"
    ! pgrep -x weston > /dev/null || found=1
fi

mkdir "$private/owner"
if [ "$ondata" = 1 ]; then
    # A writable view of the partition, outside /data, for the check's
    # sentinel alone, which sentinels_remove unmounts after the check.
    mkdir "$rw" || fail "no new $rw"
    rw_made=1
    mount --bind /data "$rw" || fail "$data would not bind on $rw"
    rw_bound=1
    # The mounted device already has a claim. Make a read-only view of it.
    mount --bind /data "$private/owner" || fail "$data would not bind"
    source_bound=1
    mount -o remount,bind,ro "$private/owner" || fail "$data bind would not turn read-only"
    mount --rbind "$private" /data || fail "the workspace would not bind on /data"
    mounted=1
else
    mount --bind "$private" /data || fail "the workspace would not bind on /data"
fi
bound=1
[ "$private" -ef /data ] || fail "/data does not name the workspace's bind"
# The workspace is root's and closed; the desktop account, which runs the
# game, must pass through /data to reach /data/crysis.
chmod 0755 /data || fail "/data would not open to the desktop account"
mounted_at /data rw || fail "the workspace is not a writable ext2 file system"
if [ "$ondata" = 0 ]; then
    mount -t ext2 -o ro "$data" "$owner" || fail "$data would not mount read-only on $owner"
    mounted=1
fi
mounted_at "$owner" ro || fail "$owner is not read-only"
[ -f "$owner/crysis/game/Bin64/Crysis.exe" ] || fail "no game in $owner/crysis/game"
say "$data read-only on $owner"

# The desktop account reads the game through the links.
closed=$( {
    find "$owner" "$owner/crysis" -maxdepth 0 ! -perm -0001
    find "$owner/crysis/game" \( -type f ! -perm -0004 \) -o \( -type d ! -perm -0005 \)
} | head -n 3)
[ -z "$closed" ] || fail "the desktop account cannot read $(echo "$closed" | tr '\n' ' ')"

copies=''
for name in compat saves steam cache accept.json; do
    [ ! -e "$owner/crysis/$name" ] || copies="$copies $owner/crysis/$name"
done
# The copies, a new prefix as large as the game's own for `load`, and a GiB
# for the logs, the game's files and the links.
# shellcheck disable=SC2086 # paths without spaces
need=$( { [ -z "$copies" ] || du -sk $copies; } | awk '{ n += $1 } END { print n + 0 }')
[ "$1" != load ] || [ ! -d "$owner/crysis/compat" ] ||
    need=$((need + $(du -sk "$owner/crysis/compat" | cut -f 1)))
need=$((need + 1048576))
# umu unpacks the root's Steam Linux Runtime into the game's HOME.
if [ "$1:${2:-}" = flyby:umu ] || [ "$1" = load ] || [ "$1" = shm ]; then
    runtime=$(xz --robot --list /opt/umu/SteamLinuxRuntime_4.tar.xz 2> /dev/null |
        awk '$1 == "totals" { print int($5 / 1024) }')
    need=$((need + ${runtime:-4194304}))
fi
free=$(df -Pk "$private" | awk 'NR == 2 { print $4 }')
[ "$free" -ge "$need" ] || fail "the copies need $need KiB, the workspace has $free"

proton=''
[ ! -d /opt/proton ] || proton=$(realpath /opt/proton) || fail "cannot resolve /opt/proton"

mkdir "$base" "$base/logs"
cp -rs "$owner/crysis/game" "$base/game" || fail "game-links"
# shellcheck disable=SC2086 # paths without spaces
[ -z "$copies" ] || cp -a $copies "$base/" || fail copies
# A link that names a file outside the copy and the read-only trees fails
# the case here, before the game or crysis.py touches the tree.
links_check "$base" "$proton" "$owner" || fail "links in the private game lead out of it"
# -R follows no link, and -h changes the links themselves.
chown -R -h "$uid:$uid" "$base"
install -d -o "$uid" -g "$uid" -m 0755 "$home"
say "private game: $(find "$base/game" -type l | wc -l) links, copies of$(
    echo "$copies" | sed "s|$owner/crysis/||g"), $need KiB reserved"

# The umu container can write to the private /data. The test's output
# directory on the host root is outside the container.
CRYSIS_HUD_PARENT=$base/logs/hud
# crysis.py runs the game with HOME and the XDG directories in the private
# /data, since /home/desktop is read-only in `seal`'s namespace.
CRYSIS_HOME=$home
export CRYSIS_HUD_PARENT CRYSIS_HOME

# The read-only namespace's checks, with the sentinels, once before the game;
# each namespace the game's commands run in checks statvfs and the writable
# paths again.
mkdir -p "$here/out"
seal_check
[ "$1" != check ] || exit 0

if [ "$1" = shm ]; then
    # Let the VM runner own the handoff and its signal traps. The mounts
    # go away when the last process of this namespace ends.
    exec sh ./bed-shm.sh -d "$owner/native/bed/vm.disk" -w -l crysis \
        -g 0000:00:02.0 -m 8192 -o "$here/out"
fi

cp -p "$ini" weston.ini
cp -p "$session" rach2-session
kept=1

# prepare writes the session's files, which the namespace keeps read-only;
# every write of the game's commands is made in it.
python3 ./crysis.py prepare "$here/out" || fail prepare
sealed()
{
    unshare --mount --propagation private python3 -c "$seal_py" "$here/out" "$@"
}
case $1 in
    flyby)
        sealed python3 ./crysis.py flyby "$2" "flyby-$2" 2 || fail "flyby-$2"
        ;;
    load)
        sealed python3 ./crysis.py load load-update "$2" umu || fail load-update
        mkdir -p "${fresh%/*}"
        install -d -o "$uid" -g "$uid" -m 0755 "$fresh"
        sealed python3 ./crysis.py load load-new "$2" direct "$fresh" || fail load-new
        ;;
esac
