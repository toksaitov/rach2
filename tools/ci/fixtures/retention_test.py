#!/usr/bin/env python3
"""Retention uses isolated owned bundles; it never visits real run evidence."""
import contextlib
import datetime
import io
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

CI = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CI))
sys.path.insert(0, str(CI.parent / 'etude'))
from retention import initialize
from retention_access import reading
from retention_io import checked_file, document, files, registry, write
from retention_registry import allocate, begin, reclaim, record_for, seal, stamp
from retention_closure import closure


class Retention(unittest.TestCase):
    def setUp(self):
        live = patch('retention_registry.live', return_value=None)
        live.start()
        self.addCleanup(live.stop)
        self.scratch = tempfile.TemporaryDirectory(prefix='rach2-retention.')
        self.addCleanup(self.scratch.cleanup)
        self.base = Path(self.scratch.name).resolve()
        self.site = self.base / 'site'
        self.site.write_text('etude-site 2\n')
        self.issues = self.base / 'issues.json'
        write(self.issues, dict(format='etude-retention-open-issues', schema_version=1,
                               complete=True, captured_utc=stamp(), issues=[], unknown=[]))
        self.policy = dict(format='etude-retention-policy', schema_version=1,
                           root=str(self.base / 'evidence'), site_sha256=checked_file(self.site)['sha256'],
                           max_bytes=1000000, max_age_days=1, reserve_bytes=10000,
                           issue_snapshot_max_age_seconds=3600, baselines=[], anchors=[],
                           open_issues=checked_file(self.issues))
        self.policy_path = self.base / 'policy.json'
        write(self.policy_path, self.policy)
        args = type('Args', (), dict(policy=self.policy_path, site=self.site))()
        with contextlib.redirect_stdout(io.StringIO()):
            initialize(args)
        self.root = Path(self.policy['root'])

    def bundle(self, payload=None, cleanup=True):
        bundle = allocate(self.policy, self.root)
        begin(self.root, bundle, bundle.name)
        (bundle / 'history').mkdir()
        (bundle / 'sequence.status').write_text('0\n')
        (bundle / 'failed').touch()
        records = [dict(record_id='p', stream_id='s', sequence=0, run_id=bundle.name,
                        kind='plan', data=payload or {}),
                   dict(record_id='r', stream_id='s', sequence=1, run_id=bundle.name,
                        kind='run', data=dict(result=dict(cleanup='confirmed', outcome='pass')))]
        (bundle / 'history/run.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in records))
        (bundle / 'sequence.receipt').write_text(
            'run ' + bundle.name + '\n' +
            'phase-end ' + bundle.name + ' preparation 1 status 0 cleanup confirmed cpu_us 1\n' +
            'phase-end ' + bundle.name + ' collection 2 status 0 cleanup confirmed cpu_us 1\n' +
            'error 0\ncancelled 0\nstatus 0\n' +
            ('active 0\ncleanup confirmed\n' if cleanup else 'cleanup unknown\n'))
        return bundle

    def old(self, bundle):
        path, item, bundle = record_for(self.root, bundle)
        old = time.time() - 3 * 86400
        item['sealed_utc'] = datetime.datetime.fromtimestamp(old, datetime.timezone.utc).isoformat()
        item['last_used'] = old
        write(path, item)
        for member in [*bundle.rglob('*'), bundle]:
            os.utime(member, (old, old))

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

    def test_prunes_only_sealed_unchanged_old_registered_bundle(self):
        ordinary = self.bundle()
        self.assertEqual(seal(self.policy, self.root, ordinary)['state'], 'sealed')
        active = allocate(self.policy, self.root)
        self.old(ordinary)
        unknown = self.root / 'runs/unknown'
        unknown.mkdir()
        (unknown / 'evidence').write_text('unknown')
        report = reclaim(self.policy, self.root, apply=False)
        self.assertTrue(ordinary.exists())
        self.assertEqual(report['would_remove'], [str(ordinary)])
        reclaim(self.policy, self.root, apply=True)
        self.assertFalse(ordinary.exists())
        self.assertTrue(active.exists() and unknown.exists())

    def test_changed_incomplete_and_uncertain_cleanup_are_protected(self):
        uncertain = self.bundle(cleanup=False)
        self.assertEqual(seal(self.policy, self.root, uncertain)['state'], 'protected')
        changed = self.bundle()
        seal(self.policy, self.root, changed)
        self.old(changed)
        (changed / 'new-proof').write_text('changed')
        partial = self.bundle()
        with (partial / 'history/run.jsonl').open('a') as stream:
            stream.write('{')
        self.assertEqual(seal(self.policy, self.root, partial)['state'], 'protected')
        reclaim(self.policy, self.root, apply=True)
        self.assertTrue(all(p.exists() for p in (uncertain, changed, partial)))

    def test_nested_missing_and_changed_blob_references_fail_closed(self):
        trace = self.base / 'trace'
        trace.write_text('trace')
        nested = self.base / 'nested.json'
        ref = dict(**{k: v for k, v in checked_file(trace).items() if k != 'path'}, locations=[str(trace)])
        write(nested, dict(trace=ref))
        self.assertEqual(len(closure([checked_file(nested)])), 2)
        trace.write_text('changed')
        with self.assertRaises(ValueError):
            closure([checked_file(nested)])
        trace.unlink()
        with self.assertRaises(ValueError):
            closure([checked_file(nested)])

    def test_baseline_and_open_issue_protect_full_proof_closure(self):
        sample = self.bundle()
        seal(self.policy, self.root, sample)
        self.old(sample)
        ref = checked_file(sample / 'history/run.jsonl')
        baseline = self.base / 'baseline.json'
        write(baseline, dict(format='etude-retention-protection', kind='baseline', roots=[ref]))
        self.policy['baselines'] = [checked_file(baseline)]
        self.assertIn(sample.name, reclaim(self.policy, self.root, apply=True)['protected'])
        self.policy['baselines'] = []
        snapshot = dict(number=991, state='open', body='Retain the full proof closure')
        issue = dict(number=991, snapshot=snapshot, snapshot_sha256=hashlib.sha256(
            json.dumps(snapshot, sort_keys=True).encode()).hexdigest(), evidence=[ref])
        write(self.issues, dict(format='etude-retention-open-issues', schema_version=1, complete=True,
                               captured_utc=stamp(), issues=[issue], unknown=[]))
        self.policy['open_issues'] = checked_file(self.issues)
        self.assertIn(sample.name, reclaim(self.policy, self.root, apply=True)['protected'])
        self.assertTrue(sample.exists())

    def test_stale_snapshot_protects_everything_but_space_allows_growth(self):
        sample = self.bundle()
        seal(self.policy, self.root, sample)
        self.old(sample)
        self.policy['open_issues']['sha256'] = '0'*64
        self.assertTrue(allocate(self.policy, self.root).exists())
        self.assertTrue(sample.exists())
        self.policy['max_bytes'] = 1
        with self.assertRaisesRegex(ValueError, 'exceeds byte limit'):
            allocate(self.policy, self.root)
        self.assertTrue(sample.exists())

    def test_one_day_floor_and_whole_root_byte_accounting(self):
        sample = self.bundle()
        seal(self.policy, self.root, sample)
        path, item, _ = record_for(self.root, sample)
        item['sealed_utc'] = '2000-01-01T00:00:00Z'
        write(path, item)
        self.assertFalse(reclaim(self.policy, self.root, apply=True)['removed'])
        large = self.root / 'unregistered-large-object'
        large.write_bytes(b'x'*1000000)
        with self.assertRaises(ValueError):
            allocate(self.policy, self.root)
        self.assertTrue(large.exists() and sample.exists())

    def test_ordinary_bundle_preserves_its_older_dependencies(self):
        sample = self.bundle()
        seal(self.policy, self.root, sample)
        ref = checked_file(sample / 'history/run.jsonl')
        blob = dict(sha256=ref['sha256'], size_bytes=ref['size_bytes'], locations=[ref['path']])
        newer = self.bundle(dict(previous=blob))
        seal(self.policy, self.root, newer)
        self.old(sample)
        reclaim(self.policy, self.root, apply=True)
        self.assertTrue(sample.exists() and newer.exists())

    def test_input_identity_is_not_an_external_blob_reference(self):
        bundle = self.bundle(dict(compiler=dict(path='/missing/compiler', sha256='a'*64, size_bytes=1)))
        self.assertEqual(seal(self.policy, self.root, bundle)['state'], 'sealed')

    def test_selected_reference_is_retained_immutably(self):
        sample = self.bundle()
        seal(self.policy, self.root, sample)
        self.old(sample)
        policy = self.base / 'budget.json'
        policy.write_text('{}')
        reference = self.base / 'reference.json'
        write(reference, dict(format='etude-site-reference', policy=checked_file(policy),
                              histories=[checked_file(sample / 'history/run.jsonl')]))
        allocate(self.policy, self.root, reference)
        reference.unlink()  # The accepted descriptor bytes were retained separately.
        self.assertIn(sample.name, reclaim(self.policy, self.root, apply=True)['protected'])
        self.assertEqual(len(list((self.root / 'accepted').iterdir())), 1)

    def test_parallel_allocators_serialize_registry_updates(self):
        command = [sys.executable, str(CI / 'retention.py'), '--policy', str(self.policy_path),
                   '--site', str(self.site), 'allocate']
        one = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        two = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        first, first_error = one.communicate()
        second, second_error = two.communicate()
        self.assertEqual((one.returncode, two.returncode), (0, 0), first_error + second_error)
        self.assertNotEqual(first, second)
        self.assertTrue(Path(first.strip()).is_dir() and Path(second.strip()).is_dir())

    def test_policy_site_mismatch_refuses_before_mutation(self):
        self.site.write_text('a different installed authority')
        with self.assertRaisesRegex(ValueError, 'different site'):
            with registry(self.policy_path, self.site):
                self.fail('different site was admitted')

    def test_native_windows_cleanup_receipt_shape_is_supported(self):
        bundle = self.bundle()
        path = bundle / 'sequence.receipt'
        path.write_text(path.read_text().replace('cleanup confirmed cpu_us', 'active 0 cpu_us'))
        self.assertEqual(seal(self.policy, self.root, bundle)['state'], 'sealed')

    def test_changed_live_issue_set_defers_pruning_without_blocking_spare_capacity(self):
        bundle = self.bundle()
        seal(self.policy, self.root, bundle)
        self.old(bundle)
        with patch('retention_registry.live', side_effect=ValueError('open issues changed')):
            fresh = allocate(self.policy, self.root)
        self.assertTrue(bundle.exists() and fresh.exists())

    def test_routine_reader_prevents_pruning_recently_used_bundle(self):
        bundle = self.bundle()
        seal(self.policy, self.root, bundle)
        self.old(bundle)
        with patch.dict(os.environ, ETUDE_RETENTION=str(self.policy_path), ETUDE_SITE=str(self.site)):
            with reading([bundle], bundle=True):
                self.assertTrue(bundle.exists())
        self.assertFalse(reclaim(self.policy, self.root, apply=True)['removed'])
        command = [sys.executable, str(CI / 'retention.py'), '--policy', str(self.policy_path),
                   '--site', str(self.site), 'touch', str(bundle)]
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)

    def test_registry_lock_released_on_normal_exit(self):
        with registry(self.policy_path, self.site):
            pass
        with registry(self.policy_path, self.site):
            pass


if __name__ == '__main__':
    unittest.main()
