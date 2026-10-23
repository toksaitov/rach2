#!/usr/bin/env python3
"""Check declared collection costs against a hash-bound selected-site reference."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'etude'))
from preparation_paths import filesystem_path, native_path


def checked(ref):
    path = filesystem_path(ref['path']).resolve(strict=True)
    before = path.stat()
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    after = path.stat()
    if ((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) !=
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) or
            digest.hexdigest() != ref['sha256'] or after.st_size != ref['size_bytes']):
        raise ValueError('reference bytes changed: ' + str(path))
    return path


def forecast(args):
    descriptor = json.loads(args.reference.read_text())
    if descriptor.get('format') != 'etude-site-reference' or descriptor.get('schema_version') != 1:
        raise ValueError('not a version-one selected-site reference')
    policy = checked(descriptor['policy'])
    selected = json.loads(policy.read_text())['site']
    site = dict(line.split(' ', 1) for line in args.site.read_text().splitlines() if line)
    capacity = dict(zip(('cpu_millis', 'memory_bytes', 'disk_bytes'),
                        map(int, site['capacity'].split(','))))
    preparation = json.loads(args.preparation.read_text())
    if (preparation.get('format') != 'etude-preparation-receipt' or
            preparation.get('outcome') != 'pass' or preparation.get('cleanup') != 'confirmed'):
        raise ValueError('reference check needs successful native preparation')
    machine = preparation['machine_id']
    if (selected['capacity'] != capacity or selected['os'] != site['endpoint'] or
            selected['accelerator'] != args.accelerator or selected['machine_id'] != machine):
        raise ValueError('reference policy differs from the actual selected machine/site')
    histories = [checked(ref) for ref in descriptor['histories']]
    if not histories:
        raise ValueError('selected-site reference has no measured history')
    manifests = sorted((args.tree / 'tests/cases').glob('*.json'))
    if not manifests:
        raise ValueError('no case manifests')
    command = [native_path(args.etude), 'estimate', '-s',
               native_path(args.tree / 'tools/etude/schemas'), 'forecast', native_path(policy),
               'release', *map(native_path, manifests), '--', *map(native_path, histories)]
    with args.output.open('x') as log:
        status = subprocess.call(command, stdout=log, stderr=subprocess.STDOUT)
    with args.output.with_suffix('.receipt.json').open('x') as stream:
        json.dump(dict(format='etude-reference-check', schema_version=1,
                       reference=descriptor, command=command, status=status,
                       log=dict(path=native_path(args.output),
                                sha256=hashlib.sha256(args.output.read_bytes()).hexdigest(),
                                size_bytes=args.output.stat().st_size)), stream)
        stream.write('\n')
    checked(descriptor['policy'])
    for ref in descriptor['histories']:
        checked(ref)
    return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('tree', 'etude', 'reference', 'output', 'site', 'preparation'):
        parser.add_argument('--' + flag, type=filesystem_path, required=True)
    parser.add_argument('--accelerator', required=True)
    args = parser.parse_args()
    return forecast(args)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('ci: reference: ' + str(error), file=sys.stderr)
        sys.exit(2)
