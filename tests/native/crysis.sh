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
# every mount there is read-only but /data, /tmp, /var/tmp, /dev/shm, the
# session's runtime directory and the case's output, so that no link in the
# copy, present or made later by Proton or Wine, can carry a write to a file
# of the owner's, in /home/desktop or on the partition. `seal` checks that
# each kind of write fails there with EROFS through a link out of the copy,
# in /home/desktop and on the partition, and succeeds in the writable paths,
# before it starts the command. Before that, links_check refuses a link in
# the copy that names a file outside it and the read-only trees, as an early
# and readable error; `crysis.sh links` runs it alone, read-only, on a tree
# such as the owner's /data/crysis. The script's own writes as root before
# the namespace, the copies and `chown -R -h`, which follows no link, touch
# only the private /data.
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

# shellcheck disable=SC2329 # the exit trap runs it
cleanup()
{
    status=$?
    trap - EXIT
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
# among them, once the check below has passed; with no COMMAND, run only the
# check. Each mount of a type that holds files is remounted read-only, with
# its nosuid, nodev and noexec kept; proc, sysfs and the device file systems
# are left as they are, since their writes are the kernel's controls, such as
# a user namespace's uid_map, and a read-only mount opens a device node for
# writing all the same, as Linux's does. Each writable path then has its own
# mount made writable again, or a recursive bind of itself over itself made
# writable, which leaves a mount below it, /data/owner under /data, read-only.
#
# The check, as root, so that only the mount refuses: in /data/owner/crysis,
# in the desktop account's home and through links in the copy to that home
# and to Proton's tree, a create, rename, unlink, mkdir and symlink of a new
# name and a write, truncate and chown of an existing file, each to its own
# size and owner, must each fail with EROFS; in /data/crysis and each
# writable path the same writes of a new file must succeed. A write that went
# through is taken back at once. Prints a line for each place and exits 1 on
# a failure, before COMMAND runs. The process is the caller's: unshare and
# python3 exec in turn, so that a signal to it reaches COMMAND.
seal_py=$(cat << 'PYTHON'
import errno, os, pwd, re, subprocess, sys

UID = 1001
BASE = "/data/crysis"
OWNER = "/data/owner/crysis"
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


def mounts():
    """Return each mount point, the last listed, which is on top, with its
    type and the options of KEPT it has."""
    table = {}
    with open("/proc/self/mountinfo") as f:
        for line in f:
            fields = line.split()
            dash = fields.index("-")
            point = re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), fields[4])
            table[point] = (fields[dash + 1], [o for o in fields[5].split(",") if o in KEPT])
    return table


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


def tries(folder, existing):
    """Return the writes to try in FOLDER and on EXISTING, if given, and the
    names they may leave."""
    name = os.path.join(folder, ".crysis-check-%d" % os.getpid())
    other = name + "-renamed"
    rows = [("create", lambda: os.close(os.open(name, os.O_CREAT | os.O_EXCL | os.O_WRONLY,
                                                0o600))),
            ("rename", lambda: os.rename(name, other)),
            ("unlink", lambda: os.unlink(other if os.path.lexists(other) else name)),
            ("mkdir", lambda: os.mkdir(name)),
            ("symlink", lambda: os.symlink("crysis-check", name + "-link"))]
    if existing:
        st = os.lstat(existing)
        rows += [("write", lambda: os.close(os.open(existing, os.O_WRONLY))),
                 ("truncate", lambda: os.truncate(existing, st.st_size)),
                 ("chown", lambda: os.chown(existing, st.st_uid, st.st_gid,
                                            follow_symlinks=False))]
    return rows, (name, other, name + "-link")


def remove(names):
    for path in names:
        if os.path.isdir(path) and not os.path.islink(path):
            attempt(lambda: os.rmdir(path))
        elif os.path.lexists(path):
            attempt(lambda: os.unlink(path))


def refused(label, folder, existing):
    """Return the writes in FOLDER and on EXISTING that did not fail with
    EROFS, taking back each that went through."""
    rows, names = tries(folder, existing)
    bad = []
    for op, call in rows:
        code = attempt(call)
        remove(names)
        if code != errno.EROFS:
            bad.append("%s %s: %s" % (label, op, errno.errorcode.get(code, "allowed")))
    say("check: read-only: %s %s: %d of %d writes failed with EROFS"
        % (label, folder, len(rows) - len(bad), len(rows)))
    return bad


def allowed(folder):
    """Return the writes of a new file in FOLDER that failed."""
    rows, names = tries(folder, None)
    name = names[0]
    rows[2:2] = [("write", lambda: os.close(os.open(name + "-renamed", os.O_WRONLY))),
                 ("truncate", lambda: os.truncate(name + "-renamed", 0)),
                 ("chown", lambda: os.chown(name + "-renamed", UID, UID))]
    bad = []
    for op, call in rows:
        code = attempt(call)
        if code:
            bad.append("%s %s: %s" % (folder, op, errno.errorcode.get(code, code)))
    remove(names)
    say("check: writable: %s: %d of %d writes went through" % (folder, len(rows) - len(bad),
                                                                 len(rows)))
    return bad


def first_file(top):
    """Return a regular file in TOP, at most three levels down, or None."""
    for folder, dirs, files in os.walk(top):
        dirs.sort()
        if folder.count("/") - top.count("/") >= 3:
            dirs[:] = []
        for name in sorted(files):
            path = os.path.join(folder, name)
            if os.path.isfile(path) and not os.path.islink(path):
                return path
    return None


out, command = os.path.realpath(sys.argv[1]), sys.argv[2:]
if os.geteuid() != 0:
    die("run as root")
try:
    home = pwd.getpwuid(UID).pw_dir
except KeyError:
    die("no account of uid %d" % UID)
if not os.path.isdir(out) or under(out, home) or under(out, "/data/owner"):
    die("%s is no directory of the case's own" % out)
table = mounts()
if "/data" not in table or table.get("/data/owner", ("", []))[0] == "" or \
        os.stat("/data").st_dev == os.stat("/data/owner").st_dev or not os.path.isdir(BASE):
    die("no private /data with the partition on /data/owner and the game in %s" % BASE)

count = 0
for point, (kind, kept) in sorted(table.items()):
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

proton = os.path.realpath("/opt/proton") if os.path.isdir("/opt/proton") else None
desk = first_file(home)
links = []
bad = []
try:
    places = [("owner", OWNER, os.path.join(OWNER, "game/Bin64/Crysis.exe")),
              ("home", home, desk)]
    for label, target, existing in (("link-home", home, desk),
                                    ("link-proton", proton, proton and proton + "/proton")):
        if target:
            link = os.path.join(BASE, ".crysis-check-%s-%d" % (label, os.getpid()))
            os.symlink(target, link)
            links.append(link)
            places.append((label, link, existing and link + existing[len(target):]))
    for label, folder, existing in places:
        if existing and not os.path.isfile(existing):
            existing = None
        if not existing:
            say("check: read-only: %s %s: no existing file; its write, truncate and chown not tried"
                % (label, folder))
        bad += refused(label, folder, existing)
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

if [ "$1" = check ]; then
    mkdir -p "$here/out"
    unshare --mount --propagation private python3 -c "$seal_py" "$here/out" || fail check
    exit 0
fi

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
