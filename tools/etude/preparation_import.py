#!/usr/bin/env python3
"""Export verified Linux artifacts, then prepare native collection inputs.

Only preparation invokes this module. The C adapter owns its processes,
artifact record, cleanup and same-run timing receipt.
"""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import stat
import subprocess
import sys

from preparation_paths import cygpath, filesystem_path, native_path, canonical_path
from collection_tools import native_goal

FORMAT = 'etude-preparation-producer'
AUXILIARY = (
    'vendor/musl/musl-1.2.5/src/crypt/crypt_sha512.c',
    'build/x86_64/vendor-work/musl/musl-1.2.5/src/crypt/crypt_sha512.c',
    'vendor/musl/install/lib/libc.a', 'vendor/musl/.stamp',
)
NATIVE_MAGIC = {'darwin': (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe'),
                'windows': (b'MZ',), 'linux': (b'\x7fELF',)}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def relative(value):
    path = Path(value)
    require(value and not path.is_absolute() and '..' not in path.parts and
            '\\' not in value and ':' not in value and '\0' not in value and
            '\n' not in value and '\r' not in value and
            not re.match(r'^[A-Za-z]:', value), 'unsafe relative path: ' + value)
    return value


def describe(path):
    before = path.lstat()
    require(stat.S_ISREG(before.st_mode), 'not a regular file: ' + str(path))
    value = dict(sha256=digest(path), size_bytes=before.st_size)
    after = path.lstat()
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns,
             before.st_ctime_ns) == (after.st_dev, after.st_ino, after.st_size,
                                    after.st_mtime_ns, after.st_ctime_ns),
            'file changed while hashing: ' + str(path))
    return value


def verify(path, value):
    require(describe(path) == value, 'changed input: ' + str(path))


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def write_json(path, value):
    with path.open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')


def run(argv, tree, **kwargs):
    print('prepare import: ' + shlex.join(map(str, argv)), flush=True)
    return subprocess.run(list(map(str, argv)), cwd=tree, check=True, **kwargs)


def git(tree, *args):
    return subprocess.check_output(['git', *args], cwd=tree).strip()


def source(tree, commit):
    require(re.fullmatch('[0-9a-f]{40}', commit), 'invalid source commit')
    require(git(tree, 'rev-parse', 'HEAD').decode() == commit, 'source commit differs')
    require(not git(tree, 'status', '--porcelain', '--untracked-files=all'),
            'source tree has uncommitted files')
    paths = git(tree, 'ls-files', '-z').split(b'\0')
    # Git symlinks are source bytes too; resolve them only within this tree.
    return {os.fsdecode(p): dict(sha256=digest(tree / os.fsdecode(p)),
                                size_bytes=(tree / os.fsdecode(p)).stat().st_size)
            for p in paths if p and (tree / os.fsdecode(p)).is_file()}


def manifest_goals(tree):
    goals = set()
    for path in sorted((tree / 'tests/cases').glob('*.json')):
        for case in read_json(path).get('cases', []):
            arches = case['architectures']
            for arch in (['i686'] if arches == 'none' else arches):
                for artifact in case.get('prepare', []):
                    prefix = 'build/x86_64' if artifact['per_architecture'] and arch == 'x86_64' else 'build'
                    goals.add(relative(prefix + '/' + artifact['node']))
    return goals


def role(goal):
    """Classify the declared build output, never a user-supplied role map."""
    if '/host-suite/' in goal and goal.endswith('.inventory.json'):
        return 'inventory'
    return 'native' if native_goal(goal) else 'portable'



def record_files(text):
    lines = text.splitlines()
    require(lines and lines[0] == 'etude-prepare 1', 'not a preparation record')
    files = {}
    for line in lines[1:]:
        if line.startswith(('tree ', 'arch ', 'kernel ')):
            continue
        match = re.fullmatch(r'file ([0-9a-f]{64}) ([0-9]+) ([0-9]+) (-?[0-9]+) (.+)', line)
        require(match is not None, 'invalid preparation record line')
        sha, size, _, _, name = match.groups()
        require(relative(name) not in files, 'duplicate producer artifact: ' + name)
        files[name] = dict(sha256=sha, size_bytes=int(size))
    require(files, 'empty preparation record')
    return files


