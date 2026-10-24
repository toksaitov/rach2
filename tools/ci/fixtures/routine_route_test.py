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
