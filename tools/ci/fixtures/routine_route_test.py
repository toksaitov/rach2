#!/usr/bin/env python3
"""Exercise fixed route vectors and retained proof closure without a site."""
import hashlib
import json
import io
import os
from pathlib import Path
import sys
import subprocess
from contextlib import redirect_stdout
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import routine_entry
import routine_jobs
import routine_pack
import routine_remote


def reference(path, location=None):
    raw = path.read_bytes()
    return dict(sha256=hashlib.sha256(raw).hexdigest(), size_bytes=len(raw),
                locations=[str(location or path)])


class RouteTests(unittest.TestCase):
    def test_listener_state(self):
        self.assertIsNone(routine_jobs.listener_active('Running job: ci\nJob ci completed with result: Succeeded\n'))
        self.assertEqual(routine_jobs.listener_active('Running job: release\n'), 'release')
        for text in ('Running job: a\nRunning job: b\n',
                     'Running job: a\nJob b completed with result: Failed\n'):
            with self.assertRaises(ValueError):
                routine_jobs.listener_active(text)

    def test_listener_boot_reconciliation(self):
        boot = dict(boot_id='a' * 36, boot_unix_seconds=1792454400)
        old = '[2026-10-17 22:44:01Z INFO Terminal] Running job: core (i686)\n'
        old += '[2026-10-18 02:38:51Z INFO Terminal] Running job: bisect\n'
        current = '[2026-10-22 16:27:44Z INFO Terminal] Running job: ci\n'
        current += '[2026-10-22 16:28:44Z INFO Terminal] Job ci completed with result: Succeeded\n'
        state = routine_jobs.listener_boot_state(old + current, boot)
        self.assertIsNone(state['active'])
        self.assertEqual(len(state['superseded_by_boot']), 2)
        self.assertEqual(len(state['current_events']), 2)
        self.assertEqual(routine_jobs.listener_boot_state(current.splitlines()[0], boot)['active'], 'ci')
        for text in (old, 'Running job: unknown',
                     '[2026-10-22 16:27:44Z INFO Terminal] Running job:',
                     '[2026-10-22 16:27:44Z INFO Terminal] Job ci completed with result: Failed',
                     '[2026-99-22 16:27:44Z INFO Terminal] Running job: ci',
                     '[9999-10-22 16:27:44Z INFO Terminal] Running job: ci'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                routine_jobs.listener_boot_state(text, boot)

    def test_listener_boot_proof(self):
        identity = '12345678-1234-1234-1234-123456789abc\n'
        with patch.object(routine_jobs.Path, 'read_text', side_effect=[identity, 'btime 1792454400\n']):
            self.assertEqual(routine_jobs.boot_snapshot()['boot_id'], identity.strip())
        for values in (['unknown'], [identity, ''],
                       [identity, 'btime 1792454400\nbtime 1792454401\n'],
                       [identity, 'btime 9999999999999\n']):
            with self.subTest(values=values), \
                    patch.object(routine_jobs.Path, 'read_text', side_effect=values), \
                    self.assertRaises(ValueError):
                routine_jobs.boot_snapshot()

    def test_listener_drain_revalidation(self):
        boot = dict(boot_id='a', boot_unix_seconds=1792454400)
        server = dict(runners={'ci': dict(id=1, status='online', busy=False)},
                      workflows={}, jobs=[], coordinating_job=None)
        with tempfile.TemporaryDirectory(prefix='rach2-routine-') as name:
            path = Path(name) / 'Runner_20261022-162559-utc.log'
            path.write_text('[2026-10-22 16:27:44Z INFO Terminal] Listening for Jobs\n')
            config = dict(repository='toksaitov/rach2', listeners={'ci': name})
            with patch('routine_jobs.boot_snapshot', return_value=boot), \
                    patch('routine_jobs.server_snapshot', return_value=server):
                result = routine_jobs.drain_snapshot(config, None, None)
                self.assertEqual(result['boot'], boot)
                self.assertEqual(result['listeners']['ci']['files'][0]['sha256'], reference(path)['sha256'])
            with patch('routine_jobs.boot_snapshot', side_effect=[boot, dict(boot, boot_id='b')]), \
                    patch('routine_jobs.server_snapshot', return_value=server):
                with self.assertRaises(ValueError):
                    routine_jobs.drain_snapshot(config, None, None)
            changed = dict(server, runners={'ci': dict(id=1, status='online', busy=True)})
            with patch('routine_jobs.boot_snapshot', return_value=boot), \
                    patch('routine_jobs.server_snapshot', side_effect=[server, changed]):
                with self.assertRaises(ValueError):
                    routine_jobs.drain_snapshot(config, None, None)
            path.write_text('[2026-10-22 16:27:44Z INFO Terminal] Running job: unknown\n')
            with patch('routine_jobs.boot_snapshot', return_value=boot), \
                    patch('routine_jobs.server_snapshot', return_value=server):
                with self.assertRaises(ValueError):
                    routine_jobs.drain_snapshot(config, None, None)

    def test_current_workflow_and_job_proof(self):
        def pages(repository, suffix):
            if suffix == 'runners':
                return [dict(runners=[dict(name='ci', id=1, status='online', busy=True)])]
            if suffix.startswith('runs?status='):
                return [dict(workflow_runs=[dict(id=7, status='in_progress')]
                             if suffix.endswith('in_progress') else [])]
            return [dict(jobs=[dict(id=8, name='collection', runner_name='ci',
                                   runner_id=1, status='in_progress')])]
        with patch('routine_jobs.api', side_effect=pages):
            result = routine_jobs.server_snapshot('toksaitov/rach2', '7', 'ci')
            self.assertEqual(result['coordinating_job'], 'collection')
            with self.assertRaises(ValueError):
                routine_jobs.server_snapshot('toksaitov/rach2', None, None)
            with self.assertRaises(ValueError):
                routine_jobs.server_snapshot('toksaitov/rach2', '7', 'different-runner')
        def extra(repository, suffix):
            result = pages(repository, suffix)
            if suffix == 'runs/7/jobs':
                result[0]['jobs'].append(dict(id=9, name='another', runner_name='other',
                                             runner_id=2, status='queued'))
            return result
        with patch('routine_jobs.api', side_effect=extra), self.assertRaises(ValueError):
            routine_jobs.server_snapshot('toksaitov/rach2', '7', 'ci')

    def test_complete_late_proofs(self):
        with tempfile.TemporaryDirectory(prefix='rach2-routine-') as name:
            out = Path(name)
            raw = out / 'routine-proofs-input'
            raw.mkdir()
            retained = raw / 'policy-before'
            retained.write_bytes(b'etude-site 2\nendpoint linux\n')
            grant = out / 'routine.grant.json'
            grant.write_text(json.dumps(dict(policy=reference(retained, 'D:/original/policy'))))
            routine_pack.pack(out)
            trace = out / 'physical-load.jsonl'
            trace.write_bytes(b'{"kind":"end","complete":true}\n')
            phase = out / 'routine.collection.json'
            phase.write_text(json.dumps(dict(trace=reference(trace, 'D:/original/physical-load.jsonl'))))
            history = out / 'history'
            history.mkdir()
            stream = history / 'run.jsonl'
            stream.write_text(json.dumps(dict(kind='run', data=dict(load=reference(phase)))) + '\n')
            completion = out / 'routine.completion.json'
            completion.write_text(json.dumps(dict(grant=reference(grant), history=reference(stream),
                                                   phase=reference(phase))))
            routine_pack.pack(out)
            for path in (retained, grant, trace, phase, stream):
                self.assertEqual((out / 'routine-proofs' / reference(path)['sha256']).read_bytes(),
                                 path.read_bytes())
            trace.write_bytes(b'changed')
            target = out / 'routine-proofs' / reference(phase)['sha256']
            target.chmod(0o600)
            target.write_bytes(b'changed')
            with self.assertRaises(ValueError):
                routine_pack.pack(out)

    def test_ambiguous_and_missing_proof(self):
        with tempfile.TemporaryDirectory(prefix='rach2-routine-') as name:
            out = Path(name)
            grant = out / 'routine.grant.json'
            proof = dict(sha256='a' * 64, size_bytes=1, locations=['/missing/a'])
            grant.write_text(json.dumps(proof))
            with self.assertRaises(ValueError):
                routine_pack.pack(out)
            proof['locations'].append('/missing/b')
            grant.write_text(json.dumps(proof))
            with self.assertRaises(ValueError):
                routine_pack.pack(out)

    def test_failed_handshake_reaps_transport(self):
        class Child:
            stdin = io.BytesIO()
            stdout = io.BytesIO()
            waited = False
            def wait(self):
                self.waited = True
                return 1
        with tempfile.TemporaryDirectory(prefix='rach2-routine-') as name:
            child = Child()
            with patch('routine_remote.subprocess.Popen', return_value=child):
                with self.assertRaises(ValueError):
                    routine_remote.Remote(dict(transport='ssh', host='fixture', etude='/etude',
                                               state_root='/state'), 'run', Path(name) / 'wire')
            self.assertTrue(child.waited)
            self.assertTrue(child.stdin.closed)

    def test_mode_read_and_mac_bootstrap(self):
        with tempfile.TemporaryDirectory(prefix='rach2-routine-') as name:
            descriptor = Path(name) / 'route.json'
            descriptor.write_text(json.dumps(dict(format='etude-routine-route', schema_version=1,
                                                  mode='macos-import')))
            captured = io.StringIO()
            with redirect_stdout(captured), patch('routine_entry.subprocess.check_output',
                                                  side_effect=AssertionError('no work in mode read')):
                routine_entry.main(['entry', 'mode', str(descriptor)])
            self.assertEqual(captured.getvalue(), 'macos-import\n')
            if sys.platform == 'darwin':
                tree = Path(__file__).resolve().parents[3]
                env = dict(os.environ, tree=str(tree), ETUDE_ROUTINE_ROUTE=str(descriptor))
                result = subprocess.run(['/bin/sh', '-c',
                    'set -e; . "$tree/tools/ci/native_site.sh"; native_site_route /missing; '
                    'printf "%s\n" "$native_site_mode"'], env=env, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                self.assertEqual((result.returncode, result.stdout), (0, 'macos-import\n'), result.stderr)
            descriptor.chmod(0o666)
            with self.assertRaises(ValueError):
                routine_entry.read_config(descriptor)

    def test_transport_claim_precedes_responder(self):
        class Child:
            stdin = io.BytesIO()
            stdout = io.BytesIO()
            def wait(self):
                return 0
        p = dict(transport='ssh', host='fixture', etude='/etude', site='/site', state_root='/state',
                 account='fixture', preparation_resources='1,2,3',
                 policy=dict(locks='/locks', ledger='/ledger', capacity='4,5,6'),
                 _transport_admission=dict(grant='/grant', result='/result', claim='run:prepare', seconds=60))
        with tempfile.TemporaryDirectory(prefix='rach2-routine-') as name:
            with patch('routine_remote.subprocess.Popen', return_value=Child()) as start, \
                    patch.object(routine_remote.Remote, 'handshake'):
                remote = routine_remote.Remote(p, 'run', Path(name) / 'wire')
                command = start.call_args.args[0][-1]
                self.assertLess(command.index(' admit '), command.index(' serve '))
                self.assertIn('ETUDE_PREPARATION_GRANT=/grant', command)
                self.assertIn('ETUDE_ADMITTED=/site', command)
                self.assertIn(' admit -i ', command)
                self.assertIn(' authority transport /site /ledger /locks 4,5,6 60 ', command)
                remote.abandon()

    def test_counted_vectors(self):
        program = '/immutable/etude'
        for vector in ([program], [program, 'run', '-S', 'saved', 'all']):
            counted = routine_entry.counted(program, vector)
            self.assertEqual(counted[:2], [program, str(len(vector))])
            self.assertEqual(counted[2:], vector)


if __name__ == '__main__':
    unittest.main()
