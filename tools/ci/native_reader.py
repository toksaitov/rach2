#!/usr/bin/env python3
"""Verify the configured local native reader before executing it."""
import argparse
import hashlib
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'etude'))
from preparation_paths import filesystem_path, native_path


def verify(program, record):
    program = filesystem_path(program).resolve(strict=True)
    lines = filesystem_path(record).read_text().splitlines()
    if not lines or lines[0] != 'etude-prepare 1':
        raise ValueError('reader record is not a preparation record')
    roots = [line[5:] for line in lines if line.startswith('tree ')]
    if len(roots) != 1:
        raise ValueError('reader record needs exactly one tree')
    root = filesystem_path(roots[0])
    matches = []
    for line in lines:
        if not line.startswith('file '):
            continue
        fields = line.split(' ', 5)
        if len(fields) != 6:
            raise ValueError('malformed reader file record')
        if (root / filesystem_path(fields[5])).resolve(strict=True) == program:
            matches.append((fields[1], int(fields[2])))
    if len(matches) != 1:
        raise ValueError('local reader is not recorded exactly once')
    before = program.stat()
    digest = hashlib.sha256()
    with program.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    after = program.stat()
    if ((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) !=
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) or
            (digest.hexdigest(), after.st_size) != matches[0]):
        raise ValueError('local reader differs from its installed record')
    print(native_path(program))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('program')
    parser.add_argument('record')
    args = parser.parse_args()
    verify(args.program, args.record)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        print('ci: reader: ' + str(error), file=sys.stderr)
        sys.exit(2)
