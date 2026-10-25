"""Ordinary source production and endpoint handover for native routine entry.

Only preparation and post-run administration invoke this helper. Native C
owners supervise its local and remote children. It selects or judges no case.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'etude'))
from preparation_paths import filesystem_path, native_path
from preparation_import import source, describe, verify
from routine_remote import Remote, require
from routine_entry import phase_inputs, request_matches, validate_selection
from routine_pack import pack


def write(path, value):
    with Path(path).open('x', encoding='ascii') as stream:
        json.dump(value, stream, indent=2, ensure_ascii=True)
        stream.write('\n')


def ref(path):
    path = filesystem_path(path)
    return dict(**describe(path), locations=[native_path(path)])


def command(argv, **kwargs):
    # Capabilities are inherited or stored privately, never printed on argv.
    print('routine route: ' + shlex.join(map(str, argv)), flush=True)
    return subprocess.run(list(map(str, argv)), check=True, **kwargs)


class Route:
    def __init__(self, args):
        self.args = args
        self.preparation = phase_inputs(args.purpose)
        self.tree, self.out = filesystem_path(args.tree), filesystem_path(args.out)
        self.config_path = filesystem_path(args.config)
        self.config = json.loads(self.config_path.read_text())
        require(self.config.get('format') == 'etude-routine-route' and
                self.config.get('schema_version') == 1, 'invalid installed route')
        require(self.config['mode'] in ('windows-import', 'wsl-native', 'macos-import', 'native'),
                'unsupported routine route')
        require(source(self.tree, args.commit), 'candidate source is unavailable')
        self.work = self.out / 'routine-proofs-input'
        self.work.mkdir(mode=0o700, exist_ok=True)
        self.nonce = os.environ.get('ETUDE_AUTHORITY_NONCE', '')
        self.dynamic = dict(self.config, github_run=os.environ.get('GITHUB_RUN_ID'),
                            github_runner=os.environ.get('RUNNER_NAME'))

    def remote(self, action, bundle=None, label=None):
        label = label or action
        p, a = self.config['producer'], self.args
        admitted = action == 'produce' and self.config['mode'] == 'macos-import'
        transport = dict(p)
        grant = p['state_root'] + '/' + a.run + ':produce.grant.json'
        transport_result = p['state_root'] + '/' + a.run + ':produce.admit.result'
        if admitted:
            transport['_transport_admission'] = dict(grant=grant, result=transport_result,
                claim=a.run + ':producer:prepare',
                seconds=min({'push': 270, 'ci': 540, 'release': 13800, 'phase_first_run': 86400}[a.purpose],
                            int(p['policy']['seconds'])))
        remote = Remote(transport, a.run + ':' + label, self.work / (label + '.wire'))
        success = False
        try:
            remote.put('config.json', json.dumps(self.dynamic).encode())
            remote.put('nonce', (self.nonce + '\n').encode())
            for name in ('routine_producer.py', 'routine_jobs.py'):
                remote.put(name, (self.tree / 'tools/ci' / name).read_bytes())
            if bundle is not None:
                remote.put('candidate.bundle', Path(bundle).read_bytes())
            args = [action, '--config', '${WORKSPACE}/config.json', '--commit', a.commit,
                    '--run', a.run, '--purpose', a.purpose, '--arches', a.arches, '--', *a.selection]
            environment = dict(p['environment'])
            require(all(isinstance(k, str) and isinstance(v, str) for k, v in environment.items()),
                    'producer environment is not fixed strings')
            remote.put('operation.json', json.dumps(dict(action=action, args=args,
                       producer=p, mode=self.config['mode'], run=a.run, commit=a.commit,
                       purpose=a.purpose, clean=self.preparation['clean'], transport_admitted=admitted,
                       transport_result=transport_result)).encode())
            launcher = b'#!/bin/sh\nset -eu\nexec "$1" "$2" operation.json\n'
            remote.put('remote.sh', launcher, '0755')
            remote.put('routine_remote_stage.py', (self.tree / 'tools/ci/routine_remote_stage.py').read_bytes())
            environment['WORKSPACE'] = '${WORKSPACE}'
            if admitted:
                environment.update(ETUDE_ADMITTED=p['site'], ETUDE_SITE=p['site'],
                                   ETUDE_PREPARATION_GRANT=grant)
            token = remote.execute(action, 'remote.sh', [p['python'], '${WORKSPACE}/routine_remote_stage.py'],
                                   environment, self.work / (label + '.log'))
            names = [] if admitted else ['ledger-empty']
            if action in ('freeze', 'facts') or (action == 'produce' and self.config['mode'] == 'windows-import'):
                names += ['binding', 'state', 'policy-before', 'policy-disabled', 'facts.json']
            if action in ('freeze', 'facts'):
                names += ['jobs-before.json']
            if action == 'freeze':
                names += ['jobs-after.json']
            if action == 'transport-cleanup':
                names += ['producer.admit.result', 'transport.status']
            if action == 'produce':
                names += ['producer.json', 'prepare.record', 'preparation.receipt.json',
                          'produced.json']
                if not admitted:
                    names.append('producer.admit.result')
                if self.config['mode'] == 'windows-import':
                    names.append('producer.owner.json')
            result = self.work / label
            result.mkdir(mode=0o700)
            for name in names:
                (result / name).write_bytes(remote.get(token, name))
            remote.close()
            success = True
            if admitted:
                cleanup = self.remote('transport-cleanup', label='produce-cleanup')
                for name in ('producer.admit.result', 'ledger-empty', 'transport.status'):
                    (result / name).write_bytes((cleanup / name).read_bytes())
            return result
        finally:
            if not success:
                remote.abandon()

    def local(self, operation, *extra):
        command([self.args.etude, 'authority', operation, self.args.run, self.args.commit, *extra])

    def facts(self, name):
        directory = self.work / name
        directory.mkdir(mode=0o700)
        self.local('facts', native_path(directory))
        return directory

    def quiesce(self):
        require(self.config['mode'] in ('windows-import', 'wsl-native'), 'not a physical route')
        linux = self.remote('freeze')
        self.local('freeze')
        windows = self.facts('windows-grant')
        # Re-read accepted jobs after BOTH policies are disabled. Any newly
        # accepted ordinary job prevents the grant rather than being ignored.
        checked = self.remote('facts', label='facts-grant')
        self.local('physical', native_path(self.out / 'physical-grant.json'))
        facts = json.loads((windows / 'facts.json').read_text())
        endpoints = []
        for name, directory in (('linux', linux), ('windows', windows)):
            require((directory / 'ledger-empty').read_bytes() == b'etude-ledger 1\n', 'endpoint is not empty')
            endpoints.append(dict(name=name, normal_disabled=True,
                policy_before=ref(directory / 'policy-before'), policy_disabled=ref(directory / 'policy-disabled'),
                ledger_empty=ref(directory / 'ledger-empty')))
        authority = dict(binding=ref(windows / 'binding'), state=ref(windows / 'state'),
                         snapshot=facts['snapshot'], gate=facts['gate'],
                         accepted_jobs=[ref(linux / 'jobs-before.json'),
                                        ref(linux / 'jobs-after.json'), ref(checked / 'jobs-before.json')])
        write(self.out / 'lease.inputs.json', dict(authority=authority, endpoints=endpoints))

    def clock(self, name):
        path = self.work / name
        command([self.args.etude, 'authority', 'clock', native_path(path)])
        return json.loads(path.read_text())

    def produce(self):
        before = self.clock('producer-start.json')
        bundle = self.work / 'candidate.bundle'
        command(['git', '-C', self.tree, 'bundle', 'create', bundle, 'HEAD'])
        head = subprocess.check_output(['git', 'bundle', 'list-heads', bundle, 'HEAD'], text=True).split()
        require(head == [self.args.commit, 'HEAD'], 'candidate changed during source transfer')
        produced = self.remote('produce', bundle)
        after = self.clock('producer-end.json')
        metadata = json.loads((produced / 'producer.json').read_text())
        actual = json.loads((produced / 'produced.json').read_text())
        require(metadata['commit'] == self.args.commit and metadata['receipt']['run_id'] == self.args.run + ':producer',
                'producer metadata does not bind this invocation')
        require(before['machine_id'] == after['machine_id'] and before['boot_id'] == after['boot_id'],
                'physical clock changed during production')
        if self.preparation['clean']:
            condition = metadata['receipt'].get('conditioning', {})
            require(condition.get('inhouse') == 'clean' and condition.get('backend') == 'ordinary' and
                    condition.get('provenance') is None, 'fresh producer did not perform clean ordinary preparation')
        if self.config['mode'] == 'windows-import':
            require((produced / 'ledger-empty').read_bytes() == b'etude-ledger 1\n', 'Linux cleanup is not empty')
            native_tree = self.config['producer']['unc_tree_root'].rstrip('/') + '/' + self.args.commit
            native_metadata = self.config['producer']['unc_output_root'].rstrip('/') + '/' + self.args.run + '/producer.json'
            importer = dict(transport='local', tree=native_tree, metadata=native_metadata,
                            sha256=hashlib.sha256((produced / 'producer.json').read_bytes()).hexdigest())
            upstream = dict(format='etude-routine-upstream', schema_version=1, run_id=self.args.run,
                commit=self.args.commit, route=ref(self.config_path), routine_grant=ref(self.out / 'routine.grant.json'),
                mode='produced', producer=dict(receipt=ref(produced / 'preparation.receipt.json'),
                    record=ref(produced / 'prepare.record'), metadata=ref(produced / 'producer.json'),
                    admission=ref(produced / 'producer.admit.result'), cleanup=ref(produced / 'producer.owner.json')),
                owned_interval=dict(machine_id=before['machine_id'], boot_id=before['boot_id'],
                                    start=before['reading'], end=after['reading']),
                result=dict(outcome='pass', cleanup='confirmed'))
            write(self.out / 'routine.upstream.json', upstream)
            raw = json.loads((produced / 'producer.owner.json').read_text())
            result = raw['result']
            require(result == dict(end=0, value=0, exit_end=0, exit_value=0,
                                   cleanup=1, output_error=0), 'producer cleanup failed')
            grant = json.loads((self.out / 'routine.grant.json').read_text())
            phase = dict(format='etude-routine-phase', schema_version=1,
                run_id=self.args.run, commit=self.args.commit, route=grant['route'],
                lease=grant['lease'], owner=grant['owner'], phase='producer',
                child=raw['process'], machine_id=raw['machine_id'], boot_id=raw['boot_id'],
                start=raw['start'], end=raw['end'], admission=ref(produced / 'producer.admit.result'),
                receipt=ref(produced / 'producer.owner.json'),
                result=dict(end='exited', value=0, exit_end='exited', exit_value=0,
                            cleanup='confirmed', output_error=0, active=0))
            write(self.out / 'routine.producer.json', phase)
            pack(self.out)
            self.local('permit', 'consumer')
        else:
            p = self.config['producer']
            importer = {key: p[key] for key in ('transport', 'host', 'etude')}
            if p['transport'] == 'wsl-ssh':
                importer.update(distribution=p['distribution'], user=p['user'])
            importer.update(tree=actual['tree'], metadata=actual['metadata'],
                            sha256=hashlib.sha256((produced / 'producer.json').read_bytes()).hexdigest())
        path = self.out / 'preparation-import.json'
        write(path, dict(format='etude-preparation-import', schema_version=1,
                         commit=self.args.commit, producer=importer))
        return path

    def prepare(self):
        if self.preparation['selection_export']:
            allocation = self.out / 'routine.selection.export.json'
            command([self.args.etude, 'run', '-N', '-J', native_path(allocation, strict=False),
                     '-t', self.args.tree, '-a', self.args.arches, '-A', self.args.accel,
                     *self.args.selection])
            validate_selection(self.preparation['selection_export']['path'], allocation)
        importer = self.produce()
        environment = dict(os.environ, ETUDE_PREPARATION_IMPORT=native_path(importer))
        environment.pop('ETUDE_PREPARATION_CLEAN', None)
        if self.preparation['clean']:
            environment['ETUDE_PREPARATION_CLEAN'] = '1'
        if self.config['mode'] == 'windows-import':
            environment.update(ETUDE_ROUTINE_GRANT=native_path(self.out / 'routine.grant.json'),
                               ETUDE_ROUTINE_UPSTREAM=native_path(self.out / 'routine.upstream.json'),
                               ETUDE_ROUTINE_PROOFS=native_path(self.out / 'routine-proofs'),
                               ETUDE_AUTHORITY_PHASE='consumer')
        site = filesystem_path(self.args.site)
        policy = dict(line.split(' ', 1) for line in site.read_text().splitlines())
        projection = self.out / 'consumer.site'
        lines = site.read_text().splitlines()
        projection.write_text('\n'.join('endpoint windows' if l.startswith('endpoint ') else l for l in lines) + '\n')
        if self.config['mode'] != 'windows-import':
            projection = site
        env_program = self.config['consumer']['env']
        shell = self.config['consumer']['shell']
        claim = self.args.run + ':prepare'
        bound = min({'push': 270, 'ci': 540, 'release': 13800,
                     'phase_first_run': 86400}[self.args.purpose], int(policy['seconds']))
        environment.update(ETUDE_SITE=native_path(projection),
                           ETUDE_PREPARATION_GRANT=native_path(self.out / 'preparation.grant.json'))
        self.clock('consumer-start.json')
        command([self.args.etude, 'admit', '-d', policy['locks'], '-r', native_path(self.out / 'preparation.admit.result'),
                 policy['ledger'], claim, self.config['consumer']['account'], 'shared',
                 self.config['consumer']['preparation_resources'], policy['capacity'], str(bound),
                 env_program, 'ETUDE_ADMITTED=' + native_path(projection), shell,
                 native_path(self.tree / 'tools/ci/native_prepare.sh'), self.args.purpose,
                 native_path(self.tree), native_path(self.out), self.args.arches, self.args.run,
                 native_path(projection), self.args.etude, self.args.accel, *self.args.selection], env=environment)

    def direct_setup(self):
        require(self.config['mode'] == 'wsl-native', 'not a direct WSL route')
        linux = self.work / 'freeze'
        policy = (linux / 'policy-before').read_text()
        require(policy.startswith('etude-site 2\n'), 'invalid original Linux policy')
        projection = self.out / 'consumer.linux.site'
        with projection.open('x') as stream:
            stream.write('\n'.join('endpoint linux' if line.startswith('endpoint ') else line
                                   for line in policy.splitlines()) + '\n')
        request = json.loads((self.out / 'routine.request.json').read_text())
        request_matches(request, self.args.purpose, self.args.commit, self.args.run,
                        self.args.arches, self.args.accel, self.args.selection, self.preparation)
        # The Windows and Linux spellings must designate the same existing
        # shared NTFS directory. An explicit round trip is part of setup.
        expected = self.config['physical']['linux_output_root'].rstrip('/')
        require(request['out'].startswith(expected + '/'), 'OUT is outside shared NTFS retention')
        pack(self.out)

    def restore(self):
        linux = self.remote('facts', label='facts-final')
        self.local('block')
        windows = self.facts('windows-final')
        for name, directory in (('linux', linux), ('windows', windows)):
            require((directory / 'ledger-empty').read_bytes() == b'etude-ledger 1\n', 'cleanup ledger is not empty')
            (self.out / (name + '-final.claims')).write_bytes((directory / 'ledger-empty').read_bytes())
        self.remote('restore')
        self.local('restore')
        # The C owner removes the shared physical block only after this Job,
        # the observer and every phase have all ended with confirmed cleanup.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('quiesce', 'prepare', 'direct-setup', 'restore', 'ensure'))
    for name in ('config', 'tree', 'out', 'run', 'commit', 'purpose', 'arches', 'site', 'etude', 'accel'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('selection', nargs='+')
    args = parser.parse_args()
    route = Route(args)
    if args.action == 'ensure':
        print(native_path(route.produce()))
    else:
        getattr(route, args.action.replace('-', '_'))()


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print('routine route: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
