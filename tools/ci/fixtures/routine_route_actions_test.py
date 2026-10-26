#!/usr/bin/env python3
"""Exercise native route dispatch without a remote endpoint or authority."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class RouteActionTests(unittest.TestCase):
    def test_direct_setup(self):
        for change in (None, 'selection', 'out'):
            with self.subTest(change=change), tempfile.TemporaryDirectory(prefix='rach2-route-entry.') as name:
                root = Path(name)
                tree = root / 'tree'
                tree.mkdir()
                (tree / '.gitignore').write_text('build/\n')
                (tree / 'source').write_text('route source fixture\n')
                for args in (['init', '-q'], ['config', 'user.name', 'Etude fixture'],
                             ['config', 'user.email', 'fixture@example.invalid'],
                             ['add', '.'], ['commit', '-qm', 'Route fixture']):
                    subprocess.run(['git', '-C', str(tree), *args], check=True)
                commit = subprocess.check_output(['git', '-C', str(tree), 'rev-parse', 'HEAD'], text=True).strip()
                out = tree / 'build/result'
                freeze = out / 'routine-proofs-input/freeze'
                freeze.mkdir(parents=True)
                policy = freeze / 'policy-before'
                original = 'etude-site 2\nendpoint disabled\nledger /fixture\n'
                policy.write_text(original)
                config = root / 'route.json'
                config.write_text(json.dumps(dict(format='etude-routine-route', schema_version=1,
                    mode='wsl-native', physical=dict(linux_output_root='/shared/retained'))))
                request = dict(purpose='ci', commit=commit, run='fixture', arches='both', accel='kvm',
                               selection=['all'], preparation=dict(clean=False, selection_export=None),
                               out='/shared/retained/fixture')
                if change == 'selection':
                    request['selection'] = ['host']
                elif change == 'out':
                    request['out'] = '/shared/retained-other/fixture'
                (out / 'routine.request.json').write_text(json.dumps(request))
                site = root / 'site'
                site.write_text('fixture site file\n')
                binary = Path(os.environ['ETUDE_ROUTE_TEST_BINARY']).resolve()
                argv = [str(binary), 'prepare', 'route', 'direct-setup', '--config', str(config),
                        '--tree', str(tree), '--out', str(out), '--run', 'fixture', '--commit', commit,
                        '--purpose', 'ci', '--arches', 'both', '--site', str(site), '--etude', str(binary),
                        '--accel', 'kvm', '--', 'all']
                env = dict(os.environ)
                env.pop('ETUDE_PHASE_SELECTION_EXPORT', None)
                result = subprocess.run(argv, env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True)
                self.assertEqual(result.returncode, 1 if change else 0, result.stdout)
                self.assertEqual(policy.read_text(), original)
                self.assertEqual((out / 'consumer.linux.site').read_text(),
                                 original.replace('endpoint disabled', 'endpoint linux'))
                self.assertEqual((out / 'routine-proofs').exists(), change is None)


if __name__ == '__main__':
    unittest.main()
