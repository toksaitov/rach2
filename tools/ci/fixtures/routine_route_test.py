#!/usr/bin/env python3
"""Exercise fixed route vectors and retained proof closure without a site."""
import hashlib
import json
import io
import os
from pathlib import Path
import sys
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

    def test_counted_vectors(self):
        program = '/immutable/etude'
        for vector in ([program], [program, 'run', '-S', 'saved', 'all']):
            counted = routine_entry.counted(program, vector)
            self.assertEqual(counted[:2], [program, str(len(vector))])
            self.assertEqual(counted[2:], vector)


if __name__ == '__main__':
    unittest.main()
