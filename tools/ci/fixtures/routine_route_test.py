#!/usr/bin/env python3
"""Exercise fixed route vectors and retained proof closure without a site."""
import hashlib
import json
import io
import os
from pathlib import Path
import sys
import subprocess
import shlex
from contextlib import redirect_stdout
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import routine_entry
import routine_jobs
import routine_pack
import routine_remote
import routine_route
import routine_producer
import routine_remote_stage
from types import SimpleNamespace


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
            with patch('routine_jobs.boot_snapshot', return_value=boot), \
                    patch('routine_jobs.server_snapshot', return_value=changed):
                with self.assertRaises(ValueError):
                    routine_jobs.drain_snapshot(config, None, 'ci')
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
            descriptor.chmod(0o600)
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

    def allocation(self, path):
        raw = json.dumps(dict(schema_version=1, architecture='both', suites=[dict(suite='fixture',
            architecture='x86_64', serial=False, stops=False, cases=[dict(case_id='fixture.case',
            architecture='x86_64', repetitions=1)])], stop_on_fail=False)) + '\n'
        doc = dict(schema_version=1, selection_bytes=raw,
                   selection_size_bytes=len(raw), selection_sha256=hashlib.sha256(raw.encode()).hexdigest(),
                   items=[dict(case_id='fixture.case', architecture='x86_64', repetition=0)])
        path.write_text(json.dumps(doc))
        return doc

    def test_first_measurement_inputs(self):
        with tempfile.TemporaryDirectory(prefix='rach2-routine.') as name:
            path = Path(name) / 'selection.json'
            doc = self.allocation(path)
            with patch.dict(os.environ, dict(ETUDE_PHASE_SELECTION_EXPORT=str(path)), clear=True):
                retained = routine_entry.phase_inputs('phase_first_run')
                self.assertFalse(retained['clean'])
                self.assertEqual(retained['selection_export']['sha256'], reference(path)['sha256'])
                os.environ['ETUDE_PREPARATION_CLEAN'] = '1'
                self.assertTrue(routine_entry.phase_inputs('phase_first_run')['clean'])
                for purpose in ('push', 'ci', 'release'):
                    with self.subTest(purpose=purpose), self.assertRaises(ValueError):
                        routine_entry.phase_inputs(purpose)
                os.environ['ETUDE_PREPARATION_CLEAN'] = '0'
                with self.assertRaises(ValueError):
                    routine_entry.phase_inputs('phase_first_run')
                os.environ.pop('ETUDE_PREPARATION_CLEAN')
                doc['selection_sha256'] = '0' * 64
                path.write_text(json.dumps(doc))
                with self.assertRaisesRegex(ValueError, 'bytes/hash'):
                    routine_entry.phase_inputs('phase_first_run')
            with patch.dict(os.environ, {}, clear=True):
                with self.assertRaises(ValueError):
                    routine_entry.phase_inputs('phase_first_run')
                self.assertEqual(routine_entry.phase_inputs('ci'), dict(clean=False, selection_export=None))
                self.assertEqual(routine_entry.phase_inputs('release'), dict(clean=True, selection_export=None))
                os.environ['ETUDE_PHASE_SELECTION_EXPORT'] = ''
                with self.assertRaises(ValueError):
                    routine_entry.phase_inputs('ci')

    def test_current_allocator_must_match_preallocation(self):
        with tempfile.TemporaryDirectory(prefix='rach2-routine.') as name:
            expected, actual = (Path(name) / leaf for leaf in ('expected', 'actual'))
            doc = self.allocation(expected)
            actual.write_bytes(expected.read_bytes())
            routine_entry.validate_selection(expected, actual)
            doc['items'][0]['repetition'] = 1
            actual.write_text(json.dumps(doc))
            with self.assertRaisesRegex(ValueError, 'current public allocator'):
                routine_entry.validate_selection(expected, actual)

    def test_wsl_first_measurement_round_trip(self):
        with tempfile.TemporaryDirectory(prefix='rach2-routine.') as name:
            out = Path(name)
            allocated = out / 'allocated.json'
            self.allocation(allocated)
            route = out / 'route.json'
            route.write_text('{}')
            config = dict(mode='wsl-native', physical=dict(etude='/etude', python='/python',
                          wsl='/wsl', preparation_path='/prep', runtime_path='/runtime'),
                          producer=dict(distribution='Ubuntu', user='testing', environment={}))
            argv = ['entry', 'phase_first_run', '/tree', str(out), 'both', 'sample-01',
                    '/site', '/etude', 'kvm', 'all']
            environment = dict(ETUDE_ROUTINE_ROUTE=str(route), ETUDE_PHASE_SELECTION_EXPORT=str(allocated),
                               ETUDE_PREPARATION_CLEAN='1',
                               WSLENV='ETUDE_PHASE_SELECTION_EXPORT/p:ETUDE_PREPARATION_CLEAN/u:KEEP')
            with patch.dict(os.environ, environment, clear=True), \
                    patch.object(routine_entry, 'read_config', return_value=config), \
                    patch.object(routine_entry.subprocess, 'check_output', return_value='a' * 40), \
                    patch.object(routine_entry, 'wsl_native', side_effect=str), \
                    patch.object(routine_entry, 'native_path', side_effect=str), \
                    patch.object(routine_entry, 'execute', side_effect=StopIteration) as launch:
                with patch.object(routine_entry.sys, 'platform', 'linux'), self.assertRaises(StopIteration):
                    routine_entry.main(argv)
                self.assertIn('ETUDE_PHASE_SELECTION_EXPORT', os.environ['WSLENV'].split(':'))
                self.assertIn('ETUDE_PREPARATION_CLEAN', os.environ['WSLENV'].split(':'))
                self.assertEqual(os.environ['WSLENV'].split(':').count('ETUDE_PHASE_SELECTION_EXPORT'), 1)
                self.assertNotIn('ETUDE_PHASE_SELECTION_EXPORT/p', os.environ['WSLENV'].split(':'))
                self.assertNotIn('ETUDE_PREPARATION_CLEAN/u', os.environ['WSLENV'].split(':'))
                self.assertIn('KEEP', os.environ['WSLENV'].split(':'))
                request = json.loads((out / 'routine.request.json').read_text())
                self.assertEqual(request['selection_export_linux'], str(allocated))
                self.assertEqual(request['preparation']['selection_export']['sha256'], reference(allocated)['sha256'])
                with patch.object(routine_entry.sys, 'platform', 'win32'), self.assertRaises(StopIteration):
                    routine_entry.main(argv)
                command = launch.call_args.args[1]
                self.assertIn('ETUDE_PHASE_SELECTION_EXPORT=' + str(allocated), command)
                self.assertIn('ETUDE_PREPARATION_CLEAN=1', command)
                request['selection'] = ['host']
                (out / 'routine.request.json').write_text(json.dumps(request))
                with patch.object(routine_entry.sys, 'platform', 'win32'), self.assertRaisesRegex(ValueError, 'changed'):
                    routine_entry.main(argv)

    def test_first_measurement_consumer_preflight(self):
        with tempfile.TemporaryDirectory(prefix='rach2-routine.') as name:
            out = Path(name)
            allocated = out / 'allocated.json'
            self.allocation(allocated)
            site = out / 'site'
            site.write_text('etude-site 2\nendpoint windows\nlocks /locks\nledger /ledger\n'
                            'capacity 8000,20000000000,50000000000\nseconds 1234\n')
            route = object.__new__(routine_route.Route)
            route.args = SimpleNamespace(etude='/etude', tree='/tree', out=str(out), arches='both',
                run='sample-01', purpose='phase_first_run', accel='whpx', selection=['all'], site=str(site))
            route.tree, route.out = Path('/tree'), out
            route.preparation = dict(clean=True, selection_export=dict(path=str(allocated)))
            route.config = dict(mode='windows-import', consumer=dict(env='/env', shell='/shell',
                                account='fixture', preparation_resources='1000,2000,3000'))
            calls = []
            def command(argv, **kwargs):
                calls.append((argv, kwargs))
                if argv[1:4] == ['run', '-N', '-J']:
                    Path(argv[4]).write_bytes(allocated.read_bytes())
            def produce():
                self.assertEqual(len(calls), 1, 'public allocation precedes fresh production')
                return out / 'import.json'
            with patch.object(routine_route, 'command', side_effect=command), \
                    patch.object(routine_route, 'native_path', side_effect=lambda p, **kw: str(p)), \
                    patch.object(route, 'clock'), patch.object(route, 'produce', side_effect=produce):
                route.prepare()
            argv, options = calls[-1]
            self.assertEqual(argv[argv.index('shared') + 3], '1234')
            self.assertEqual(options['env']['ETUDE_PREPARATION_CLEAN'], '1')
            self.assertIn('phase_first_run', argv)
            self.assertIn('/tree/tools/ci/native_prepare.sh', argv)
            self.assertEqual(argv[-1], 'all')

            def wrong_allocation(argv, **kwargs):
                doc = json.loads(allocated.read_text())
                doc['items'][0]['case_id'] = 'fixture.other'
                Path(argv[4]).write_text(json.dumps(doc))
            with patch.object(routine_route, 'command', side_effect=wrong_allocation), \
                    patch.object(routine_route, 'native_path', side_effect=lambda p, **kw: str(p)), \
                    patch.object(route, 'produce', side_effect=AssertionError('production must not start')):
                with self.assertRaisesRegex(ValueError, 'current public allocator'):
                    route.prepare()

    def test_sequence_preserves_only_declared_first_clean(self):
        if sys.platform not in ('linux', 'darwin'):
            self.skipTest('native POSIX shell fixture')
        tree = Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory(prefix='rach2-routine.') as name:
            out = Path(name)
            for leaf in ('locks', 'ledger'):
                (out / leaf).mkdir()
            (out / 'ledger/claims').write_text('etude-ledger 1\n')
            tool = out / 'etude'
            tool.write_text('#!/bin/sh\nprintf "%s\\n" "${ETUDE_PREPARATION_CLEAN-unset}" "$@" > "$CAPTURE"\n')
            tool.chmod(0o755)
            site = out / 'site'
            endpoint = 'macos' if sys.platform == 'darwin' else 'linux'
            site.write_text(f'etude-site 2\nendpoint {endpoint}\nlauncher {tool}\netude {tool}\n'
                f'locks {out}/locks\nledger {out}/ledger\ncapacity 8000,20000000000,50000000000\n'
                'wait 30\nseconds 86400\n')
            allocated = out / 'allocated.json'
            self.allocation(allocated)
            for purpose, clean, expected in (('phase_first_run', None, 'unset'),
                    ('phase_first_run', '1', '1'), ('release', None, '1'), ('ci', '1', 'unset')):
                env = dict(os.environ, CAPTURE=str(out / 'captured'), JOBS='2')
                for key in ('ETUDE_ROUTINE_ROUTE', 'ETUDE_ADMITTED', 'ETUDE_PHASE_SELECTION_EXPORT',
                            'ETUDE_PREPARATION_CLEAN', 'ETUDE_REFERENCE', 'ETUDE_PREPARATION_IMPORT'):
                    env.pop(key, None)
                if purpose == 'phase_first_run':
                    env['ETUDE_PHASE_SELECTION_EXPORT'] = str(allocated)
                if clean is not None:
                    env['ETUDE_PREPARATION_CLEAN'] = clean
                result = subprocess.run(['/bin/sh', str(tree / 'tools/ci/native_sequence.sh'), purpose,
                    str(tree), str(out), 'both', 'sample', str(site), str(tool), 'tcg', 'all'],
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                captured = (out / 'captured').read_text().splitlines()
                self.assertEqual(captured[0], expected)
                self.assertEqual(captured[1:3], ['observe', 'sequence'])
                self.assertIn('sample:prepare', captured)

            for purpose, clean, export in (('ci', None, ''), ('phase_first_run', '0', str(allocated))):
                if (out / 'captured').exists():
                    (out / 'captured').unlink()
                env['ETUDE_PHASE_SELECTION_EXPORT'] = export
                env.pop('ETUDE_PREPARATION_CLEAN', None)
                if clean is not None:
                    env['ETUDE_PREPARATION_CLEAN'] = clean
                result = subprocess.run(['/bin/sh', str(tree / 'tools/ci/native_sequence.sh'), purpose,
                    str(tree), str(out), 'both', 'sample', str(site), str(tool), 'tcg', 'all'],
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertFalse((out / 'captured').exists())

    def test_native_preallocation_precedes_remote_production(self):
        if sys.platform not in ('linux', 'darwin'):
            self.skipTest('native POSIX shell fixture')
        tree = Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory(prefix='rach2-routine.') as name:
            base = Path(name)
            bins = base / 'bin'
            bins.mkdir()
            allocated, actual = base / 'allocated', base / 'actual'
            doc = self.allocation(allocated)
            tool = bins / 'etude'
            tool.write_text('#!/bin/sh\n[ "$1/$2/$3" = run/-N/-J ] || exit 91\n'
                            'printf "allocation\\n" >> "$CAPTURE"\ncp "$ACTUAL_EXPORT" "$4"\n')
            tool.chmod(0o755)
            for leaf in ('qemu-system-i386', 'qemu-system-x86_64'):
                (bins / leaf).symlink_to(tool)
            python = bins / 'python3'
            python.write_text('#!/bin/sh\ncase $1 in */routine_route.py) '
                'printf "ensure\\n" >> "$CAPTURE"; exit 37 ;; esac\nexec ' +
                shlex.quote(sys.executable) + ' "$@"\n')
            python.chmod(0o755)
            site = base / 'site'
            site.write_text('fixture policy\n')
            for matches in (True, False):
                out = base / ('valid' if matches else 'changed')
                out.mkdir()
                (out / 'outer-load.jsonl').write_text('{}\n')
                if not matches:
                    doc['items'][0]['case_id'] = 'fixture.other'
                actual.write_text(json.dumps(doc))
                capture = out / 'capture'
                env = dict(os.environ, PATH=str(bins) + os.pathsep + os.environ['PATH'],
                    ETUDE_ADMITTED=str(site), ETUDE_PHASE_SELECTION_EXPORT=str(allocated),
                    ETUDE_ROUTINE_ENSURE='/fixed/route', ACTUAL_EXPORT=str(actual), CAPTURE=str(capture))
                for key in ('ETUDE_PHYSICAL_TRACE', 'ETUDE_PHYSICAL_STOP_REQUEST',
                            'ETUDE_PREPARATION_IMPORT', 'ETUDE_PREPARATION_EXPORT'):
                    env.pop(key, None)
                result = subprocess.run(['/bin/sh', str(tree / 'tools/ci/native_prepare.sh'),
                    'phase_first_run', str(tree), str(out), 'both', 'sample', str(site), str(tool),
                    'tcg', 'all'], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                self.assertEqual(result.returncode, 37 if matches else 2, result.stderr)
                self.assertEqual(capture.read_text().splitlines(),
                                 ['allocation', 'ensure'] if matches else ['allocation'])
                self.assertFalse((out / 'prepare.record').exists())

    def test_native_producer_preserves_clean_declaration(self):
        if sys.platform not in ('linux', 'darwin'):
            self.skipTest('native POSIX shell fixture')
        tree = Path(__file__).resolve().parents[3]
        commit = subprocess.check_output(['git', '-C', str(tree), 'rev-parse', 'HEAD'], text=True).strip()
        with tempfile.TemporaryDirectory(prefix='rach2-routine.') as name:
            out = Path(name)
            tool = out / 'etude'
            tool.write_text('#!/bin/sh\nprintf "%s\\n" "${ETUDE_PREPARATION_CLEAN-unset}" '
                            '"$@" > "$CAPTURE"\nexit 37\n')
            tool.chmod(0o755)
            for purpose, clean, expected in (('phase_first_run', None, 'unset'),
                    ('phase_first_run', '1', '1'), ('release', None, '1'), ('ci', '1', 'unset')):
                env = dict(os.environ, CAPTURE=str(out / 'captured'), ETUDE_ADMITTED='/site',
                           ETUDE_ROUTINE_COMMIT=commit)
                env.pop('ETUDE_PHASE_SELECTION_EXPORT', None)
                env.pop('ETUDE_PREPARATION_CLEAN', None)
                if clean is not None:
                    env['ETUDE_PREPARATION_CLEAN'] = clean
                result = subprocess.run(['/bin/sh', str(tree / 'tools/ci/native_producer.sh'), purpose,
                    str(tree), str(out), 'both', 'sample:producer', '/site', str(tool), 'kvm', 'all'],
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                self.assertEqual(result.returncode, 37, result.stderr)
                captured = (out / 'captured').read_text().splitlines()
                self.assertEqual(captured[0], expected)
                self.assertEqual(captured[1:3], ['prepare', 'collection'])
                self.assertEqual(captured[-1], 'all')

    def test_fresh_producer_per_run(self):
        with tempfile.TemporaryDirectory(prefix='rach2-routine.') as name:
            base = Path(name)
            tree = base / 'trees' / ('a' * 40)
            tree.mkdir(parents=True)
            site = base / 'site'
            site.write_text('installed fixture policy\n')
            args = SimpleNamespace(commit='a' * 40, purpose='phase_first_run', arches='both',
                                   selection=['all'], run='sample-01')
            config = dict(producer=dict(tree_root=str(tree.parent), repository=str(tree),
                output_root=str(base / 'outputs'), site=str(site), etude='/etude', accelerator='kvm'))
            with patch.dict(os.environ, dict(ETUDE_ADMITTED=str(site), ETUDE_ROUTINE_TRANSPORT_ADMITTED='1'), clear=True), \
                    patch.object(routine_producer, 'execute') as execute, \
                    patch.object(routine_producer.subprocess, 'check_output', side_effect=lambda argv:
                        b'a' * 40 if argv[-1] == 'HEAD' else b''), \
                    patch.object(routine_producer.shutil, 'copyfile'), patch.object(routine_producer, 'write'):
                for run in ('sample-01', 'sample-02'):
                    args.run = run
                    routine_producer.stage(args, config)
                    command = execute.call_args.args[0]
                    self.assertEqual(command[2], 'phase_first_run')
                    self.assertEqual(command[4], str(base / 'outputs' / run))
                    self.assertEqual(command[6], run + ':producer')
                with self.assertRaises(FileExistsError):
                    routine_producer.stage(args, config)

    def test_remote_stage_declares_clean_without_exemption(self):
        with tempfile.TemporaryDirectory(prefix='rach2-routine.') as name:
            base = Path(name)
            (base / 'nonce').write_text('\n')
            operation = base / 'operation.json'
            producer = dict(site='/site', etude='/etude', repository='/repository', python='/python')
            original = Path.cwd()
            try:
                os.chdir(base)
                for purpose, clean in (('phase_first_run', True), ('phase_first_run', False),
                                       ('release', True), ('ci', False)):
                    operation.write_text(json.dumps(dict(action='produce', mode='macos-import',
                        purpose=purpose, clean=clean, producer=producer, transport_admitted=True,
                        args=['produce', '--purpose', purpose])))
                    with patch.dict(os.environ, dict(ETUDE_ADMITTED='/site',
                            ETUDE_PHASE_SELECTION_EXPORT='/unrelated', ETUDE_PREPARATION_CLEAN='stale'), clear=True), \
                            patch.object(routine_remote_stage.sys, 'argv', ['stage', str(operation)]), \
                            patch.object(routine_remote_stage, 'run') as run:
                        routine_remote_stage.main()
                        self.assertEqual(os.environ.get('ETUDE_PREPARATION_CLEAN'), '1' if clean else None)
                        self.assertNotIn('ETUDE_PHASE_SELECTION_EXPORT', os.environ)
                        self.assertEqual(run.call_args.args[0][:3], ['/etude', 'lock', 'tree'])
                bad = json.loads(operation.read_text())
                bad['clean'] = True
                operation.write_text(json.dumps(bad))
                with patch.object(routine_remote_stage.sys, 'argv', ['stage', str(operation)]), \
                        patch.object(routine_remote_stage, 'run', side_effect=AssertionError('invalid operation')):
                    with self.assertRaisesRegex(ValueError, 'preparation condition'):
                        routine_remote_stage.main()
            finally:
                os.chdir(original)

    def test_counted_vectors(self):
        program = '/immutable/etude'
        for vector in ([program], [program, 'run', '-S', 'saved', 'all']):
            counted = routine_entry.counted(program, vector)
            self.assertEqual(counted[:2], [program, str(len(vector))])
            self.assertEqual(counted[2:], vector)


if __name__ == '__main__':
    unittest.main()
