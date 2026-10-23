"""Checked files and ownership for the selected site's evidence registry."""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import tempfile

from preparation_paths import canonical_path, native_path


def require(ok, message):
    if not ok:
        raise ValueError(message)


def decode(raw):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'duplicate JSON member: ' + key)
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=unique)


def document(path):
    return decode(path.read_bytes())


def checked_file(path):
    require(not path.is_symlink() and path.is_file(), 'not a regular file: ' + str(path))
    before = path.stat()
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    after = path.stat()
    identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns)
    require(identity(before) == identity(after), 'file changed while reading: ' + str(path))
    return dict(path=native_path(path), sha256=digest.hexdigest(), size_bytes=after.st_size)


def checked(ref):
    path = canonical_path(ref['path'])
    actual = checked_file(path)
    require(all(actual[key] == ref[key] for key in ('sha256', 'size_bytes')),
            'retained reference changed: ' + str(path))
    return path


def write(path, value):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False,
                                         prefix='.retention-') as stream:
            temporary = Path(stream.name)
            json.dump(value, stream, sort_keys=True)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def files(root):
    result = {}
    for path in sorted(root.rglob('*')):
        require(not path.is_symlink(), 'bundle contains a symlink: ' + str(path))
        if path.is_file():
            result[path.relative_to(root).as_posix()] = checked_file(path)
        else:
            require(path.is_dir(), 'bundle contains a special file: ' + str(path))
    return result


@contextlib.contextmanager
def registry(policy_path, site):
    policy = document(canonical_path(policy_path))
    require(policy.get('format') == 'etude-retention-policy' and policy.get('schema_version') == 1,
            'not a version-one retention policy')
    require(checked_file(canonical_path(site))['sha256'] == policy['site_sha256'],
            'retention policy belongs to a different site')
    for name in ('max_bytes', 'max_age_days', 'reserve_bytes', 'issue_snapshot_max_age_seconds'):
        require(type(policy.get(name)) is int and policy[name] > 0, 'invalid retention limit: ' + name)
    require(policy['reserve_bytes'] <= policy['max_bytes'], 'run reservation exceeds retention limit')
    require(isinstance(policy['root'], str) and not any(c in policy['root'] for c in '\r\n\0'),
            'retention root has invalid characters')
    root = canonical_path(policy['root'])
    require(root.is_dir() and not root.is_symlink(), 'retention root is unavailable')
    for name in ('registry', 'runs', 'accepted'):
        require((root / name).is_dir() and not (root / name).is_symlink(),
                'retention registry directory is absent or aliased')
    marker = document(root / '.etude-retention-root.json')
    require(marker == dict(format='etude-retention-root', schema_version=1,
                           root=native_path(root), site_sha256=policy['site_sha256']),
            'retention root ownership marker differs')
    lock = root / '.registry-lock'
    require(not lock.is_symlink(), 'registry lock is a symlink')
    with lock.open('a+b') as stream:
        if stream.tell() == 0:
            stream.write(b'\0')
            stream.flush()
        stream.seek(0)
        if os.name == 'nt':
            import msvcrt
            msvcrt.locking(stream.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        try:
            yield policy, root
        finally:
            stream.seek(0)
            if os.name == 'nt':
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