def receipt_reading(value):
    utc = value.get('utc', '')
    require(isinstance(utc, str) and re.fullmatch(
        r'[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\.[0-9]{1,6})?Z', utc),
        'invalid producer UTC reading')
    datetime.fromisoformat(utc.removesuffix('Z') + '+00:00')
    for key in ('monotonic_us', 'resolution_us'):
        number = value.get(key)
        require(type(number) is int and 0 <= number <= 9007199254740991,
                'invalid producer clock reading')
    require(value['resolution_us'] > 0, 'unknown producer clock resolution')
    return value['monotonic_us']


def receipt_check(receipt, commit, record):
    require(receipt.get('format') == 'etude-preparation-receipt' and
            receipt.get('schema_version') == 1 and receipt.get('outcome') == 'pass' and
            receipt.get('cleanup') == 'confirmed' and receipt.get('commit') == commit and
            receipt.get('record_sha256') == hashlib.sha256(record.encode()).hexdigest(),
            'ordinary producer preparation receipt does not bind this source and record')
    for key, limit in (('run_id', 255), ('machine_id', 127), ('boot_id', 127)):
        value = receipt.get(key)
        require(isinstance(value, str) and len(value) <= limit and
                re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._:/-]*', value),
                'invalid ordinary producer identity')
    require(receipt_reading(receipt.get('start', {})) <= receipt_reading(receipt.get('end', {})),
            'producer interval is invalid')


def clean_goals(receipt, roles, conditioning):
    """A clean import cannot replace portable production with retained bytes."""
    if conditioning != 'clean' or 'portable' not in roles.values():
        return
    profile = receipt.get('conditioning', {})
    require(profile.get('inhouse') == 'clean' and profile.get('backend') == 'ordinary' and
            profile.get('provenance') is None,
            'clean portable goals need a clean ordinary producer receipt')


def inventory_texts(tree, files):
    return {name: (tree / name).read_text() for name in files if role(name) == 'inventory'}


def inventory_path(item):
    if 'path_hex' in item:
        value = item['path_hex']
        require('path' not in item and isinstance(value, str) and value and
                len(value) % 2 == 0 and re.fullmatch('[0-9a-f]+', value),
                'invalid encoded inventory input')
        raw = bytes.fromhex(value)
        require(b'\0' not in raw, 'NUL in inventory input')
        return relative(os.fsdecode(raw))
    require(isinstance(item.get('path'), str), 'inventory input has no path')
    return relative(item['path'])


def nested_inputs(inventories, files, manifest_hash):
    """Follow the record-to-inventory-to-independent-fixture digest chain."""
    nested = {}
    for name, text in inventories.items():
        require(name in files and role(name) == 'inventory' and
                hashlib.sha256(text.encode()).hexdigest() == files[name]['sha256'] and
                len(text.encode()) == files[name]['size_bytes'], 'inventory anchor differs')
        inventory = json.loads(text)
        require(inventory['manifest_sha256'] == manifest_hash, 'producer inventory manifest differs')
        for check in inventory['checks']:
            if check['assertion_id'] != 'exfat':
                continue
            for item in check['inputs']:
                path = inventory_path(item)
                if path == 'build/tests/host/exfat_writer' or path.startswith('build/tests/host-suite/tools/exfat_external/'):
                    relative(path)
                    require(path not in nested or nested[path] == item['sha256'],
                            'independent fixture digests disagree')
                    nested[path] = item['sha256']
    return nested


