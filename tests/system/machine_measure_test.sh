#!/bin/sh
# Run the measurement drivers against private command fixtures: the check of
# the booted slot and kernel before timing, the load window, the second
# between load sessions, and inbound cleanup.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "${1:-}" != --texts ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" machine-measure-test "$repo/tests/system/machine_measure_test.sh" "$@"
fi
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
if sys.argv[2:] not in ([], ['--mutate'], ['--texts']):
    sys.exit('usage: machine_measure_test.sh [--mutate | --texts]')
work = pathlib.Path(tempfile.mkdtemp(prefix='rach2-measure.', dir='/tmp'))
print('machine-measure-test: logs:', work, flush=True)
source = work / 'source'
source.mkdir()
for name in ('machine_net_in.sh', 'machine_load.sh', 'machine_load_session.sh',
             'machine_identity.sh'):
    shutil.copyfile(repo / 'tools/dev' / name, source / name)
saved = {p: p.read_bytes() for p in source.iterdir()}

# The machine the identity check reads: a kernel in each slot of a boot
# partition, at the extents the fixture bootctl reports, and a second
# partition whose two slots hold the same kernel.
machine = work / 'machine'
machine.mkdir()
kernels = {'candidate': b'the candidate kernel\n' * 150, 'fallback': b'the fallback kernel\n' * 160}
first_sector = {'candidate': 8, 'fallback': 16}
for slots, image_name in (({'candidate': 'candidate', 'fallback': 'fallback'}, 'bootpart'),
                          ({'candidate': 'candidate', 'fallback': 'candidate'}, 'bootpart-same')):
    image = bytearray(32 * 512)
    for slot, kernel in slots.items():
        at = first_sector[slot] * 512
        image[at:at + len(kernels[kernel])] = kernels[kernel]
    (machine / image_name).write_bytes(bytes(image))
for kernel, data in kernels.items():
    (machine / (kernel + '.kernel')).write_bytes(data)

binpath = work / 'bin'
binpath.mkdir()
peer = '''#!/usr/bin/env python3
import hashlib, os, pathlib, subprocess, sys, time
name = pathlib.Path(sys.argv[0]).name
case = pathlib.Path(os.environ['CASE'])
mode = os.environ['MODE']
machine = pathlib.Path(os.environ['MACHINE'])


def record(line):
    with (case / 'commands').open('a') as log:
        log.write(line + '\\n')


def identity(command):
    """Run the identity check's remote half on this host's copy of the machine."""
    record('identity')
    for option in sys.argv:
        if option.startswith('UserKnownHostsFile=') and option != 'UserKnownHostsFile=/dev/null':
            pathlib.Path(option.split('=', 1)[1]).write_text('dummy address and public key\\n')
    slot = os.environ['MACHINE_SLOT']
    cmdline = case / 'cmdline'
    cmdline.write_text('rach2.root=ext2' + (' rach2.slot=' + slot if slot else '') + '\\n')
    image = machine / os.environ.get('MACHINE_IMAGE', 'bootpart')
    command = command.replace('/proc/cmdline', str(cmdline)).replace('/dev/bootpart', str(image))
    sys.exit(subprocess.call(['sh', '-c', command]))


if name == 'ssh':
    command = sys.argv[-1]
    if command.startswith(': machine-identity'):
        identity(command)
    if mode in ('load', 'stagger'):
        if command.startswith('mktemp '):
            record('mktemp')
            print(case / 'remote')
        elif mode == 'stagger':
            session = command.split()[3]
            record('session')
            with (case / 'order').open('a') as order:
                order.write('session ' + session + '\\n')
            print('SESSION ' + session + ' builds 0 failed 0 before-deadline 0 late 0')
        else:
            record('session')
            sys.exit(subprocess.call(command, shell=True))
    else:
        if command.startswith('kill '):
            sys.exit(0)
        record('receiver')
        known = next(x.split('=', 1)[1] for x in sys.argv if x.startswith('UserKnownHostsFile='))
        pathlib.Path(known).write_text('dummy address and public key\\n')
        print('machine-net: receiver-pid: 1')
        print('machine-net: listening: port 27158', flush=True)
        if mode == 'interrupt':
            while True:
                time.sleep(1)
        print('machine-net: result: fixture transfer')
elif name == 'bootctl':
    assert sys.argv[1:] == ['status']
    for slot in ('candidate', 'fallback'):
        start = 8 if slot == 'candidate' else 16
        print('bootctl: %s: first sector %d, 8 sectors in 1 clusters, 3000 bytes' % (slot, start))
    print('bootctl: grubenv: first sector 24, 2 sectors in 1 clusters, 1024 bytes')
    print('bootctl: saved_entry: fallback')
    print('bootctl: next_entry: ' + os.environ['MACHINE_NEXT'])
    slot = os.environ['MACHINE_SLOT']
    print('bootctl: booted: ' + (slot or 'not from a slot (no rach2.slot= on /proc/cmdline)'))
elif name == 'sha256sum':
    print(hashlib.sha256(sys.stdin.buffer.read()).hexdigest() + '  -')
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
elif name == 'sleep':
    order = case / 'order'
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        lines = order.read_text().splitlines() if order.exists() else []
        starts = sum(line.startswith('session ') for line in lines)
        if starts > sum(line.startswith('sleep ') for line in lines):
            break
        time.sleep(0.02)
    with order.open('a') as log:
        log.write('sleep ' + ' '.join(sys.argv[1:]) + '\\n')
'''
for name in ('ssh', 'sender', 'cp', 'cut', 'make', 'bootctl', 'sha256sum'):
    path = binpath / name
    path.write_text(peer)
    path.chmod(0o700)
