#!/usr/bin/env python3
"""Run source checks during preparation and publish source-bound evidence."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/setup'))
from source_tool_record import identity  # noqa: E402


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def judges(case, prefix, arch):
    if case == 'sparse':
        program = shutil.which('clang')
        if not program:
            raise ValueError('clang is required for user-pointer checking')
        version = subprocess.check_output([program, '--version'], text=True).splitlines()[0]
        return dict(tool='clang', version=version, path=str(Path(program).resolve()),
                    binary_sha256=digest(Path(program)))
    pinned = {'sizes': ('pmccabe', '2.8'), 'manual': ('mandoc', '1.14.6')}
    if case not in pinned:
        return None
    tool, _ = pinned[case]
    try:
        found = identity(tool, prefix)
    except (OSError, ValueError) as error:
        raise ValueError(f'{tool} is missing or changed; make host-{tool}') from error
    if case == 'manual':
        path = Path('build/x86_64' if arch == 'x86_64' else 'build') / 'vendor/busybox/applet_tables-main.h'
        found['prepared_inputs'] = {path.as_posix(): digest(ROOT / path)}
    return found


def fingerprint(driver):
    result = subprocess.run([str(driver), '--fingerprint', str(ROOT)], check=True,
                            stdout=subprocess.PIPE, text=True).stdout.strip()
    if not re.fullmatch('[0-9a-f]{64}', result):
        raise ValueError('the source fingerprint is malformed')
    return result


def collect(definition, output):
    identity = definition['case_id']
    assertions, branches = [], []
    began, ended = False, False
    for line in output.splitlines():
        if not line.startswith('@etude '):
            continue
        words = line.split(' ')
        if ended or len(words) < 4 or words[:2] != ['@etude', '1']:
            raise ValueError('an invalid or late report record was written')
        verb = words[2]
        if verb == 'begin' and words[3:] == [identity] and not began:
            began = True
        elif verb == 'assert' and began and len(words) == 5 and words[4] == 'pass':
            assertions.append(words[3])
        elif verb == 'branch' and began and len(words) == 5:
            branches.append(dict(group=words[3], alternative=words[4]))
        elif verb == 'end' and began and words[3:] == [str(len(assertions) + len(branches))]:
            ended = True
        else:
            raise ValueError(f'a source check was not fully judged: {line}')
    expected = [a['assertion_id'] for a in definition['assertions']]
    if not ended or sorted(assertions) != sorted(expected):
        raise ValueError('the source check did not pass every declared assertion')
    wanted_groups = sorted(b['group_id'] for b in definition['branches'])
    if sorted(b['group'] for b in branches) != wanted_groups or any(
            b['alternative'] not in ('recorded', 'present', 'judged') for b in branches):
        raise ValueError('a required tool or recorded judge version was absent')
    return assertions, branches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case')
    parser.add_argument('output', type=Path)
    parser.add_argument('--arch', choices=('i686', 'x86_64'), default='i686')
    args = parser.parse_args()
    os.chdir(ROOT)
    driver = ROOT / 'build/tests/cases/host/style'
    manifest = json.loads((ROOT / 'tests/cases/host.style.json').read_text())
    definition = next(c for c in manifest['cases'] if c['case_id'] == 'host.style.' + args.case)
    architecture = 'none' if definition['architectures'] == 'none' else args.arch
    prefix = Path(os.environ.get('SOURCE_TOOL_PREFIX', ROOT / 'build/host-tools/native')).resolve()
    actual_judges = judges(args.case, prefix, args.arch)
    source_hash = fingerprint(driver)
    if args.output.is_file():
        old = json.loads(args.output.read_text())
        if old.get('version') == 1 and old.get('source_sha256') == source_hash and \
                old.get('architecture') == architecture and old.get('case_id') == definition['case_id'] and \
                old.get('actual_judges') == actual_judges:
            # The native reader also checks exact assertion and branch coverage.
            checked = subprocess.run([str(driver), args.case], env={**os.environ,
                                     'ETUDE_TREE': str(ROOT), 'ARCH': args.arch},
                                     stdout=subprocess.DEVNULL)
            if checked.returncode == 0:
                print(f'style preparation: {args.case}: unchanged evidence')
                return
    args.output.parent.mkdir(parents=True, exist_ok=True)
    log = args.output.with_suffix('.log')
    env = {**os.environ, 'ARCH': args.arch, 'ETUDE_TREE': str(ROOT),
           'ETUDE_CASE': definition['case_id'], 'PYTHONDONTWRITEBYTECODE': '1',
           'PATH': str(prefix / 'bin') + os.pathsep + os.environ.get('PATH', '')}
    result = subprocess.run(['sh', 'tests/cases/host/style.sh', args.case], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log.write_text(result.stdout)
    if result.returncode:
        raise ValueError(f'{args.case} exited {result.returncode}; evidence: {log}')
    assertions, branches = collect(definition, result.stdout)
    if fingerprint(driver) != source_hash:
        raise ValueError(f'sources changed during {args.case}; evidence: {log}')
    if judges(args.case, prefix, args.arch) != actual_judges:
        raise ValueError(f'the source-check tool changed during {args.case}; evidence: {log}')
    record = dict(version=1, case_id=definition['case_id'], architecture=architecture,
                  source_sha256=source_hash, assertions=assertions, branches=branches,
                  prepared_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  command=['sh', 'tests/cases/host/style.sh', args.case],
                  actual_judges=actual_judges,
                  judges=(ROOT / 'tools/setup/judges').read_text())
    fd, name = tempfile.mkstemp(dir=args.output.parent, prefix=args.output.name + '.')
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(record, stream, indent=2)
            stream.write('\n')
        os.replace(name, args.output)
    finally:
        if os.path.exists(name):
            os.unlink(name)
    print(f'style preparation: {args.case}: passed; evidence: {log}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'style preparation: {error}') from error
