#!/usr/bin/env python3
"""Stage shared native run evidence without copying executable artifact objects."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'etude'))
from preparation_paths import filesystem_path


def file_hash(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def stage(source, output):
    source = source.resolve(strict=True)
    output = output.absolute()
    if output == source or source in output.parents:
        raise ValueError('evidence staging must be outside the source directory')
    output.mkdir(parents=True, exist_ok=False)
    inventory = []
    for path in sorted(source.iterdir()):
        if path.name in ('objects', 'native-evidence') or path.name.startswith(('core-', 'full-', 'push-')):
            continue
        if path.is_symlink():
            raise ValueError('evidence contains a symlink: ' + str(path))
        paths = sorted(path.rglob('*')) if path.is_dir() else [path]
        for item in paths:
            if item.is_symlink():
                raise ValueError('evidence contains a symlink: ' + str(item))
            if not item.is_file():
                continue
            relative = item.relative_to(source)
            target = output / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(item, target)
            digest = file_hash(target)
            inventory.append(dict(path=relative.as_posix(), sha256=digest,
                                  size_bytes=target.stat().st_size))
    with (output / 'evidence.json').open('x') as stream:
        json.dump(dict(format='etude-native-evidence', schema_version=1, files=inventory), stream)
        stream.write('\n')


def regular_member(root, name):
    path = root
    for part in Path(name).parts:
        path = path / part
        if path.is_symlink():
            raise ValueError('evidence path contains a symlink: ' + str(path))
    return path


def restore(source, output):
    source = source.resolve(strict=True)
    output = output.absolute()
    if any(parent.is_symlink() for parent in (output, *output.parents)):
        raise ValueError('evidence destination contains a symlink')
    inventory = json.loads((source / 'evidence.json').read_text())
    if inventory.get('format') != 'etude-native-evidence' or inventory.get('schema_version') != 1:
        raise ValueError('not a native evidence inventory')
    seen = set()
    for entry in inventory['files']:
        name = entry['path']
        path = Path(name)
        if path.is_absolute() or ':' in name or '\\' in name or any(part in ('', '.', '..') for part in name.split('/')) or name in seen:
            raise ValueError('invalid evidence member: ' + name)
        seen.add(name)
        item = regular_member(source, name)
        target = regular_member(output, name)
        if target.exists():
            raise ValueError('evidence destination already exists: ' + name)
        if item.is_symlink() or not item.is_file() or item.stat().st_size != entry['size_bytes'] or file_hash(item) != entry['sha256']:
            raise ValueError('evidence bytes differ: ' + name)
    for name in sorted(seen):
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with (source / name).open('rb') as src, target.open('xb') as dst:
            shutil.copyfileobj(src, dst)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('stage', 'restore'))
    parser.add_argument('source', type=filesystem_path)
    parser.add_argument('output', type=filesystem_path)
    args = parser.parse_args()
    (stage if args.action == 'stage' else restore)(args.source, args.output)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('ci: evidence: ' + str(error), file=sys.stderr)
        sys.exit(2)
