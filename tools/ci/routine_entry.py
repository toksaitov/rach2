#!/usr/bin/env python3
"""Construct the fixed phase vectors for the existing native sequence owner."""
import json
import os
import stat
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'etude'))
from preparation_paths import filesystem_path, native_path
from routine_remote import require


def read_config(path):
    doc = json.loads(filesystem_path(path).read_text())
    require(doc.get('format') == 'etude-routine-route' and doc.get('schema_version') == 1,
            'invalid installed routine route')
    require(doc.get('mode') in ('wsl-native', 'windows-import', 'macos-import'), 'invalid route mode')
    if doc['mode'] == 'macos-import':
        user = getattr(os, 'geteuid', lambda: None)()
        require(user is not None, 'Mac route requires its native preparation platform')
        identity = filesystem_path(path).lstat()
        require(stat.S_ISREG(identity.st_mode) and identity.st_uid in (0, user) and
                not identity.st_mode & 0o022, 'Mac route descriptor is not protected')
    return doc


def wsl_native(path):
    value = subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()
    require(value and '\n' not in value, 'invalid native path round trip')
    reverse = subprocess.check_output(['wslpath', '-u', value], text=True).strip()
    require(os.path.samefile(path, reverse), 'Windows path round trip changed file identity')
    return value.replace('\\', '/')


def execute(program, argv):
    require(os.path.isabs(program) or (len(program) > 2 and program[1:3] == ':/'),
            'routine program is not absolute')
    os.execve(program, argv, os.environ)


def counted(program, args):
    return [program, str(len(args)), *args]