def export(args):
    tree = filesystem_path(args.tree)
    require(sys.platform.startswith('linux'), 'the ordinary producer must be Linux')
    inputs = source(tree, args.commit)
    record = filesystem_path(args.record)
    text = record.read_text()
    files = record_files(text)
    require(files.keys() <= manifest_goals(tree) |
            {'build/rach2/kernel', 'build/x86_64/rach2/kernel'},
            'producer record contains undeclared artifacts')
    receipt = read_json(filesystem_path(args.receipt))
    receipt_check(receipt, args.commit, text)
    etude = filesystem_path(args.etude)
    validator = describe(etude)
    run([etude, 'check', 'record', '-d', record], tree)
    for name, value in files.items():
        verify(tree / name, value)
    auxiliary = {name: describe(tree / name) for name in AUXILIARY if (tree / name).exists()}
    inventories = inventory_texts(tree, files)
    nested_hashes = nested_inputs(inventories, files, digest(tree / 'tests/cases/host.host.json'))
    nested = {name: describe(tree / name) for name in nested_hashes}
    require(all(nested[name]['sha256'] == sha for name, sha in nested_hashes.items()),
            'independent fixture differs')
    data = dict(format=FORMAT, schema_version=1, commit=args.commit, source=inputs,
                record=text, receipt=receipt, validator=dict(path=native_path(etude), **validator),
                auxiliary=auxiliary, nested=nested, inventories=inventories)
    require(source(tree, args.commit) == inputs and record.read_text() == text,
            'producer changed during export')
    verify(etude, validator)
    output = filesystem_path(args.output)
    write_json(output, data)
    print(json.dumps(dict(metadata=native_path(output), **describe(output))), flush=True)


def windows_host():
    return os.name == 'nt' or sys.platform in ('msys', 'cygwin')


def remote_prefix(producer):
    if producer['transport'] == 'ssh':
        return []
    require(producer['transport'] == 'wsl-ssh', 'unsupported producer transport')
    for key in ('distribution', 'user'):
        value = producer.get(key)
        require(isinstance(value, str) and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', value),
                'invalid WSL distribution or user')
    return ['env', 'MSYS_NO_PATHCONV=1', 'wsl.exe', '--distribution',
            producer['distribution'], '--user', producer['user'], '--exec']


def remote_command(producer, words):
    host = producer['host']
    require(re.fullmatch(r'[A-Za-z0-9_.@-]+', host) and not host.startswith('-'), 'invalid SSH host')
    return ['ssh', '-o', 'BatchMode=yes', host,
            shlex.join([*remote_prefix(producer), *words])]


def validate_producer(producer):
    require(producer['transport'] in ('local', 'ssh', 'wsl-ssh'), 'unsupported producer transport')
    for key in ('tree', 'metadata'):
        value = producer[key]
        require(isinstance(value, str) and not any(c in value for c in ('\0', '\n', '\r')),
                'invalid producer path')
        if producer['transport'] != 'local':
            require(value.startswith('/'), 'remote producer paths must be absolute')
        else:
            require(filesystem_path(value).is_absolute(), 'local producer paths must be absolute')
    if producer['transport'] != 'local':
        require(producer['etude'].startswith('/'), 'remote etude must be absolute')
        require(not windows_host(), 'Windows import requires local native copying from an idle UNC producer')
        remote_command(producer, [])


def get_metadata(producer, work):
    if producer['transport'] == 'local':
        return filesystem_path(producer['metadata'])
    path = work / 'producer.json'
    locker = [producer['etude'], 'lock', 'tree', producer['tree'], 'prepare-import',
              '/bin/cat', producer['metadata']]
    with path.open('xb') as stream:
        run(remote_command(producer, locker), work, stdout=stream)
    return path


def idle_local(producer, tree):
    root = canonical_path(producer['tree'])
    require(not root.samefile(tree), 'producer and native destination are the same filesystem object')
    require(not os.path.lexists(root / 'build/.run.lock'), 'local producer tree is active or uncertain')
    return root


def rsync_path(path):
    """Give MSYS rsync its actual POSIX path, including a UNC operand."""
    return cygpath('-au', native_path(path)) if windows_host() else str(path)


