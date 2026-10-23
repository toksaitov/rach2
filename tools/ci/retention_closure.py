"""Resolve real retained blobs without relocating their absolute locators."""
import re

from retention_io import checked, checked_file, decode, document, require
from preparation_paths import canonical_path

SHA = re.compile(r'[0-9a-f]{64}\Z')


def references(value):
    if isinstance(value, dict):
        if {'sha256', 'size_bytes', 'locations'} <= value.keys():
            require(SHA.fullmatch(value['sha256']) and type(value['size_bytes']) is int and
                    value['size_bytes'] >= 0 and isinstance(value['locations'], list) and
                    value['locations'], 'malformed retained blob reference')
            yield value
        for child in value.values():
            yield from references(child)
    elif isinstance(value, list):
        for child in value:
            yield from references(child)


def json_lines(path):
    with path.open('rb') as stream:
        for line in stream:
            require(line.endswith(b'\n'), 'incomplete retained stream: ' + str(path))
            yield decode(line)


def documents(path):
    # Binary artifacts and text logs are not implicit manifests. JSON blobs,
    # including hash-named store objects, may contain further blob references.
    with path.open('rb') as stream:
        prefix = stream.read(256).lstrip()
    if not prefix.startswith((b'{', b'[')):
        return []
    if path.suffix == '.jsonl':
        return json_lines(path)
    try:
        return [document(path)]
    except (ValueError, UnicodeError) as error:
        raise ValueError('unreadable retained JSON: ' + str(path)) from error


def resolve(ref):
    failures = []
    for locator in ref['locations']:
        try:
            require(isinstance(locator, str) and '://' not in locator,
                    'unsupported retained locator')
            path = canonical_path(locator)
            actual = checked_file(path)
            require(all(actual[k] == ref[k] for k in ('sha256', 'size_bytes')),
                    'retained blob bytes differ')
            return path
        except (OSError, ValueError) as error:
            failures.append(str(error))
    raise ValueError('no retained blob location satisfies its digest: ' + '; '.join(failures))


def closure(roots):
    pending = [checked(ref) for ref in roots]
    result = {}
    while pending:
        path = pending.pop()
        name = str(path)
        if name in result:
            continue
        result[name] = checked_file(path)
        for value in documents(path):
            pending.extend(resolve(ref) for ref in references(value))
    return result


def complete_history(bundle):
    streams = sorted((bundle / 'history').glob('*.jsonl'))
    require(streams, 'bundle has no history streams')
    runs, plans, ids = set(), set(), set()
    for path in streams:
        sequence = {}
        for record in documents(path):
            require(record['record_id'] not in ids, 'duplicate retained record identity')
            ids.add(record['record_id'])
            stream = record['stream_id']
            require(record['sequence'] == sequence.get(stream, 0), 'retained stream sequence gap')
            sequence[stream] = record['sequence'] + 1
            if record['kind'] == 'plan':
                plans.add(record['run_id'])
            elif record['kind'] == 'run':
                require(record['run_id'] not in runs and
                        record['data']['result']['cleanup'] == 'confirmed' and
                        record['data']['result']['outcome'] == 'pass',
                        'failed, duplicate or uncertain terminal run')
                runs.add(record['run_id'])
    require(runs == plans and runs, 'history lacks a terminal run')
    return sorted(runs)


def cleanup_confirmed(bundle, run):
    require((bundle / 'sequence.status').read_text() == '0\n' and
            (bundle / 'failed').is_file() and (bundle / 'failed').stat().st_size == 0,
            'routine sequence or verdict did not pass')
    lines = (bundle / 'sequence.receipt').read_text().splitlines()
    require(lines[-5:] == ['error 0', 'cancelled 0', 'status 0', 'active 0', 'cleanup confirmed'],
            'sequence cleanup is unconfirmed')
    require([line for line in lines if line.startswith('run ')] == ['run ' + run],
            'sequence receipt has a different run identity')
    for phase in ('preparation', 'collection'):
        ended = [line.split() for line in lines if line.startswith('phase-end ' + run + ' ' + phase + ' ')]
        require(len(ended) == 1 and ended[0][4:8] in
                (['status', '0', 'cleanup', 'confirmed'], ['status', '0', 'active', '0']),
                'successful ' + phase + ' cleanup is absent')
