#!/usr/bin/env python3
"""Exercise the native fixed producer entry with small private Git sources."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class ProducerTests(unittest.TestCase):
    def fixture(self, name):
        base = Path(name)
        tree = base / 'repository'
        tree.mkdir()
        (tree / '.gitignore').write_text('build/\n')
        (tree / 'source').write_text('small source fixture\n')
        for args in (['init', '-q'], ['config', 'user.name', 'Etude fixture'],
                     ['config', 'user.email', 'fixture@example.invalid'], ['add', '.'],
                     ['commit', '-qm', 'Producer fixture']):
            subprocess.run(['git', '-C', str(tree), *args], check=True)
        commit = subprocess.check_output(['git', '-C', str(tree), 'rev-parse', 'HEAD'], text=True).strip()
        work = base / 'workspace'
        work.mkdir()
        subprocess.run(['git', '-C', str(tree), 'bundle', 'create', str(work / 'candidate.bundle'),
                        'HEAD'], check=True)
        (work / 'nonce').write_text('\n')
        site = base / 'site'
        site.write_text('etude-site 2\nendpoint linux\nledger /fixture\ncapacity 1,1,1\n')
        binary = Path(os.environ['ETUDE_PRODUCER_TEST_BINARY']).resolve()
        actor = base / 'actor'
        actor.write_text('#!' + os.environ.get('PYTHON', '/opt/homebrew/bin/python3') + '\n'
            'import json,os,sys\nfrom pathlib import Path\n'
            'a=sys.argv[1:]\n'
            'if a[:2]==["prepare","collection"]:\n'
            ' out=Path(a[a.index("-p")+1]).parent\n'
            ' (out/"prepare.record").write_text("fixture record\\n")\n'
            ' (out/"preparation.receipt.json").write_text("{}\\n")\n'
            ' (out/"actual.json").write_text(json.dumps(dict(args=a,clean=os.getenv("ETUDE_PREPARATION_CLEAN"),'
            ' run=os.getenv("ETUDE_PREPARATION_RUN_ID"),commit=os.getenv("ETUDE_PREPARATION_COMMIT"),'
            ' selected=os.getenv("ETUDE_PHASE_SELECTION_EXPORT"))))\n'
            'elif a[:1]==["status"]:\n'
            ' print(os.environ.get("ETUDE_PRODUCER_TEST_STATUS","etude status: empty"))\n'
            'elif a[:2]==["prepare","export"]:\n'
            ' Path(a[a.index("--output")+1]).write_text("{}\\n")\n'
            'else: os.execv(' + repr(str(binary)) + ',[' + repr(str(binary)) + ']+a)\n')
        actor.chmod(0o755)
        producer = dict(site=str(site), etude=str(actor), repository=str(tree),
                        tree_root=str(base / 'trees'), output_root=str(base / 'outputs'),
                        account='fixture', accelerator='fixture', preparation_resources='1,1,1',
                        policy=dict(ledger='/fixture', capacity='1,1,1'))
        config = dict(format='etude-routine-route', schema_version=1, mode='macos-import', producer=producer)
        ledger = base / 'ledger'
        ledger.mkdir()
        (ledger / 'claims').write_text('etude-ledger 1\n')
        producer['policy']['ledger'] = str(ledger)
        (work / 'config.json').write_text(json.dumps(config))
        return base, work, binary, producer, commit

    def run_operation(self, state, action='produce', run='sample-01', purpose='phase_first_run',
                      clean=True, expected=0, change=None, environment=None):
        base, work, binary, producer, commit = state
        args = [action, '--config', '${WORKSPACE}/config.json', '--commit', commit,
                '--run', run, '--purpose', purpose, '--arches', 'both', '--', 'all']
        operation = dict(action=action, producer=producer, mode='macos-import', run=run,
                         commit=commit, purpose=purpose, clean=clean, args=args,
                         transport_admitted=True, transport_result=str(base / 'transport.result'))
        if change:
            change(operation)
        (work / 'operation.json').write_text(json.dumps(operation))
        env = dict(os.environ, ETUDE_ADMITTED=producer['site'],
                   ETUDE_PHASE_SELECTION_EXPORT='/unrelated', ETUDE_PREPARATION_CLEAN='stale')
        env.update(environment or {})
        result = subprocess.run([str(binary), 'prepare', 'producer', str(work / 'operation.json')],
                                cwd=work, env=env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, expected, result.stdout)
        return result

    def test_fresh_producer_and_environment(self):
        with tempfile.TemporaryDirectory(prefix='rach2-native-producer.') as name:
            state = self.fixture(name)
            self.run_operation(state)
            actual = json.loads((state[0] / 'outputs/sample-01/actual.json').read_text())
            self.assertEqual(actual['clean'], '1')
            self.assertEqual(actual['run'], 'sample-01:producer')
            self.assertEqual(actual['commit'], state[-1])
            self.assertIsNone(actual['selected'])
            self.assertEqual(actual['args'][-1], 'all')
            self.assertTrue((state[1] / 'produced.json').is_file())
            self.run_operation(state, expected=1)
            self.assertFalse((Path(state[3]['repository']) / 'build/.run.lock').exists())

    def test_declared_condition_refusal(self):
        with tempfile.TemporaryDirectory(prefix='rach2-native-producer.') as name:
            state = self.fixture(name)
            for purpose, clean in (('ci', True), ('release', False), ('unknown', False)):
                self.run_operation(state, purpose=purpose, clean=clean, expected=1)
            self.run_operation(state, change=lambda o: o['args'].__setitem__(4, '0' * 40), expected=1)
            self.run_operation(state, change=lambda o: o.update(clean='true'), expected=1)
            self.assertFalse((state[0] / 'outputs').exists())

    def test_transport_lost_admission(self):
        with tempfile.TemporaryDirectory(prefix='rach2-native-producer.') as name:
            state = self.fixture(name)
            self.run_operation(state, change=lambda o: o.update(mode='windows-import'), expected=1)
            self.assertFalse((state[0] / 'outputs').exists())

    @unittest.skipUnless(sys.platform == 'darwin', 'Mac has no Linux listener boot proof')
    def test_current_boot_required(self):
        with tempfile.TemporaryDirectory(prefix='rach2-native-producer.') as name:
            state = self.fixture(name)
            path = state[1] / 'config.json'
            config = json.loads(path.read_text())
            config['accepted_jobs'] = dict(repository='fixture/project', listeners={})
            path.write_text(json.dumps(config))
            self.run_operation(state, action='facts', expected=1)
            self.assertFalse((state[1] / 'jobs-before.json').exists())
            self.assertFalse(list(state[1].glob('jobs-before.api.*')))

    def test_transport_cleanup_requires_ended_claim(self):
        for status in ('claim sample-01:producer:prepare',
                       'request sample-01:producer:prepare:child', 'etude status: empty'):
            with self.subTest(status=status), tempfile.TemporaryDirectory(prefix='rach2-native-producer.') as name:
                state = self.fixture(name)
                (state[0] / 'transport.result').write_text('exited 0\n')
                expected = 0 if status.endswith('empty') else 1
                self.run_operation(state, action='transport-cleanup', expected=expected,
                                   environment=dict(ETUDE_PRODUCER_TEST_STATUS=status))
                self.assertEqual((state[1] / 'producer.admit.result').exists(), expected == 0)
                self.assertEqual((state[1] / 'transport.status').read_text().strip(), status)

    def test_projection(self):
        with tempfile.TemporaryDirectory(prefix='rach2-native-producer.') as name:
            state = self.fixture(name)
            Path(state[3]['site']).write_text('etude-site 2\nendpoint blocked\nledger /fixture\ncapacity 1,1,1\n')
            self.run_operation(state, action='projection')
            value = (state[1] / 'producer.site').read_text()
            self.assertEqual(value.count('endpoint linux\n'), 1)
            self.assertIn('ledger /fixture\n', value)
            self.run_operation(state, action='projection', expected=1)


if __name__ == '__main__':
    unittest.main()
