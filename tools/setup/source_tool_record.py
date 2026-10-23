#!/usr/bin/env python3
"""Validate the source, recipe and executable identity of a native judge."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected(tool, prefix):
    script = ROOT / 'tools/setup/build_host_tool.sh'
    source = script.read_text()
    names = [tool.upper()]
    program = prefix / 'bin' / tool
    windows = platform.system().startswith(('Windows', 'MSYS', 'MINGW', 'CYGWIN', 'UCRT', 'CLANG'))
    if windows:
        program = program.with_suffix('.exe')
    files = [script, Path(__file__).resolve(), ROOT / 'mk/source-tools.mk',
             ROOT / 'tests/system/source_tools_test.py', ROOT / 'tests/system/source_tools_test.ps1']
    licenses = ['pmccabe.txt'] if tool == 'pmccabe' else ['mandoc.txt', 'zlib.txt']
    if tool == 'mandoc':
        names.append('ZLIB')
        if windows:
            names.append('STRPTIME')
            licenses.append('mandoc-strptime.c')
        files.extend([ROOT / 'tools/setup/mandoc-batch.mk',
                      *sorted((ROOT / 'tools/setup/mandoc-win').rglob('*.[ch]'))])
    pins = {name.lower(): re.search(r'^' + name + r'_SHA256=([0-9a-f]{64})$', source, re.M)[1]
            for name in names}
    recipe = hashlib.sha256()
    for path in files:
        recipe.update(path.relative_to(ROOT).as_posix().encode() + b'\0' + path.read_bytes() + b'\0')
    return dict(tool=tool, version={'pmccabe': '2.8', 'mandoc': '1.14.6'}[tool],
                sources=pins, recipe_sha256=recipe.hexdigest(), binary_sha256=digest(program),
                executable=program.name, native_windows=windows,
                checks_sha256=digest(prefix / (tool + '.check.json')),
                licenses={name: digest(prefix / 'share/licenses' / name) for name in licenses})


def identity(tool, prefix):
    found = json.loads((prefix / (tool + '.json')).read_text())
    if found != expected(tool, prefix):
        raise ValueError(f'{tool}: stale pinned tool; make host-{tool}')
    return {**found, 'path': str((prefix / 'bin' / found['executable']).resolve())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('check', 'write'))
    parser.add_argument('tool', choices=('pmccabe', 'mandoc'))
    parser.add_argument('prefix', type=Path)
    args = parser.parse_args()
    try:
        if args.mode == 'check':
            identity(args.tool, args.prefix)
        else:
            target = args.prefix / (args.tool + '.json')
            temporary = target.with_suffix('.json.tmp')
            temporary.write_text(json.dumps(expected(args.tool, args.prefix), indent=2) + '\n')
            os.replace(temporary, target)
        return 0
    except (OSError, ValueError, TypeError) as error:
        if args.mode == 'write':
            raise SystemExit(f'source tool record: {error}') from error
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