def main(argv):
    if len(argv) == 3 and argv[1] == 'mode':
        print(read_config(argv[2])['mode'])
        return
    require(len(argv) >= 10, 'expected the native sequence fixed operands and selection')
    purpose, tree, out, arches, run, site, etude, accel = argv[1:9]
    selection = argv[9:]
    route = os.environ.get('ETUDE_ROUTINE_ROUTE')
    require(route, 'selected site has no installed routine route')
    config = read_config(route)
    commit = subprocess.check_output(['git', '-C', tree, 'rev-parse', 'HEAD'], text=True).strip()
    require(len(commit) == 40 and all(c in '0123456789abcdef' for c in commit), 'invalid candidate')
    os.environ['ETUDE_ROUTINE_COMMIT'] = commit
    if config['mode'] == 'macos-import':
        # The normal admitted preparation invokes ensure before public prepare.
        os.environ['ETUDE_ROUTINE_ENSURE'] = route
        os.environ.pop('ETUDE_ROUTINE_ROUTE', None)
        execute('/bin/sh', ['/bin/sh', str(Path(tree) / 'tools/ci/native_sequence.sh'), *argv[1:]])
    physical = config['physical']
    windows = sys.platform in ('win32', 'msys', 'cygwin')
    if not windows:
        require(config['mode'] == 'wsl-native', 'physical route requires its native Windows entry')
        request = dict(run=run, commit=commit, tree=tree, out=out, site=site, etude=etude,
                       purpose=purpose, arches=arches, accel=accel, selection=selection)
        with (Path(out) / 'routine.request.json').open('x') as stream:
            json.dump(request, stream)
            stream.write('\n')
        native_tree, native_out, native_route = map(wsl_native, (tree, out, route))
        os.environ['ETUDE_ROUTINE_ROUTE'] = native_route
        os.environ['ETUDE_ROUTINE_COMPLETION_LOCATOR'] = out + '/routine.completion.json'
        # WSLENV passes only fixed configuration and the same current commit.
        names = ['ETUDE_ROUTINE_ROUTE', 'ETUDE_ROUTINE_COMMIT', 'ETUDE_ROUTINE_COMPLETION_LOCATOR',
                 'GITHUB_RUN_ID', 'RUNNER_NAME', 'ETUDE_ROUTINE_INPUT']
        os.environ['WSLENV'] = ':'.join(filter(None, [os.environ.get('WSLENV', ''), *names]))
        native_script = native_tree + '/tools/ci/routine_entry.py'
        child = [physical['etude'], 'authority', 'helper', physical['python'], native_script,
                 purpose, native_tree, native_out, arches, run, wsl_native(site),
                 physical['etude'], accel, *selection]
        execute(etude, [etude, 'authority', 'proxy', *child])
    require(native_path(filesystem_path(etude)) == physical['etude'], 'immutable physical tool differs')
    os.environ.pop('ETUDE_SEQUENCE_PARENT', None)
    os.environ['ETUDE_SEQUENCE_LOG'] = out + '/log'
    os.environ['ETUDE_SITE'] = site
    script = native_path(filesystem_path(tree) / 'tools/ci/routine_route.py')
    common = ['--config', native_path(filesystem_path(route)), '--tree', tree, '--out', out,
              '--run', run, '--commit', commit, '--purpose', purpose, '--arches', arches,
              '--site', site, '--etude', etude, '--accel', accel, '--', *selection]
    def helper(action):
        return [etude, 'authority', 'helper', physical['python'], script, action, *common]
    quiesce, restore = helper('quiesce'), helper('restore')
    trace, stop = out + '/outer-load.jsonl', out + '/outer-stop.request'
    if config['mode'] == 'windows-import':
        prepare = helper('prepare')
        collection = [etude, 'run', '-t', tree, '-a', arches, '-p', out + '/prepare.record',
                      '-o', out, '-e', out + '/failed-evidence', '-I', out + '/context/context.json',
                      '-g', commit, '-A', accel, *selection]
        collection_program = etude
    else:
        prepare = helper('direct-setup')
        request = json.loads((filesystem_path(out) / 'routine.request.json').read_text())
        require(request['run'] == run and request['commit'] == commit, 'direct request changed')
        lo = request['out']
        trace, stop = out + '/physical-load.jsonl', out + '/physical-stop.request'
        p = config['producer']
        environment = dict(p['environment'], ETUDE_AUTHORITY_INPUT='pipe',
            ETUDE_ROUTINE_DIRECT='1', ETUDE_ROUTINE_GRANT=lo + '/routine.grant.json',
            ETUDE_ROUTINE_PROOFS=lo + '/routine-proofs', ETUDE_ROUTINE_DIRECTORY=lo,
            ETUDE_ROUTINE_RUN=run, ETUDE_PHYSICAL_TRACE=lo + '/physical-load.jsonl',
            ETUDE_PHYSICAL_STOP_REQUEST=lo + '/physical-stop.request',
            ETUDE_ROUTINE_COMMIT=commit)
        # The capability is inherited through WSLENV, never printed on argv.
        os.environ['WSLENV'] = ':'.join(filter(None, [os.environ.get('WSLENV', ''),
            'ETUDE_AUTHORITY_NONCE', 'ETUDE_AUTHORITY_RUN']))
        inner = [request['etude'], 'authority', 'phase', run, commit, 'consumer',
                 lo + '/direct.owner.json', '/usr/bin/env', '-u', 'ETUDE_ROUTINE_ROUTE',
                 '-u', 'ETUDE_ROUTINE_INPUT', '-u', 'ETUDE_SEQUENCE_PARENT',
                 *[key + '=' + value for key, value in environment.items()], '/bin/sh',
                 request['tree'] + '/tools/ci/native_sequence.sh', purpose, request['tree'], lo,
                 arches, run, lo + '/consumer.linux.site', request['etude'], request['accel'], *selection]
        collection_program = physical['wsl']
        collection = [collection_program, '--distribution', p['distribution'], '--user', p['user'],
                      '--exec', *inner]
    command = [etude, 'observe', 'routine', out + '/sequence.receipt', run, trace, stop, etude,
               physical['preparation_path'], physical['runtime_path'], native_path(filesystem_path(route)),
               out, purpose, *counted(etude, quiesce), *counted(etude, prepare),
               *counted(collection_program, collection), *counted(etude, restore)]
    execute(etude, command)


if __name__ == '__main__':
    try:
        main(sys.argv)
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print('routine entry: ' + str(error), file=sys.stderr)
        raise SystemExit(2)
