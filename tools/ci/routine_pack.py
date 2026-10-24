"""Retain the exact immutable proof bytes needed by routine composition."""
import json
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'etude'))
from preparation_paths import filesystem_path
from native_evidence import references, file_hash


def pack(out, roots=()):
    out = filesystem_path(out)
    target = out / 'routine-proofs'
    target.mkdir(mode=0o700, exist_ok=True)
    known = {}
    for root in (out / 'routine-proofs-input', *map(filesystem_path, roots)):
        if root.is_dir():
            for path in root.rglob('*'):
                if path.is_file() and not path.is_symlink():
                    digest, size = file_hash(path), path.stat().st_size
                    known.setdefault((digest, size), path)
    pending = []
    for name in ('routine.grant.json', 'routine.upstream.json'):
        if (out / name).is_file():
            pending.append((out / name).read_bytes())
    seen = set()
    while pending:
        data = pending.pop()
        try:
            document = json.loads(data)
        except (ValueError, UnicodeDecodeError):
            continue
        for proof in references(document):
            sha, size, locations = (proof.get(name) for name in ('sha256', 'size_bytes', 'locations'))
            if (not isinstance(sha, str) or len(sha) != 64 or
                    any(c not in '0123456789abcdef' for c in sha) or type(size) is not int or
                    not 0 <= size <= 64 * 1024 * 1024 or not isinstance(locations, list) or
                    len(locations) != 1 or not isinstance(locations[0], str)):
                raise ValueError('invalid or ambiguous routine proof reference')
            key = sha, size
            if key in seen:
                continue
            candidates = [filesystem_path(value) for value in proof['locations']]
            candidates += [known[key]] if key in known else []
            source = next((path for path in candidates if path.is_file() and not path.is_symlink()
                           and (file_hash(path), path.stat().st_size) == key), None)
            if source is None:
                raise ValueError('unreachable routine proof: ' + key[0])
            destination = target / key[0]
            if destination.exists():
                if (file_hash(destination), destination.stat().st_size) != key:
                    raise ValueError('changed retained routine proof: ' + key[0])
            else:
                with destination.open('xb') as stream, source.open('rb') as original:
                    shutil.copyfileobj(original, stream)
                if (file_hash(destination), destination.stat().st_size) != key:
                    raise ValueError('routine proof changed while copying: ' + key[0])
                destination.chmod(0o444)
            seen.add(key)
            pending.append(destination.read_bytes())
    return target
