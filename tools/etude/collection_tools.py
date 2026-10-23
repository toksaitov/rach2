#!/usr/bin/env python3
"""Run collection builds and image commands in the local tooling machine."""

import os
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile


MTOOLS = tuple("mattrib mcopy mdel mdeltree mdir mformat minfo mlabel mmd mmove mrd mren mtype".split())
TOOLS = MTOOLS + ("mke2fs", "debugfs", "e2fsck", "mkfs.fat", "fsck.fat", "mkfs.exfat", "fsck.exfat")


def run_child(args, **kwargs):
    """Wait for a child, ending it before scratch is removed after a signal."""
    child = subprocess.Popen(args, **kwargs)
    try:
        return child.wait()
    except BaseException:
        child.terminate()
        child.wait()
        raise


def interrupted(number, frame):
    """Unwind the active transfer after stopping its supervised child."""
    raise InterruptedError("interrupted by signal " + str(number))


def local(path):
    """Translate a native Windows path for the supporting MSYS Python."""
    if len(path) > 2 and path[1] == ":":
        path = subprocess.check_output(["cygpath", "-u", path], text=True).strip()
    return Path(path).absolute()


def native(path):
    """Give a path that native Windows children can open."""
    if sys.platform == "msys" or sys.platform == "cygwin":
        return subprocess.check_output(["cygpath", "-m", str(path)], text=True).strip()
    return str(path)


class Job:
    """Own one command's transfers and attached disks until it ends."""

    def __init__(self, root, work):
        self.root, self.work = root, work
        self.inputs, self.disks, self.outputs = [], [], []
        self.back = []

    def disk(self, text):
        """Attach an image and preserve mtools' byte offset suffix."""
        path, sep, offset = text.partition("@@")
        path = local(path)
        if not path.is_file() or (sep and not offset.isdecimal()):
            raise ValueError("the disk or its byte offset is invalid: " + text)
        if path not in self.disks:
            self.disks.append(path)
        return '"$ETUDE_DISK%d"%s' % (self.disks.index(path) + 1, shlex.quote(sep + offset))

    def file(self, text, output=False):
        """Stage a local operand, bringing writable results back separately."""
        path = local(text)
        name = "file%d" % len(self.inputs)
        directory = self.work / name
        directory.mkdir()
        staged = directory / path.name
        if path.is_dir():
            shutil.copytree(path, staged)
        elif path.exists():
            shutil.copy2(path, staged)
        elif not output:
            raise ValueError("the input is absent: " + text)
        self.inputs.append(directory)
        if output:
            self.outputs.append(name)
            self.back.append((name + "/" + path.name, path))
        return shlex.quote(name + "/" + path.name)

    def run(self, script, volume=256, memory=512):
        """Run a supervised tooling job and publish its transferred outputs."""
        script_path = self.work / "job.sh"
        script_path.write_text(script, encoding="utf-8")
        out = self.work / "out"
        out.mkdir()
        etude = self.root / "build/tools/etude/etude"
        cache = local(os.environ.get("ETUDE_TOOLING_CACHE", str(self.root / "build/tooling")))
        accel = os.environ.get("RACH2_QEMU_ACCEL")
        if not accel:
            raise ValueError("RACH2_QEMU_ACCEL must name the requested accelerator")
        args = [native(etude), "provision", "tooling",
                "-r", native(self.root / "tools/etude/tooling"),
                "-C", native(cache), "-a", accel, "run", "-c", "1", "-m", str(memory),
                "-s", str(volume), "-O", native(out)]
        for flag, paths in (("-i", self.inputs), ("-d", self.disks)):
            for path in paths:
                args += [flag, native(path)]
        for name in self.outputs:
            args += ["-o", name]
        args.append(native(script_path))
        env = dict(os.environ, MSYS2_ARG_CONV_EXCL="*", MSYS_NO_PATHCONV="1")
        status = run_child(args, env=env)
        for name, dest in self.back:
            source = out / name
            if not source.exists():
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, dest, dirs_exist_ok=True)
            else:
                shutil.copy2(source, dest)
        return status

    def provision(self):
        """Make the recipe's tooling machine under the enclosing preparation."""
        if not os.environ.get("RACH2_QEMU_ACCEL"):
            raise ValueError("RACH2_QEMU_ACCEL must name the requested accelerator")
        return run_child([
            native(self.root / "build/tools/etude/etude"), "provision", "tooling",
            "-r", native(self.root / "tools/etude/tooling"), "-C",
            native(local(os.environ.get("ETUDE_TOOLING_CACHE", str(self.root / "build/tooling")))),
            "-a", os.environ["RACH2_QEMU_ACCEL"], "prepare", "-c", "1"])


