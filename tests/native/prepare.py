#!/usr/bin/env python3
"""Prepare immutable native files and roots; execute no native test case."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

PROFILE = ("sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,"
           "^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,"
           "^dir_nlink,^extra_isize")


def cases(root):
    result = []
    seen = set()
    for name in ("probes.cases", "musl.cases", "rows.cases"):
        pending = ""
        for raw in (root / "tests/native" / name).read_text().splitlines():
            line = raw.split("#", 1)[0].rstrip()
            if line.endswith("\\"):
                pending += line[:-1] + " "
                continue
            words = (pending + line).split()
            pending = ""
            if not words:
                continue
            if words[0] != "case" or "--" not in words or words[1] in seen:
                raise ValueError(f"invalid or duplicate case in {name}: {words}")
            at = words.index("--")
            if at < 2 or not words[at + 1:]:
                raise ValueError(f"incomplete case in {name}")
            seen.add(words[1])
            result.append((words[1], words[2:at], words[at + 1:]))
        if pending:
            raise ValueError(f"continued line has no end in {name}")
    return result


def invoke(args, *, root, env, **kwargs):
    print("native-prepare:", " ".join(map(str, args)), flush=True)
    result = subprocess.run(list(map(str, args)), cwd=root, env=env, check=True,
                          capture_output=True, text=True, **kwargs)
    return result.stdout + result.stderr


def idmaps(root):
    result = {}
    for line in (root / "tests/native/maps").read_text().splitlines():
        words = line.split()
        if words[:1] == ["idmap"]:
            for pair in words[1:]:
                old, new = pair.split(":")
                if old in result or new in result.values():
                    raise ValueError("duplicate native id mapping")
                result[old] = new
    return result


def manifest(root, extras, build, ids):
    lines = []
    for line in (root / "krffs/manifests/test.manifest").read_text().splitlines():
        fields = line.split(None, 3)
        if len(fields) < 3 or fields[0] not in ("file", "dir", "text", "exec-dir"):
            lines.append(line)
            continue
        owner = fields[1].split(":")
        if len(owner) == 3:
            fields[1] = ":".join((owner[0], ids.get(owner[1], owner[1]),
                                  ids.get(owner[2], owner[2])))
        if fields[0] == "text" and len(fields) == 4:
            kind = fields[2]
            text = []
            for item in fields[3].split(r"\n"):
                parts = item.split(":")
                if kind in ("/etc/passwd", "/etc/group") and len(parts) >= 3:
                    parts[2] = ids.get(parts[2], parts[2])
                    if kind == "/etc/passwd" and len(parts) >= 4:
                        parts[3] = ids.get(parts[3], parts[3])
                    item = ":".join(parts)
                elif kind == "/etc/sudoers":
                    item = ids.get(item, item)
                text.append(item)
            fields[3] = r"\n".join(text)
        lines.append(" ".join(fields))
    for extra in extras:
        name = Path(extra).name
        if (root / build / "tests" / name).is_file():
            continue
        if any(re.match(rf"^(file|text)\s+\S+\s+/{re.escape(name)}(\s|$)", line)
               for line in lines):
            continue
        source = Path("tests") / extra if extra.startswith("native/") else build / "tests" / extra
        mode = "0755" if os.access(root / source, os.X_OK) else "0644"
        lines.append(f"file {mode} /{name} {source}")
    return "\n".join(lines) + "\n"


def blank(path, mib):
    with path.open("wb") as stream:
        stream.truncate(mib * 1048576)


def root_image(root, out, kind, mib, text, env, work, tools):
    manifest_file = work / "root.manifest"
    manifest_file.write_text(text)
    if kind == "krffs":
        blank(out, mib + 10)
        invoke([tools["mkfs"], out], root=root, env=env)
        staged = invoke([tools["stage"], manifest_file, out], root=root, env=env)
        match = re.search(r"bytes used, ([0-9]+) free", staged)
        if not match or int(match[1]) < mib * 1048576:
            raise ValueError(f"root {out.name} has insufficient free space: {staged}")
        return
    if kind not in ("ext2", "ext2-dir"):
        raise ValueError(f"unknown root kind: {kind}")
    tree = work / "staged"
    shutil.rmtree(tree, ignore_errors=True)
    nodes = invoke([tools["stage"], manifest_file, "--tree", tree], root=root, env=env)
    blank(out, mib + 16 if kind == "ext2" else 32)
    invoke([tools["mke2fs"], "-q", "-F", "-t", "ext2", "-b", "4096", "-I", "128",
            "-O", PROFILE, "-L", "rach2", "-d", tree, out], root=root, env=env)
    edits = []
    for line in nodes.splitlines():
        fields = line.split()
        if fields[:1] == ["node"]:
            for field, value in zip(("mode", "uid", "gid"), fields[1:4]):
                edits.append(f"set_inode_field {fields[4]} {field} {value}")
    commands = work / "debugfs"
    commands.write_text("\n".join(edits) + "\n")
    output = invoke([tools["debugfs"], "-w", "-f", commands, out], root=root, env=env)
    if any(line and not line.startswith("debugfs") for line in output.splitlines()):
        raise ValueError(output)
    invoke([tools["e2fsck"], "-fn", out], root=root, env=env)


def copy_file(root, tree, path):
    source = root / path
    target = tree / path
    if not source.is_file():
        raise ValueError(f"prepared input is missing: {source}")
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists():
        shutil.copy2(source, target)
    return target


def program_path(build, name):
    return Path("tests") / name if name.startswith(("native/", "system/")) else build / "tests" / name


def inputs(root, tree, build, inventory):
    selected = set()
    programs = set()
    for _, fixtures, argv in inventory:
        programs.add(program_path(build, argv[0]))
        selected.add(program_path(build, argv[0]))
        for fixture in fixtures:
            kind, _, value = fixture.partition(":")
            if kind == "stage":
                selected.update(program_path(build, name) for name in value.split(","))
            elif kind == "file":
                selected.add(Path(value))
            elif kind == "pty":
                selected.add(Path("tests/native") / value)
    selected.add(build / "tests/native/etude")
    selected.add(build / "tools/etude/guest/etude-guest")
    selected.add(Path("tests/native/tccbuild.sh"))
    for name in ("probes.cases", "musl.cases", "rows.cases", "maps", "Readme.md"):
        selected.add(Path("tests/native") / name)
    invocation = set(selected)
    # Include the source recipe for a local TinyCC rebuild of the controller.
    for base in ("tests/native", "tools/etude"):
        for path in (root / base).rglob("*"):
            if path.is_file() and path.suffix in (".c", ".h", ".mk"):
                selected.add(path.relative_to(root))
    for name in ("sha256.c", "sha256.h"):
        selected.add(Path("utils") / name)
    for path in (root / "vendor/gnu/agents/packages").glob("*.sh"):
        selected.add(path.relative_to(root))
    for path in sorted(selected):
        target = copy_file(root, tree, path)
        if path in programs:
            target.chmod(target.stat().st_mode | 0o111)
    return [tree / path for path in sorted(invocation)]


def images(root, output, build, inventory, env, work, tools):
    output.mkdir(parents=True)
    for line in (root / "tests/native/maps").read_text().splitlines():
        fields = line.split()
        if fields[:1] != ["image"]:
            continue
        _, name, kind, size, *source = fields
        path = output / name
        mib = int(size)
        if source:
            if len(source) != 2 or source[0] != "from":
                raise ValueError(line)
            origin = source[1]
            if build != Path("build") and origin.startswith("build/") and not origin.startswith("build/tests/system/"):
                origin = str(build) + origin[5:]
            shutil.copy2(root / origin, path)
        elif kind == "exfat":
            invoke([tools["exfat"], "--make", path, "--sectors", mib * 2048], root=root, env=env)
        else:
            blank(path, mib)
            if kind == "krffs":
                invoke([tools["mkfs"], path], root=root, env=env)
            elif kind == "ext2":
                invoke([tools["mke2fs"], "-q", "-F", "-t", "ext2", "-b", "4096", "-I", "128", "-O", PROFILE, path], root=root, env=env)
            elif kind == "vfat":
                invoke([tools["mformat"], "-i", path, "-F", "::"], root=root, env=env)
            else:
                raise ValueError(f"unknown image kind: {kind}")
    made = {}
    roots = []
    ids = idmaps(root)
    for name, fixtures, _ in inventory:
        kind = None
        mib = 64
        extras = []
        for fixture in fixtures:
            if fixture.startswith("root:"):
                words = fixture.split(":")
                kind = words[1]
                if len(words) == 3:
                    mib = int(words[2])
            elif fixture.startswith("stage:"):
                extras.extend(fixture[6:].split(","))
        if not kind:
            continue
        text = manifest(root, extras, build, ids)
        digest = hashlib.sha256(f"{kind} {mib}\n{text}".encode()).hexdigest()[:12]
        file = f"r-{digest}.img"
        if file not in made:
            root_image(root, output / file, kind, mib, text, env, work, tools)
            made[file] = text
        roots.append(f"{name} {file}\n")
    (output / "roots").write_text("".join(roots))


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            value.update(block)
    return value.hexdigest()


def prepare(args):
    root = Path(__file__).resolve().parents[2]
    build = Path("build/x86_64" if args.architecture == "x86_64" else "build")
    destination = (root / args.output).resolve()
    if root / "build" not in destination.parents:
        raise ValueError("output must be below this tree's build directory")
    destination.parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="rach2-native-prepare.", dir=destination.parent))
    tools = {"mkfs": root / "build/krffs/mkfs.krffs", "stage": root / "build/krffs/stageimage.krffs",
             "exfat": root / "build/tests/host/mkexfatfixture", "mke2fs": args.mke2fs,
             "debugfs": args.debugfs, "e2fsck": args.e2fsck, "mformat": args.mformat}
    env = dict(os.environ, E2FSPROGS_FAKE_TIME="1")
    env["RACH2_PATH_MAP"] = "" if args.architecture == "i686" else (
        f"build/man/=build/man/ build/tests/system/=build/tests/system/ build/={build}/ "
        f"vendor/musl/install/={build}/vendor-work/musl/install/")
    try:
        payload = work / "payload"
        tree = payload / "tree"
        stage = payload / "stage"
        stage.mkdir(parents=True)
        inventory = cases(root)
        files = inputs(root, tree, build, inventory)
        image_dir = tree / build / "tests/native/images"
        images(root, image_dir, build, inventory, env, work, tools)
        files.extend(path for path in image_dir.iterdir() if path.name != "roots")
        for file in files:
            name = file.name
            target = stage / name
            if target.exists():
                if digest(target) != digest(file):
                    raise ValueError(f"two staged inputs have different bytes: {name}")
            else:
                os.link(file, target)
        records = [f"{digest(path)}  {path.relative_to(tree)}\n"
                   for path in sorted(tree.rglob("*")) if path.is_file()]
        (tree / "assets.sha256").write_text("".join(records))
        (payload / "prepared.json").write_text(json.dumps({"schema_version": 1, "architecture": args.architecture,
            "cases": [name for name, _, _ in inventory], "account": "maps", "roots": str(image_dir.relative_to(payload) / "roots")}, indent=2) + "\n")
        if destination.exists():
            if not (destination / "prepared.json").is_file():
                raise ValueError("existing output is not a native preparation")
            shutil.rmtree(destination)
        payload.rename(destination)
        print(f"native-prepare: {len(inventory)} cases in {destination}")
    finally:
        shutil.rmtree(work)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--architecture", choices=("i686", "x86_64"), required=True)
    parser.add_argument("--output", required=True)
    for tool in ("mke2fs", "debugfs", "e2fsck", "mformat"):
        parser.add_argument("--" + tool, default=tool)
    prepare(parser.parse_args())
