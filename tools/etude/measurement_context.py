#!/usr/bin/env python3
"""Prepare immutable measurement snapshots. This tool never executes cases."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
from preparation_paths import filesystem_path, native_path, canonical_path

IDENTITY = re.compile(r'[A-Za-z0-9][A-Za-z0-9._:/-]{0,254}\Z')
SHA = re.compile(r'[0-9a-f]{64}\Z')
COMMIT = re.compile(r'[0-9a-f]{40}\Z')


def require(held, reason):
    if not held:
        raise ValueError(reason)


def digest(path):
    path = filesystem_path(path)
    before = path.stat()
    require(stat.S_ISREG(before.st_mode), f'not a regular input: {path}')
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    after = path.stat()
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns),
            f'input changed while hashing: {path}')
    return dict(sha256=h.hexdigest(), size_bytes=after.st_size)


def descriptor(path):
    path = canonical_path(path)
    return dict(path=native_path(path), **digest(path))


def data(path):
    return json.loads(filesystem_path(path).read_text())


def create(path, value):
    path = filesystem_path(path)
    raw = (json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(',', ':')) + '\n').encode()
    with path.open('xb') as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    return descriptor(path)


def git(tree, *args):
    return subprocess.check_output(['git', *args], cwd=tree)


def frozen(tree, commit):
    require(git(tree, 'rev-parse', 'HEAD').decode().strip() == commit, 'source HEAD differs')
    require(not git(tree, 'status', '--porcelain', '--untracked-files=all').strip(),
            'source inputs are modified or untracked')


def relative(path):
    p = Path(path)
    require(path and not p.is_absolute() and '\\' not in path and ':' not in path and
            all(part not in ('', '.', '..') for part in path.split('/')),
            f'not a relative source path: {path}')
    return p


def source(args):
    tree = canonical_path(args.tree)
    frozen(tree, args.commit)
    names = git(tree, 'ls-files', '-z').decode().split('\0')
    names = sorted(set(filter(None, names)) | set(args.extra))
    inputs = []
    for name in names:
        p = tree / relative(name)
        require(not p.is_symlink(), f'source symlink needs an explicit contract: {name}')
        inputs.append(dict(path=name, **digest(p)))
    frozen(tree, args.commit)
    snapshot = dict(schema_version=1, commit=args.commit, inputs=inputs,
                    scope='all tracked regular files and explicitly listed prepared source inputs')
    print(json.dumps(create(args.output, snapshot), sort_keys=True))


def verify_source(tree, snapshot, expected, commit):
    require(SHA.fullmatch(expected) and digest(snapshot)['sha256'] == expected,
            'source snapshot hash differs from its producer')
    doc = data(snapshot)
    require(doc.get('schema_version') == 1 and doc.get('commit') == commit,
            'source snapshot belongs to another commit')
    inputs = doc.get('inputs')
    require(isinstance(inputs, list) and inputs, 'source snapshot has no inputs')
    names = set()
    for entry in inputs:
        name = entry['path']
        require(name not in names, f'duplicate source input: {name}')
        names.add(name)
        require(digest(tree / relative(name)) == {key: entry[key] for key in ('sha256', 'size_bytes')},
                f'deployed source differs: {name}')
    return doc


def observation(trace, stop, run_id):
    if not trace:
        require(not stop, 'a stop request needs its observation trace')
        return None
    require(stop is not None and not stop.exists() and not stop.is_symlink(),
            'stop request already exists or is absent')
    path = canonical_path(trace)
    require(stat.S_ISREG(path.stat().st_mode), 'observation trace is not regular')
    with path.open('rb') as stream:
        line = stream.readline(65536)
    require(line.endswith(b'\n') and len(line) < 65536, 'observation header is incomplete')
    header = json.loads(line)
    require(header.get('schema_version') == 1 and header.get('kind') == 'start' and
            header.get('method') == 'native-load-v1' and header.get('run_id') == run_id,
            'observation header belongs to another run or method')
    return dict(trace=native_path(path), stop_request=native_path(stop, strict=False),
                header_sha256=hashlib.sha256(line).hexdigest())


def exemption(args, output):
    if args.purpose != 'phase_first_run':
        require(args.selection_export is None, 'selection exemption needs phase_first_run')
        return None
    require(args.selection_export is not None, 'phase_first_run needs allocator selection export')
    export = data(args.selection_export)
    raw = export['selection_bytes'].encode('ascii')
    require(len(raw) == export['selection_size_bytes'] and
            hashlib.sha256(raw).hexdigest() == export['selection_sha256'],
            'allocator selection bytes/hash disagree')
    json.loads(raw)
    path = output / 'selection.json'
    with path.open('xb') as stream:
        stream.write(raw)
    attempts = []
    for item in export['items']:
        require(item['architecture'] in ('i686', 'x86_64', 'none') and
                isinstance(item['repetition'], int) and item['repetition'] >= 0,
                'invalid allocated item')
        attempt = f"{args.run_id}:{item['case_id']}:{item['architecture']}:r{item['repetition']}:a0"
        require(IDENTITY.fullmatch(attempt), 'allocated attempt identity exceeds the history limit')
        attempts.append(attempt)
    require(attempts and len(set(attempts)) == len(attempts), 'empty or repeated allocated attempts')
    blob = descriptor(path)
    manifest = dict(sha256=blob['sha256'], size_bytes=blob['size_bytes'], locations=[blob['path']])
    return dict(reason='milestone-62-first-runs', phase_issue=589, manifest=manifest,
                run_ids=[args.run_id], attempt_ids=attempts)


def cache_context(args, receipt, output):
    names = ('artifacts', 'build', 'tools', 'os')
    states = {name: dict(state='unknown', procedure='No cache conditioning was recorded.')
              for name in names}
    proof = None
    path = getattr(args, 'cache_protocol', None)
    if path:
        doc = data(path)
        require(receipt is not None and doc.get('format') == 'etude-cache-protocol' and
                doc.get('schema_version') == 1, 'cache protocol needs native preparation')
        for name in ('run_id', 'commit', 'machine_id', 'boot_id'):
            require(doc.get(name) == receipt[name], 'cache protocol differs in ' + name)
        states = doc.get('states')
        require(isinstance(states, dict) and set(states) == set(names),
                'cache protocol must classify artifacts, build, tools and os')
        for value in states.values():
            require(isinstance(value, dict) and set(value) == {'state', 'procedure'} and
                    value['state'] in ('cold', 'warm', 'unknown') and
                    isinstance(value['procedure'], str) and value['procedure'].strip(),
                    'cache state needs a classification and procedure')
        evidence = doc.get('evidence')
        require(isinstance(evidence, list) and 0 < len(evidence) <= 16,
                'cache protocol needs bounded retained evidence')
        for ref in evidence:
            locations = ref.get('locations')
            require(isinstance(locations, list) and locations and
                    all(isinstance(p, str) and filesystem_path(p).is_absolute() for p in locations),
                    'cache evidence needs absolute locations')
            expected = {key: ref[key] for key in ('sha256', 'size_bytes')}
            found = False
            for location in locations:
                try:
                    found |= digest(location) == expected
                except OSError:
                    pass
            require(found, 'cache evidence bytes are missing or changed')
        proof = create(output / 'cache-evidence.json', doc)
    profile = dict(schema_version=1, states=states,
                   controlled=proof is not None and all(v['state'] != 'unknown' for v in states.values()),
                   method=args.cache_method, ordinary_preparation=args.ordinary_preparation,
                   one_time_production=args.one_time_production)
    return create(output / 'cache.json', profile), proof


def compose(args):
    tree = canonical_path(args.tree)
    require(IDENTITY.fullmatch(args.run_id),
            'run identity must be valid and fit the native allocator (at most 255 bytes)')
    source_doc = verify_source(tree, args.source_snapshot, args.source_sha256, args.commit)
    output = canonical_path(args.output, strict=False)
    output.mkdir(parents=True, exist_ok=False)
    snapshots = {}
    snapshots['source'] = create(output / 'source.json', source_doc)
    programs = sorted({canonical_path(args.etude), *(canonical_path(p) for p in args.tool)})
    snapshots['tools'] = create(output / 'tools.json', dict(schema_version=1,
                                inputs=[descriptor(p) for p in programs]))
    snapshots['site_profile'] = create(output / 'site.json', dict(schema_version=1,
                                            policy=descriptor(args.site)))
    record = descriptor(args.record)
    prep = receipt = None
    if args.preparation:
        receipt = data(args.preparation)
        require(receipt.get('format') == 'etude-preparation-receipt' and
                receipt.get('schema_version') == 1 and receipt.get('outcome') == 'pass' and
                receipt.get('cleanup') == 'confirmed' and receipt.get('run_id') == args.run_id and
                receipt.get('commit') == args.commit and receipt.get('record_sha256') == record['sha256'],
                'preparation receipt differs from this run/source/record')
        prep = descriptor(args.preparation)
    snapshots['cache_state'], cache_evidence = cache_context(args, receipt, output)
    context = dict(format='etude-measurement-context', schema_version=1,
                   run_id=args.run_id, commit=args.commit, purpose=args.purpose,
                   artifacts=record, preparation=prep, cache_evidence=cache_evidence, **snapshots)
    context['outer_observation'] = observation(args.outer_trace, args.outer_stop_request, args.run_id)
    context['physical_observation'] = observation(args.physical_trace, args.physical_stop_request, args.run_id)
    context['exemption'] = exemption(args, output)
    result = create(output / 'context.json', context)
    print(json.dumps(dict(context=result, run_id=args.run_id,
                          runtime=['etude', 'run', '-I', result['path'], '-g', args.commit]), sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    capture = commands.add_parser('source', help='capture all tracked inputs of a clean frozen source')
    capture.add_argument('--tree', type=filesystem_path, required=True)
    capture.add_argument('--commit', required=True)
    capture.add_argument('--output', type=filesystem_path, required=True)
    capture.add_argument('--extra', action='append', default=[], help='additional prepared source path under tree')
    make = commands.add_parser('compose', help='verify deployed inputs and compose context before execution')
    for flag in ('tree', 'record', 'site', 'etude', 'source-snapshot', 'output'):
        make.add_argument('--' + flag, type=filesystem_path, required=True)
    for flag in ('commit', 'run-id', 'source-sha256', 'cache-method', 'ordinary-preparation', 'one-time-production'):
        make.add_argument('--' + flag, required=True)
    make.add_argument('--purpose', choices=('focused', 'push', 'ci', 'release', 'phase_first_run'), required=True)
    make.add_argument('--tool', type=filesystem_path, action='append', default=[])
    make.add_argument('--cache-protocol', type=filesystem_path,
                      help='recorded cache classifications and hash-bound conditioning evidence')
    for flag in ('preparation', 'outer-trace', 'outer-stop-request', 'physical-trace',
                 'physical-stop-request', 'selection-export'):
        make.add_argument('--' + flag, type=filesystem_path)
    args = parser.parse_args()
    require(COMMIT.fullmatch(args.commit), 'commit must be 40 lowercase hexadecimal digits')
    if args.command == 'source':
        source(args)
    else:
        compose(args)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f'measurement context preparation failed: {error}', file=sys.stderr)
        sys.exit(1)