def image_command(job, tool, args):
    """Translate the image commands used by collection scenarios and scripts."""
    if tool not in TOOLS:
        raise ValueError("no image command adapter for " + tool)
    words = [shlex.quote(tool)] + [shlex.quote(a) for a in args]
    if tool in MTOOLS:
        try:
            image = args.index("-i") + 1
            words[image + 1] = job.disk(args[image])
        except (ValueError, IndexError) as error:
            raise ValueError("mtools needs an explicit -i image") from error
        if tool == "mcopy":
            # The collection uses one source and destination; option operands
            # precede them. Neither a label nor a sector count is a pathname.
            if len(args) < image + 3:
                raise ValueError("mcopy needs a source and destination")
            for index in range(image + 1, len(args)):
                if args[index].startswith("-"):
                    continue
                if not args[index].startswith("::"):
                    words[index + 1] = job.file(args[index], output=index == len(args) - 1)
    else:
        if not args:
            raise ValueError("the image command has no disk operand")
        # e2fsprogs accepts image?offset=N; keep that suffix on the device.
        at = len(args) - 2 if args[-1].isdecimal() else len(args) - 1
        disk, sep, offset = args[at].partition("?offset=")
        words[at + 1] = job.disk(disk) + shlex.quote(sep + offset)
        if tool == "mke2fs" and "-d" in args:
            at = args.index("-d") + 1
            words[at + 1] = job.file(args[at])
        if tool == "debugfs" and "-R" in args:
            index = args.index("-R") + 1
            request = shlex.split(args[index])
            if request and request[0] in ("dump", "rdump", "write"):
                at = 1 if request[0] == "write" else len(request) - 1
                request[at] = job.file(request[at], output=request[0] != "write")
                # file() quotes for the outer shell; debugfs parses the
                # request itself, so the staged name needs no shell quotes.
                request[at] = shlex.split(request[at])[0]
                words[index + 1] = shlex.quote(shlex.join(request))
    # A failed command may still have changed an output. Return those bytes
    # with its original status, including e2fsck's status bit mask.
    return "set +e\n" + " ".join(words) + "\nstatus=$?\nexit \"$status\"\n"


def native_goal(goal):
    """Select native executables and inventories, retaining portable guest data."""
    parts = Path(goal).parts
    if parts[:2] == ('build', 'x86_64'):
        parts = ('build', *parts[2:])
    if parts[:4] == ('build', 'tools', 'etude', 'guest'):
        return False
    if parts[:4] in (('build', 'tests', 'cases', 'machine'),
                     ('build', 'tests', 'cases', 'host')):
        return len(parts) == 5
    if parts[:3] == ('build', 'tests', 'host-suite'):
        return (len(parts) == 4 and (parts[3].endswith('.inventory.json') or
                                    parts[3] == 'vfs_unmount') or
                len(parts) == 5 and parts[3] in ('scripts', 'schemas') or
                parts[3:] == ('tests', 'usbstamp_hosttest'))
    if parts[:3] == ('build', 'tests', 'host'):
        return len(parts) == 4 and parts[3] not in ('exfat_writer', 'tooling_machine')
    return (parts[:2] in (('build', 'tools'), ('build', 'krffs')) or
            len(parts) == 4 and parts[:3] == ('build', 'tests', 'system'))