def transfer(producer, names, tree, work):
    listing = work / 'import-files.list'
    listing.write_text('\n'.join(names) + '\n', encoding='utf-8', errors='surrogateescape')
    args = ['rsync', '-a', '--sparse', '--protect-args', '--files-from=' + rsync_path(listing)]
    if producer['transport'] == 'local':
        root = idle_local(producer, tree)
        operand = rsync_path(root).rstrip('/') + '/'
    else:
        locker = [producer['etude'], 'lock', 'tree', producer['tree'], 'prepare-import', '/usr/bin/rsync']
        args += ['--compress', '--rsync-path=' + shlex.join([*remote_prefix(producer), *locker]),
                 '-e', 'ssh -o BatchMode=yes']
        operand = producer['host'] + ':' + producer['tree'].rstrip('/') + '/'
    run([*args, '--', operand, rsync_path(tree).rstrip('/') + '/'], tree)
    if producer['transport'] == 'local':
        idle_local(producer, tree)



def batches(base, goals, limit=28000):
    batch = list(base)
    require(len(subprocess.list2cmdline(batch)) < limit, 'frozen input list exceeds native argv limit')
    for goal in goals:
        if len(subprocess.list2cmdline([*batch, goal])) >= limit:
            require(len(batch) > len(base), 'a native goal exceeds argv limit')
            yield batch
            batch = list(base)
        batch.append(goal)
    if len(batch) > len(base):
        yield batch


def build_native(tree, roles, imported, default_arch='i686'):
    frozen = ['-o' + name for name in sorted(imported)]
    settings = ['HOSTCC=/ucrt64/bin/gcc', 'CC=/ucrt64/bin/gcc', 'ETUDECC=/ucrt64/bin/gcc',
                'HOST_TEST_PLATFORM=windows', 'OS=Windows_NT'] if windows_host() else []
    commands = []
    for kind, flags in [('native', ['-B', '-j2']), ('inventory', ['-j1'])]:
        for arch in ('i686', 'x86_64'):
            goals = sorted(name for name, value in roles.items() if value == kind and
                           ('x86_64' if name.startswith('build/x86_64/') else default_arch) == arch)
            base = ['make', '--no-print-directory', '-k', 'ARCH=' + arch, *flags, *settings, *frozen]
            for argv in batches(base, goals):
                run(argv, tree)
                commands.append(argv)
    return commands


def native_binary(path):
    platform = 'windows' if windows_host() else 'darwin' if sys.platform == 'darwin' else 'linux'
    with path.open('rb') as stream:
        head = stream.read(4)
    require(any(head.startswith(magic) for magic in NATIVE_MAGIC[platform]),
            'not a native executable: ' + str(path))


def validate_native(tree, roles):
    for name, kind in roles.items():
        if kind == 'native':
            native_binary(tree / name)
        if kind != 'inventory':
            continue
        inventory = read_json(tree / name)
        require(inventory['manifest_sha256'] == digest(tree / 'tests/cases/host.host.json'),
                'native inventory manifest differs')
        compiler = inventory['compiler']
        require(digest(filesystem_path(compiler['executable'])) == compiler['sha256'],
                'native compiler changed')
        for check in inventory['checks']:
            for item in check.get('compiler_inputs', []):
                require(digest(filesystem_path(item['path'])) == item['sha256'],
                        'native compiler external input changed')
            for item in check['inputs']:
                path = tree / inventory_path(item)
                require(digest(path) == item['sha256'], 'inventory input changed: ' + str(path))
            if check['backend'] == 'native':
                require(not check.get('unsupported'), 'unsupported native check: ' + check['assertion_id'])
                path = tree / relative(check['program'])
                require(digest(path) == check['sha256'], 'native program differs')
                native_binary(path)


