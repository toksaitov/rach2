"""Execute fixed producer operations inside the native responder workspace."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def run(argv, **kwargs):
    return subprocess.run(list(map(str, argv)), check=True, **kwargs)


def main():
    operation = json.loads(Path(sys.argv[1]).read_text())
    p, action = operation['producer'], operation['action']
    nonce = Path('nonce').read_text().strip()
    if operation['mode'] in ('windows-import', 'wsl-native') and (
            len(nonce) != 64 or any(c not in '0123456789abcdef' for c in nonce)):
        raise ValueError('invalid phase capability')
    if nonce:
        os.environ['ETUDE_AUTHORITY_NONCE'] = nonce
    purpose, clean = operation['purpose'], operation['clean']
    if purpose not in ('push', 'ci', 'release', 'phase_first_run') or type(clean) is not bool or (
            purpose != 'phase_first_run' and clean != (purpose == 'release')):
        raise ValueError('invalid declared preparation condition')
    os.environ.pop('ETUDE_PHASE_SELECTION_EXPORT', None)
    os.environ.pop('ETUDE_PREPARATION_CLEAN', None)
    if clean:
        os.environ['ETUDE_PREPARATION_CLEAN'] = '1'
    work = Path.cwd()
    args = [str(work / 'routine_producer.py'),
            *(value.replace('${WORKSPACE}', str(work)) for value in operation['args'])]
    if action == 'transport-cleanup':
        result = Path(operation['transport_result']).read_bytes()
        if result != b'exited 0\n':
            raise ValueError('transport admission did not confirm cleanup')
        status = subprocess.check_output([p['etude'], 'status', p['policy']['ledger']])
        claim = operation['run'] + ':producer:prepare'
        if any(line.startswith((('claim ' + claim + ':').encode(),
                                ('request ' + claim + ':').encode())) for line in status.splitlines()):
            raise ValueError('transport claim remains in the actual ledger')
        Path('producer.admit.result').write_bytes(result)
        Path('transport.status').write_bytes(status)
        shutil.copyfile(Path(p['policy']['ledger']) / 'claims', 'ledger-empty')
        return
    if action != 'produce':
        run([p['python'], *args])
        if action == 'restore':
            shutil.copyfile(Path(p['policy']['ledger']) / 'claims', 'ledger-empty')
        return
    if operation.get('transport_admitted'):
        if os.environ.get('ETUDE_ADMITTED') != p['site']:
            raise ValueError('producer transport lost its original admission')
        os.environ['ETUDE_ROUTINE_TRANSPORT_ADMITTED'] = '1'
        run([p['etude'], 'lock', 'tree', p['repository'], 'routine-source', p['python'], *args])
        return
    run([p['python'], args[0], 'projection', *args[2:]])
    site = str(work / 'producer.site')
    policy = dict(line.split(' ', 1) for line in Path(site).read_text().splitlines())
    if policy['ledger'] != p['policy']['ledger'] or policy['capacity'] != p['policy']['capacity']:
        raise ValueError('installed producer authority differs from fixed route')
    bound = {'push': 270, 'ci': 540, 'release': 13800, 'phase_first_run': 86400}[operation['purpose']]
    os.environ['ETUDE_PREPARATION_GRANT'] = str(work / 'producer.grant.json')
    command = [p['etude'], 'admit', '-d', policy['locks'], '-r', str(work / 'producer.admit.result'),
               policy['ledger'], operation['run'] + ':producer:prepare', p['account'], 'shared',
               p['preparation_resources'], policy['capacity'], str(min(bound, int(policy['seconds']))),
               '/usr/bin/env', 'ETUDE_ADMITTED=' + site, 'ETUDE_SITE=' + site,
               p['etude'], 'lock', 'tree', p['repository'], 'routine-source', p['python'], *args]
    if operation['mode'] == 'windows-import':
        command = [p['etude'], 'authority', 'phase', operation['run'], operation['commit'],
                   'producer', str(work / 'producer.owner.json'), *command]
    run(command)
    if operation['mode'] == 'windows-import':
        run([p['etude'], 'authority', 'facts', operation['run'], operation['commit'], str(work)])
    else:
        shutil.copyfile(Path(policy['ledger']) / 'claims', 'ledger-empty')


if __name__ == '__main__':
    main()