def workspace(root, arch):
    """Name this tree's retained build disk, apart from every other clone."""
    cache = local(os.environ.get("ETUDE_TOOLING_CACHE", str(root / "build/tooling")))
    key = hashlib.sha256(os.fsencode(root)).hexdigest()[:16]
    return cache / ("collection-" + key + "-" + arch + ".img")


def style_case(job, arch, case_id):
    """Run source analysis with the Linux tools the Windows endpoint lacks."""
    disk = workspace(job.root, arch)
    copy = job.work / "workspace.img"
    status = run_child(["qemu-img", "convert", "-f", "raw", "-O", "raw", "-S", "4k",
                        native(disk), native(copy)])
    if status:
        return status
    manifest = json.loads((job.root / "tests/cases/host.style.json").read_text())
    case = next((c for c in manifest["cases"] if c["case_id"] == "host.style." + case_id), None)
    if case is None:
        raise ValueError("unknown style case: " + case_id)
    ab = "build/x86_64" if arch == "x86_64" else "build"
    goals = [(ab if p.get("per_architecture") else "build") + "/" + p["node"]
             for p in case["prepare"]]
    job.disks.append(copy)
    job.outputs.append("style-evidence")
    script = '''set -eu
mkdir "$ETUDE_WORK/style-evidence"
export TMPDIR="$ETUDE_WORK/style-evidence"
mkdir /mnt/collection
mount "$ETUDE_DISK1" /mnt/collection
trap 'cd /; umount /mnt/collection' EXIT
cd /mnt/collection/tree
sh tools/setup/elfwrap.sh i686 /root/bin
sh tools/setup/elfwrap.sh x86_64 /root/bin
export PATH=/root/bin:$PATH
'''
    script += "make -j1 ARCH=" + shlex.quote(arch) + " " + shlex.join(goals) + "\n"
    script += "sh tests/cases/host/style.sh " + shlex.quote(case_id) + "\n"
    status = job.run(script, memory=2048)
    evidence = job.work / "out/style-evidence"
    if status and evidence.is_dir():
        dest = local(os.environ.get("TMPDIR", str(job.root / "build/tests"))) / "tooling-style-evidence"
        shutil.copytree(evidence, dest, dirs_exist_ok=True)
        print("collection-tools: evidence: " + str(dest), file=sys.stderr)
    return status