def prepare(args):
    tree, work = filesystem_path(args.tree), filesystem_path(args.work)
    config_path = canonical_path(args.config)
    output = canonical_path(args.provenance, strict=False)
    require(not output.exists() and config_path.parent == output.parent and
            canonical_path(tree) in output.parents,
            'config and new provenance must be in the declared record output directory')
    config_bytes = config_path.read_bytes()
    config = json.loads(config_bytes)
    require(set(config) == {'format', 'schema_version', 'commit', 'producer'},
            'unknown or missing import configuration field')
    require(config.get('format') == 'etude-preparation-import' and config.get('schema_version') == 1,
            'not an import configuration')
    commit = config['commit']
    require(not args.commit or args.commit == commit, 'receipt commit differs from producer')
    inputs = source(tree, commit)
    goals = {relative(line) for line in filesystem_path(args.request).read_text().splitlines()}
    known = manifest_goals(tree) | {'build/rach2/kernel', 'build/x86_64/rach2/kernel'}
    require(goals and goals <= known, 'request contains undeclared preparation goals')
    roles = {name: role(name) for name in goals}
    producer = config['producer']
    required = {'transport', 'tree', 'metadata', 'sha256'}
    optional = set() if producer['transport'] == 'local' else {'host', 'etude'}
    if producer['transport'] == 'wsl-ssh':
        optional |= {'distribution', 'user'}
    require(required <= producer.keys() and not (producer.keys() - required - optional),
            'unknown or missing producer field')
    require(producer['transport'] in ('local', 'ssh', 'wsl-ssh'), 'unknown transport')
    require(producer['transport'] == 'local' or {'host', 'etude'} <= producer.keys(),
            'remote producer needs its host and immutable etude')
    validate_producer(producer)
    metadata_path = get_metadata(producer, work)
    require(digest(metadata_path) == producer['sha256'], 'producer metadata hash differs')
    metadata = read_json(metadata_path)
    require(metadata.get('format') == FORMAT and metadata.get('schema_version') == 1 and
            metadata['commit'] == commit and metadata['source'] == inputs,
            'producer source differs from native source')
    if producer['transport'] != 'local':
        result = subprocess.check_output(remote_command(producer, ['/usr/bin/sha256sum', producer['etude']]))
        require(result.split()[0].decode() == metadata['validator']['sha256'] and
                producer['etude'] == metadata['validator']['path'], 'producer validator changed')
    files = record_files(metadata['record'])
    require(goals <= files.keys(), 'producer record does not cover every selected goal')
    receipt_check(metadata['receipt'], commit, metadata['record'])
    clean_goals(metadata['receipt'], roles, args.conditioning)
    imported = {name: files[name] for name, kind in roles.items() if kind == 'portable'}
    for name, value in metadata['auxiliary'].items():
        require(name in AUXILIARY, 'undeclared auxiliary input')
        imported[name] = value
    nested_hashes = nested_inputs(metadata['inventories'], files,
                                 digest(tree / 'tests/cases/host.host.json'))
    require(set(nested_hashes) == set(metadata['nested']) and
            all(metadata['nested'][name]['sha256'] == sha for name, sha in nested_hashes.items()),
            'nested fixture metadata differs from its producer inventory')
    if not windows_host() and any(kind == 'inventory' for kind in roles.values()):
        imported.update(metadata['nested'])
    for name in imported:
        relative(name)
        require(name not in inputs, 'import would overwrite committed source: ' + name)
    transfer(producer, sorted(imported), tree, work)
    for name, value in imported.items():
        verify(tree / name, value)
    require(args.arches in ('i686', 'x86_64', 'i686,x86_64'), 'invalid native build architectures')
    default_arch = 'x86_64' if args.arches == 'x86_64' else 'i686'
    commands = build_native(tree, roles, imported, default_arch)
    validate_native(tree, roles)
    for name, value in imported.items():
        verify(tree / name, value)
    require(source(tree, commit) == inputs and config_path.read_bytes() == config_bytes and
            digest(metadata_path) == producer['sha256'], 'preparation inputs changed')
    write_json(output, dict(schema_version=1, format='etude-preparation-import-provenance',
                           commit=commit, producer_sha256=producer['sha256'],
                           ordinary_production=metadata['receipt'], imported=imported,
                           roles=roles, native_build=commands,
                           artifacts={name: describe(tree / name) for name in roles}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    p = commands.add_parser('export')
    for name in ('tree', 'commit', 'record', 'receipt', 'etude', 'output'):
        p.add_argument('--' + name, required=True)
    p = commands.add_parser('prepare')
    for name in ('tree', 'config', 'request', 'work', 'provenance', 'commit', 'arches'):
        p.add_argument('--' + name, required=True)
    p.add_argument('--conditioning', choices=('clean', 'retained'), default='retained')
    args = parser.parse_args()
    try:
        globals()[args.action](args)
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print('prepare import: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
