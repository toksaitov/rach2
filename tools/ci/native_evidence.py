#!/usr/bin/env python3
"""Stage shared native run evidence without copying executable artifact objects."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'etude'))
from preparation_paths import filesystem_path
from retention_access import reading


def file_hash(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def references(value):
    if isinstance(value, dict):
        if {'sha256', 'size_bytes', 'locations'} <= value.keys():
            yield value
        else:
            for item in value.values():
                yield from references(item)
    elif isinstance(value, list):
        for item in value:
            yield from references(item)


def routine_proofs(source, output):
    """Retain original locators while transporting exact proof bytes by hash."""
    if not (source / 'routine.grant.json').is_file():
        return
    pending = []
    for name in ('routine.grant.json', 'routine.upstream.json', 'routine.completion.json'):
        path = source / name
        if path.is_file():
            pending.append(path.read_bytes())
    history = source / 'history'
    if history.is_dir():
        for path in sorted(history.iterdir()):
            if path.is_file() and not path.is_symlink():
                pending.append(path.read_bytes())
    directory = output / 'routine-proofs'
    directory.mkdir(mode=0o700)
    seen, failures = set(), []
    while pending:
        raw = pending.pop()
        try:
            documents = [json.loads(raw)]
        except (UnicodeError, ValueError):
            documents = []
            for line in raw.splitlines():
                try:
                    documents.append(json.loads(line))
                except (UnicodeError, ValueError):
                    continue
        for doc in documents:
            # The terminal history needs its preparation and load proofs, not
            # copies of executable artifacts or the source inventory.
            if isinstance(doc, dict) and doc.get('kind') in ('plan', 'run'):
                data = doc.get('data', {})
                doc = data.get('artifacts', []) if doc['kind'] == 'plan' else data.get('load')
            elif isinstance(doc, dict) and doc.get('format') == 'etude-preparation-nodes':
                doc = {key: doc.get(key) for key in
                       ('routine_grant', 'upstream_preparation', 'preparation')}
            refs = list(references(doc))
            for ref in refs:
                sha, size, locations = (ref.get(key) for key in ('sha256', 'size_bytes', 'locations'))
                try:
                    if (not isinstance(sha, str) or len(sha) != 64 or
                            any(c not in '0123456789abcdef' for c in sha) or
                            type(size) is not int or not 0 <= size <= 64 * 1024 * 1024 or
                            not isinstance(locations, list) or len(locations) != 1):
                        raise ValueError('invalid or ambiguous routine proof reference')
                    if sha in seen:
                        continue
                    path = filesystem_path(locations[0])
                    if (not path.is_absolute() or path.is_symlink() or not path.is_file() or
                            path.stat().st_size != size or file_hash(path) != sha):
                        raise ValueError('missing or changed routine proof: ' + str(path))
                    target = directory / sha
                    with path.open('rb') as src, target.open('xb') as dst:
                        shutil.copyfileobj(src, dst)
                    if target.stat().st_size != size or file_hash(target) != sha:
                        raise ValueError('routine proof changed while copying: ' + str(path))
                    os.chmod(target, 0o444)
                    seen.add(sha)
                    pending.append(target.read_bytes())
                except (OSError, ValueError, TypeError) as error:
                    failures.append(str(error))
    if failures:
        (output / 'routine-proof-errors.json').write_text(json.dumps(failures) + '\n')


def stage(source, output):
    source = source.resolve(strict=True)
    output = output.absolute()
    if output == source or source in output.parents:
        raise ValueError('evidence staging must be outside the source directory')
    output.mkdir(parents=True, exist_ok=False)
    routine_proofs(source, output)
    inventory = []
    for path in sorted(source.iterdir()):
        if path.name in ('objects', 'native-evidence', 'routine-proofs') or path.name.startswith(('core-', 'full-', 'push-')):
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
    for path in sorted((output / 'routine-proofs').glob('*')) if (output / 'routine-proofs').is_dir() else []:
        inventory.append(dict(path=path.relative_to(output).as_posix(), sha256=file_hash(path),
                              size_bytes=path.stat().st_size))
    failure = output / 'routine-proof-errors.json'
    if failure.is_file():
        inventory.append(dict(path=failure.name, sha256=file_hash(failure), size_bytes=failure.stat().st_size))
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
    if args.action == 'stage':
        with reading([args.source], bundle=True):
            stage(args.source, args.output)
    else:
        restore(args.source, args.output)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('ci: evidence: ' + str(error), file=sys.stderr)
        sys.exit(2)
