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
    for name in ('routine.grant.json', 'routine.upstream.json', 'routine.completion.json'):
        if (out / name).is_file():
            pending.append((out / name).read_bytes())
    history = out / 'history'
    if history.is_dir():
        pending += [path.read_bytes() for path in sorted(history.iterdir())
                    if path.is_file() and not path.is_symlink()]
    seen = set()
    while pending:
        data = pending.pop()
        try:
            documents = [json.loads(data)]
        except (ValueError, UnicodeDecodeError):
            documents = []
            for line in data.splitlines():
                try:
                    documents.append(json.loads(line))
                except (ValueError, UnicodeDecodeError):
                    continue
        proofs = []
        for document in documents:
            if isinstance(document, dict) and document.get('kind') in ('plan', 'run'):
                body = document.get('data', {})
                document = body.get('artifacts', []) if document['kind'] == 'plan' else body.get('load')
            elif isinstance(document, dict) and document.get('format') == 'etude-preparation-nodes':
                document = {key: document.get(key) for key in
                            ('routine_grant', 'upstream_preparation', 'preparation')}
            proofs += list(references(document))
        for proof in proofs:
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
            candidates += [target / sha, out / 'store/objects' / sha]
            # Cross-OS proof locators remain unchanged. Only this known shared
            # OUT is mapped, then every selected byte is checked by hash/size.
            request = out / 'routine.request.json'
            if request.is_file():
                original = json.loads(request.read_text()).get('out')
                if isinstance(original, str):
                    for value in proof['locations']:
                        if value.startswith(original.rstrip('/') + '/'):
                            candidates.append(out / value[len(original.rstrip('/')) + 1:])
            candidates += [root / Path(value.replace('\\', '/')).name
                           for root in (out, out / 'history') for value in proof['locations']]
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


if __name__ == '__main__':
    try:
        if len(sys.argv) != 2:
            raise ValueError('expected the retained routine OUT directory')
        pack(sys.argv[1])
    except (OSError, ValueError, TypeError, KeyError) as error:
        print('routine proof pack: ' + str(error), file=sys.stderr)
        raise SystemExit(2)
