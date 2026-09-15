#!/bin/sh
# Run the load window and inbound cleanup against private command fixtures.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
exec python3 - "$repo" "$@" <<'PY'
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time

repo = pathlib.Path(sys.argv[1])
if sys.argv[2:] not in ([], ['--mutate']):
    sys.exit('usage: machine_measure_test.sh [--mutate]')
work = pathlib.Path(tempfile.mkdtemp(prefix='rach2-measure.', dir='/tmp'))
print('machine-measure-test: logs:', work, flush=True)
source = work / 'source'
source.mkdir()
for name in ('machine_net_in.sh', 'machine_load.sh', 'machine_load_session.sh'):
    shutil.copyfile(repo / 'tools/dev' / name, source / name)
saved = {p: p.read_bytes() for p in source.iterdir()}
binpath = work / 'bin'
binpath.mkdir()
peer = '''#!/usr/bin/env python3
import os, pathlib, signal, subprocess, sys, time
name = pathlib.Path(sys.argv[0]).name
case = pathlib.Path(os.environ['CASE'])
mode = os.environ['MODE']
if name == 'ssh':
    if mode == 'load':
        if sys.argv[-1].startswith('mktemp '):
            print(case / 'remote')
        else:
            sys.exit(subprocess.call(sys.argv[-1], shell=True))
    else:
        if sys.argv[-1].startswith('kill '):
            sys.exit(0)
        known = next(x.split('=', 1)[1] for x in sys.argv if x.startswith('UserKnownHostsFile='))
        pathlib.Path(known).write_text('dummy address and public key\\n')
        print('machine-net: receiver-pid: 1')
        print('machine-net: listening: port 27158', flush=True)
        if mode == 'interrupt':
            while True:
                time.sleep(1)
        print('machine-net: result: fixture transfer')
elif name == 'sender':
    (case / 'sent').touch()
    sys.exit(3 if mode == 'failure' else 0)
elif name == 'cp':
    assert sys.argv[1:3] == ['-r', '/src/rach2']
    pathlib.Path(sys.argv[3]).mkdir(parents=True)
elif name == 'cut':
    assert sys.argv[1:] == ['-d.', '-f1', '/proc/uptime']
    path = case / 'clock'
    lines = path.read_text().splitlines()
    print(lines[0])
    path.write_text('\\n'.join(lines[1:]) + '\\n')
elif name == 'make':
    assert sys.argv[1:3] == ['-f', 'guest.mk']
    if sys.argv[-1] != 'clean':
        path = case / 'made'
        n = int(path.read_text()) + 1 if path.exists() else 1
        path.write_text(str(n))
        if n == 2 and os.environ.get('FAIL_BUILD') == '1':
            sys.exit(7)
'''
for name in ('ssh', 'sender', 'cp', 'cut', 'make'):
    path = binpath / name
    path.write_text(peer)
    path.chmod(0o700)
key = work / 'dummy-key'
key.write_text('fixture; no credential\n')
checks = 0


def run(args, env, directory, interrupt=False):
    log = directory / 'output'
    with log.open('wb') as out:
        child = subprocess.Popen(args, env=env, stdout=out, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        try:
            if interrupt:
                deadline = time.monotonic() + 8
                while not (directory / 'sent').exists():
                    if child.poll() is not None or time.monotonic() >= deadline:
                        raise AssertionError('interruption did not reach the sender')
                    time.sleep(0.02)
                child.send_signal(signal.SIGTERM)
            status = child.wait(timeout=10)
        finally:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait()
    return status, log.read_text()


def cases(label):
    global checks
    for mode, owned in [('success', False), ('failure', False), ('interrupt', False),
                        ('success', True), ('interrupt', True)]:
        directory = work / (label + '-' + mode + ('-owned' if owned else ''))
        directory.mkdir()
        env = dict(os.environ, PATH=str(binpath) + ':' + os.environ['PATH'],
                   CASE=str(directory), MODE=mode)
        env.pop('RACH2_NET_KNOWN', None)
        if owned:
            env['RACH2_NET_KNOWN'] = str(directory / 'caller-known')
        status, output = run(['sh', str(source / 'machine_net_in.sh'), str(key),
                              '127.0.0.1', '/dummy/receiver', str(binpath / 'sender'),
                              '1', '4096', '0', str(directory)], env, directory,
                             mode == 'interrupt')
        assert (status == 0) == (mode == 'success'), output
        assert not list(directory.glob('net-in.*/known_hosts')), 'default known-hosts file remains'
        if owned:
            assert (directory / 'caller-known').read_text() == 'dummy address and public key\n', \
                'caller-owned known-hosts file was removed'
        assert list(directory.glob('net-in.*/receive.log')), 'measurement log was removed'
        checks += 1
    for finish, fail in [(3799, False), (3700, False), (3799, True)]:
        # The production remote path validator requires this prefix.
        directory = pathlib.Path(tempfile.mkdtemp(prefix='rach2-load.', dir='/tmp'))
        print('machine-measure-test: load fixture:', directory, flush=True)
        (directory / 'clock').write_text(f'100\n101\n103\n3699\n{finish}\n{finish + 1}\n')
        env = dict(os.environ, PATH=str(binpath) + ':' + os.environ['PATH'],
                   CASE=str(directory), MODE='load', FAIL_BUILD=str(int(fail)))
        status, output = run(['sh', str(source / 'machine_load.sh'), str(key),
                              '127.0.0.1', '1', '3600', str(directory)], env, directory)
        assert (status == 0) == (not fail), output
        expected = ('SESSION 1 builds 1 failed 1 before-deadline 1 late 0' if fail else
                    'SESSION 1 builds 2 failed 0 before-deadline 1 late 1')
        assert expected in output, 'late completion counted inside the window: ' + output
        logs = list(directory.glob('load-*-session1.log'))
        assert len(logs) == 1
        text = logs[0].read_text()
        assert 'start 100 deadline 3700 clock guest-tick-seconds' in text
        assert f'start 3699 finish {finish} status {7 if fail else 0}' in text
        checks += 1
    print(f'machine-measure-test: {label}: 8 checks passed', flush=True)


try:
    cases('fixed')
    if '--mutate' in sys.argv:
        mutations = [
            ('cleanup', 'machine_net_in.sh', '        rm -f "$known"', '        :',
             'default known-hosts file remains'),
            ('deadline', 'machine_load_session.sh', '[ "$finished" -lt "$end" ]',
             '[ "$began" -lt "$end" ]', 'late completion counted inside the window')]
        for label, name, before, after, diagnostic in mutations:
            path = source / name
            text = path.read_text()
            assert text.count(before) == 1
            path.write_text(text.replace(before, after))
            try:
                cases(label)
            except AssertionError as error:
                if diagnostic not in str(error):
                    raise
                print('machine-measure-test:', label, 'mutation detected', flush=True)
            else:
                raise AssertionError(label + ' mutation passed')
            finally:
                path.write_bytes(saved[path])
            cases('restored-' + label)
finally:
    for path, content in saved.items():
        path.write_bytes(content)
PY