# The stagger case alone puts this directory first, so the inbound driver's own
# pause and the other load cases keep the real sleep.
pacepath = work / 'pace'
pacepath.mkdir()
(pacepath / 'sleep').write_text(peer)
(pacepath / 'sleep').chmod(0o700)
key = work / 'dummy-key'
key.write_text('fixture; no credential\n')
checks = 0
CONSUMED = 'empty (a trial was consumed)'


def env_for(directory, mode, **extra):
    """The fixture's environment: a machine that booted the named kernel."""
    env = dict(os.environ, PATH=str(binpath) + ':' + os.environ['PATH'], CASE=str(directory),
               MODE=mode, MACHINE=str(machine), MACHINE_SLOT='candidate', MACHINE_NEXT=CONSUMED,
               RACH2_MEASURE_SLOT='candidate',
               RACH2_MEASURE_KERNEL=str(machine / 'candidate.kernel'))
    for name in ('RACH2_NET_KNOWN', 'RACH2_NET_USER', 'RACH2_LOAD_USER', 'RACH2_MEASURE_USER'):
        env.pop(name, None)
    env.update(extra)
    return env


def commands(directory):
    path = directory / 'commands'
    return path.read_text().split() if path.exists() else []


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


# Each case: the machine's state, the names the caller gives, the status the
# check must return, what its log or output must say, and the diagnostic when
# a refused machine is accepted.
IDENTITY = [
    ('match', {}, 0, 'machine-identity: result: the machine runs the named kernel: slot candidate',
     None),
    ('committed', {'MACHINE_NEXT': 'absent'}, 0, 'machine-identity: result:', None),
    ('fallback', {'MACHINE_SLOT': 'fallback', 'RACH2_MEASURE_SLOT': 'fallback',
                  'RACH2_MEASURE_KERNEL': str(machine / 'fallback.kernel')}, 0,
     'machine-identity: result: the machine runs the named kernel: slot fallback', None),
    ('other-slot', {'MACHINE_SLOT': 'fallback', 'MACHINE_IMAGE': 'bootpart-same'}, 1,
     'the machine booted fallback, not the named candidate',
     'a machine booted from another slot was measured'),
    ('no-slot', {'MACHINE_SLOT': ''}, 1, 'the machine booted no slot, not the named candidate',
     'a machine booted from no slot was measured'),
    ('armed', {'MACHINE_NEXT': 'candidate'}, 1, 'a trial is armed (next_entry candidate)',
     'a machine with an armed trial was measured'),
    ('other-kernel', {'RACH2_MEASURE_KERNEL': str(machine / 'fallback.kernel')}, 1,
     'the candidate slot holds sha256', 'a machine running another kernel was measured'),
    ('unnamed', {'RACH2_MEASURE_SLOT': ''}, 2, 'RACH2_MEASURE_SLOT must name the slot', None),
]