def prepare(job, arch, kernel, goals):
    """Build guest artifacts locally in Linux and host programs in Windows."""
    root = job.root
    if job.provision():
        return 1
    native_goals = [g for g in goals if native_goal(g)]
    guest_goals = [g for g in goals if not native_goal(g)]
    ab = "build/x86_64" if arch == "x86_64" else "build"
    guest_goals.append(ab + "/rach2" + ("-" + kernel if kernel else "") + "/kernel")
    files = subprocess.check_output(["git", "-C", str(root), "ls-files", "-z"]).split(b"\0")
    paths = [os.fsdecode(p) for p in files if p and (root / os.fsdecode(p)).is_file()]
    index = job.work / "source-index"
    index.write_bytes(b"\0".join(os.fsencode(p) for p in paths) + b"\0")
    job.inputs.append(index)
    has_head = subprocess.call(["git", "-C", str(root), "rev-parse", "--verify", "HEAD"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0
    if has_head:
        bundle = job.work / "source.bundle"
        subprocess.check_call(["git", "-C", str(root), "bundle", "create", str(bundle), "HEAD"])
        job.inputs.append(bundle)
    for name in ("ports/doom/doom1.wad", "krffs/recovery_authorized_keys"):
        if (root / name).is_file() and name not in paths:
            paths.append(name)
    source = job.work / "source.tar"
    with tarfile.open(source, "w") as archive:
        for name in paths:
            archive.add(root / name, arcname=name, recursive=False)
    job.inputs.append(source)
    cache = local(os.environ.get("ETUDE_TOOLING_CACHE", str(root / "build/tooling")))
    cache.mkdir(parents=True, exist_ok=True)
    disk = workspace(root, arch)
    if not disk.exists():
        part = job.work / "workspace.img"
        subprocess.check_call(["qemu-img", "create", "-q", "-f", "raw", native(part), "96G"])
        part.rename(disk)
    job.disks.append(disk)
    products = job.work / "products.tar"
    subprocess.check_call(["qemu-img", "create", "-q", "-f", "raw", native(products), "64G"])
    job.disks.append(products)
    goals_text = " ".join(shlex.quote(g) for g in guest_goals)
    script = "set -eu\n"
    script += 'blkid -s TYPE -o value "$ETUDE_DISK1" | grep -qx ext4 || mkfs.ext4 -q -F "$ETUDE_DISK1"\n'
    script += '''mkdir /mnt/collection
mount "$ETUDE_DISK1" /mnt/collection
trap 'cd /; umount /mnt/collection' EXIT
mkdir -p /mnt/collection/tree
mkdir "$ETUDE_WORK/source"
tar -xf "$ETUDE_WORK/source.tar" -C "$ETUDE_WORK/source"
tar -tf "$ETUDE_WORK/source.tar" | sort > "$ETUDE_WORK/source.list"
cd /mnt/collection/tree
if [ -f .etude-sources ]; then
    comm -23 .etude-sources "$ETUDE_WORK/source.list" | while IFS= read -r name; do
        rm -f "./$name"
    done
fi
rsync -rc "$ETUDE_WORK/source/" ./
cp "$ETUDE_WORK/source.list" .etude-sources
git init -q
git config core.filemode false
if [ -f "$ETUDE_WORK/source.bundle" ]; then
    git fetch -q "$ETUDE_WORK/source.bundle" HEAD
    git update-ref refs/heads/master FETCH_HEAD
    git symbolic-ref HEAD refs/heads/master
    git read-tree HEAD
fi
git add -u
git --literal-pathspecs add -f --pathspec-from-file="$ETUDE_WORK/source-index" --pathspec-file-nul
sh tools/setup/elfwrap.sh i686 /root/bin
sh tools/setup/elfwrap.sh x86_64 /root/bin
export PATH=/root/bin:$PATH
'''
    script += "make -j1 ARCH=" + shlex.quote(arch) + " components\n"
    if kernel:
        script += "make -j1 ARCH=" + shlex.quote(arch) + " " + shlex.quote(kernel + "-kernel") + "\n"
    script += "make -j1 ARCH=" + shlex.quote(arch) + " " + goals_text + "\n"
    script += 'tar --sparse -cf "$ETUDE_DISK2" ' + goals_text + "\n"
    input_bytes = sum(path.stat().st_size for path in job.inputs)
    volume = max(512, (input_bytes + 1048575) // 1048576 + 64)
    status = job.run(script, volume=volume, memory=4096)
    if status:
        return status
    subprocess.check_call(["tar", "-xf", str(products), "-C", str(root)])
    if native_goals:
        return subprocess.call(["make", "-C", str(root), "--no-print-directory", "-j1",
                                "ARCH=" + arch, "HOSTCC=/ucrt64/bin/gcc", "CC=/ucrt64/bin/gcc",
                                "HOST_TEST_PLATFORM=windows", *native_goals])
    return 0


def main():
    """Run one adapter request, removing its private transfers on every exit."""
    if sys.argv[1:] == ["list"]:
        print(" ".join(TOOLS))
        return 0
    mode, root_text, *args = sys.argv[1:]
    root = local(root_text)
    scratch = root / "build/tests"
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="rach2-collection.", dir=scratch) as work:
        job = Job(root, Path(work))
        if mode == "tool":
            return job.run(image_command(job, args[0], args[1:]))
        if mode == "prepare":
            return prepare(job, args[0], args[1], args[2:])
        if mode == "style":
            return style_case(job, args[0], args[1])
        raise ValueError("unknown adapter request: " + mode)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("collection-tools: " + str(error), file=sys.stderr)
        sys.exit(2)
