#!/usr/bin/env python3
"""Preserve evidence-transport regressions until their native caller is replaced."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

CI = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CI))
sys.path.insert(0, str(CI.parent / 'etude'))


class Retention(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix='rach2-retention.')
        self.addCleanup(self.scratch.cleanup)
        self.base = Path(self.scratch.name).resolve()

    def test_transported_routine_proofs(self):
        from native_evidence import stage, restore
        source = self.base / 'routine-source'
        source.mkdir()
        leaf = self.base / 'proof bytes'
        leaf.write_bytes(b'actual proof bytes\n')
        ref = dict(sha256=hashlib.sha256(leaf.read_bytes()).hexdigest(),
                   size_bytes=leaf.stat().st_size, locations=[str(leaf)])
        grant = source / 'routine.grant.json'
        grant.write_text(json.dumps(dict(proof=ref)) + '\n')
        grant_ref = dict(sha256=hashlib.sha256(grant.read_bytes()).hexdigest(),
                         size_bytes=grant.stat().st_size, locations=[str(grant)])
        (source / 'routine.completion.json').write_text(json.dumps(dict(routine_grant=grant_ref)))
        output = self.base / 'routine-transport'
        stage(source, output)
        self.assertEqual((output / 'routine-proofs' / ref['sha256']).read_bytes(), leaf.read_bytes())
        self.assertFalse((output / 'routine-proof-errors.json').exists())
        restored = self.base / 'routine-restored'
        restore(output, restored)
        self.assertEqual((restored / 'routine.grant.json').read_bytes(), grant.read_bytes())
        self.assertEqual(json.loads(grant.read_text())['proof']['locations'], [str(leaf)])
        leaf.write_bytes(b'changed proof bytes\n')
        bad = self.base / 'routine-changed'
        stage(source, bad)
        self.assertTrue((bad / 'routine-proof-errors.json').is_file())
        self.assertFalse((bad / 'routine-proofs' / ref['sha256']).exists())
        leaf.unlink()
        missing = self.base / 'routine-missing'
        stage(source, missing)
        self.assertTrue((missing / 'routine-proof-errors.json').is_file())
        ref['locations'].append(str(self.base / 'second-location'))
        grant.write_text(json.dumps(dict(proof=ref)) + '\n')
        ambiguous = self.base / 'routine-ambiguous'
        stage(source, ambiguous)
        errors = json.loads((ambiguous / 'routine-proof-errors.json').read_text())
        self.assertTrue(any('ambiguous' in error for error in errors))

    def test_foreign_routine_locator_uses_exact_retained_proof(self):
        from native_evidence import stage
        source = self.base / 'foreign-routine'
        source.mkdir()
        proofs = source / 'routine-proofs'
        proofs.mkdir()
        payload = b'original Windows authority snapshot\n'
        sha = hashlib.sha256(payload).hexdigest()
        proof = proofs / sha
        proof.write_bytes(payload)
        ref = dict(sha256=sha, size_bytes=len(payload), locations=['D:/private/authority.snapshot'])
        grant = source / 'routine.grant.json'
        grant.write_text(json.dumps(dict(proof=ref)) + '\n')
        output = self.base / 'foreign-transport'
        stage(source, output)
        self.assertFalse((output / 'routine-proof-errors.json').exists())
        self.assertEqual((output / 'routine-proofs' / sha).read_bytes(), payload)
        self.assertEqual(json.loads((output / 'routine.grant.json').read_text())['proof'], ref)
        proof.write_bytes(b'changed')
        wrong = self.base / 'foreign-wrong'
        stage(source, wrong)
        self.assertTrue((wrong / 'routine-proof-errors.json').is_file())
        self.assertFalse((wrong / 'routine-proofs' / sha).exists())
        proof.unlink()
        missing = self.base / 'foreign-missing'
        stage(source, missing)
        self.assertTrue((missing / 'routine-proof-errors.json').is_file())
        proof.symlink_to(grant)
        symlinked = self.base / 'foreign-symlink'
        stage(source, symlinked)
        self.assertTrue((symlinked / 'routine-proof-errors.json').is_file())


if __name__ == '__main__':
    unittest.main()