def identity_cases(label):
    """The check alone, against each state of the machine."""
    global checks
    for name, extra, expected, text, diagnostic in IDENTITY:
        directory = work / (label + '-identity-' + name)
        directory.mkdir()
        log = directory / 'identity.log'
        status, output = run(['sh', str(source / 'machine_identity.sh'), str(log), 'ssh',
                              '-o', 'BatchMode=yes', 'root@127.0.0.1'],
                             env_for(directory, 'identity', **extra), directory)
        recorded = output + (log.read_text() if log.exists() else '')
        if expected != 0 and status == 0:
            raise AssertionError((diagnostic or name) + ': ' + recorded)
        assert status == expected, name + ': status ' + str(status) + ': ' + recorded
        assert text in recorded, name + ': ' + recorded
        if expected == 0:
            assert 'machine-identity: named: slot ' in log.read_text(), name
            assert 'machine-identity: sha256: ' in log.read_text(), name
        checks += 1


def refusal_cases(label):
    """Both drivers stop before timing when the check refuses the machine."""
    global checks
    directory = work / (label + '-refused-net-in')
    directory.mkdir()
    status, output = run(['sh', str(source / 'machine_net_in.sh'), str(key), '127.0.0.1',
                          '/dummy/receiver', str(binpath / 'sender'), '1', '4096', '0',
                          str(directory)],
                         env_for(directory, 'success', MACHINE_SLOT='fallback'), directory)
    if (directory / 'sent').exists() or 'receiver' in commands(directory):
        raise AssertionError('the inbound driver timed a machine the identity check refused: ' +
                             output)
    assert status == 1, output
    assert commands(directory) == ['identity'], output
    assert not list(directory.glob('net-in.*/known_hosts')), 'default known-hosts file remains'
    logs = list(directory.glob('net-in.*/identity.log'))
    assert len(logs) == 1 and 'machine-identity: refused: ' in logs[0].read_text(), output
    checks += 1
    directory = pathlib.Path(tempfile.mkdtemp(prefix='rach2-load.', dir='/tmp'))
    print('machine-measure-test: refused load fixture:', directory, flush=True)
    status, output = run(['sh', str(source / 'machine_load.sh'), str(key), '127.0.0.1', '2',
                          '3600', str(directory)],
                         env_for(directory, 'load', MACHINE_SLOT='fallback'), directory)
    if 'mktemp' in commands(directory) or 'session' in commands(directory):
        raise AssertionError('the load driver started sessions on a machine the identity '
                             'check refused: ' + output)
    assert status == 1, output
    assert commands(directory) == ['identity'], output
    assert not list(directory.glob('load-*-session*.log')), output
    logs = list(directory.glob('load-*-identity.log'))
    assert len(logs) == 1 and 'machine-identity: refused: ' in logs[0].read_text(), output
    checks += 1


def inbound_cases(label):
    """Inbound cleanup of the default known-hosts file, on a machine the check accepts."""
    global checks
    for mode, owned in [('success', False), ('failure', False), ('interrupt', False),
                        ('success', True), ('interrupt', True)]:
        directory = work / (label + '-' + mode + ('-owned' if owned else ''))
        directory.mkdir()
        env = env_for(directory, mode)
        if owned:
            env['RACH2_NET_KNOWN'] = str(directory / 'caller-known')
        status, output = run(['sh', str(source / 'machine_net_in.sh'), str(key),
                              '127.0.0.1', '/dummy/receiver', str(binpath / 'sender'),
                              '1', '4096', '0', str(directory)], env, directory,
                             mode == 'interrupt')
        assert (status == 0) == (mode == 'success'), output
        assert commands(directory)[:1] == ['identity'], 'the check did not come first: ' + output
        assert not list(directory.glob('net-in.*/known_hosts')), 'default known-hosts file remains'
        if owned:
            assert (directory / 'caller-known').read_text() == 'dummy address and public key\n', \
                'caller-owned known-hosts file was removed'
        assert list(directory.glob('net-in.*/receive.log')), 'measurement log was removed'
        checks += 1


def load_cases(label):
    """The load window: late completions and failures counted apart."""
    global checks
    for finish, fail in [(3799, False), (3700, False), (3799, True)]:
        # The production remote path validator requires this prefix.
        directory = pathlib.Path(tempfile.mkdtemp(prefix='rach2-load.', dir='/tmp'))
        print('machine-measure-test: load fixture:', directory, flush=True)
        (directory / 'clock').write_text(f'100\n101\n103\n3699\n{finish}\n{finish + 1}\n')
        env = env_for(directory, 'load', FAIL_BUILD=str(int(fail)))
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
        assert commands(directory)[:1] == ['identity'], 'the check did not come first: ' + output
        checks += 1


def stagger_case(label):
    """Three sessions, with the driver's one-second pause between each start.

    Dropbear refuses a sixth unauthenticated connection from one address, so
    the driver pauses a second after starting each session. The fixture's
    sleep does not sleep: it records its argument once the session before it
    has started, so the record is the order of the driver's calls.
    """
    global checks
    directory = pathlib.Path(tempfile.mkdtemp(prefix='rach2-load.', dir='/tmp'))
    print('machine-measure-test: stagger fixture:', directory, flush=True)
    env = env_for(directory, 'stagger',
                  PATH=str(pacepath) + ':' + str(binpath) + ':' + os.environ['PATH'])
    status, output = run(['sh', str(source / 'machine_load.sh'), str(key),
                          '127.0.0.1', '3', '3600', str(directory)], env, directory)
    assert status == 0, output
    order = (directory / 'order').read_text().splitlines()
    starts = [i for i, line in enumerate(order) if line.startswith('session ')]
    assert len(starts) == 3, 'sessions started: ' + ', '.join(order)
    for this, following in zip(starts, starts[1:]):
        assert 'sleep 1' in order[this + 1:following], \
            'sessions started without a second between them: ' + ', '.join(order)
    checks += 1


def cases(label):
    global checks
    checks = 0
    identity_cases(label)
    refusal_cases(label)
    inbound_cases(label)
    load_cases(label)
    stagger_case(label)
    print(f'machine-measure-test: {label}: {checks} checks passed', flush=True)


# Each fault: its name, the driver, the text, its replacement, and the words
# of the row it must fail.
mutations = [
    ('cleanup', 'machine_net_in.sh', '        rm -f "$known"', '        :',
     'default known-hosts file remains'),
    ('deadline', 'machine_load_session.sh', '[ "$finished" -lt "$end" ]',
     '[ "$began" -lt "$end" ]', 'late completion counted inside the window'),
    ('stagger', 'machine_load.sh', '    sleep 1\n', '    :\n',
     'sessions started without a second between them'),
    ('slot', 'machine_identity.sh', '[ "$booted" = "$want" ]', '[ -n "$want" ]',
     'a machine booted from another slot was measured'),
    ('trial', 'machine_identity.sh', '[ "$trial" = none ]', '[ -n "$trial" ]',
     'a machine with an armed trial was measured'),
    ('kernel', 'machine_identity.sh', '[ "$have" = "$sum" ]', '[ -n "$sum" ]',
     'a machine running another kernel was measured'),
    ('inbound-unchecked', 'machine_net_in.sh', '"$measure_user@$address" || exit\n',
     '"$measure_user@$address" || :\n',
     'the inbound driver timed a machine the identity check refused'),
    ('load-unchecked', 'machine_load.sh', '"$MEASURE_USER@$ADDR" || exit\n',
     '"$MEASURE_USER@$ADDR" || :\n',
     'the load driver started sessions on a machine the identity check refused')]
if '--texts' in sys.argv:
    for label, name, before, after, diagnostic in mutations:
        assert (source / name).read_text().count(before) == 1, label
    shutil.rmtree(work)
    print('machine-measure-test: texts: every fault is once in its source', flush=True)
    sys.exit(0)
try:
    cases('fixed')
    if '--mutate' in sys.argv:
        for label, name, before, after, diagnostic in mutations:
            path = source / name
            text = path.read_text()
            assert text.count(before) == 1, label
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
        print('machine-measure-test: result: pass: each mutation was detected and each '
              'restoration passed', flush=True)
finally:
    for path, content in saved.items():
        path.write_bytes(content)
PY
