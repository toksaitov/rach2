"""Bounded preparation path and context serialization regressions."""
import argparse
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import measurement_context as context
import preparation_paths as paths


class PreparationPaths(unittest.TestCase):
    def test_native_operands_use_msys_filesystem(self):
        with patch.object(paths, 'msys_host', return_value=True), \
                patch.object(paths, 'cygpath', return_value='/d/work name/input') as convert:
            self.assertEqual(paths.filesystem_path('D:/work name/input'), Path('/d/work name/input'))
            convert.assert_called_once_with('-au', 'D:/work name/input')
        with patch.object(paths, 'msys_host', return_value=True), \
                patch.object(paths, 'cygpath', return_value='//host/share/input') as convert:
            paths.filesystem_path('\\\\host\\share\\input')
            convert.assert_called_once_with('-au', '\\\\host\\share\\input')

    def test_relative_source_paths_are_not_translated(self):
        with patch.object(paths, 'msys_host', return_value=True), patch.object(paths, 'cygpath') as convert:
            self.assertEqual(paths.filesystem_path('tools/etude/etude.c'), Path('tools/etude/etude.c'))
            convert.assert_not_called()
        for value in ('D:/source.c', '../source.c', 'dir\\source.c'):
            with self.assertRaises(ValueError):
                context.relative(value)

    def test_cygpath_output_is_validated(self):
        for value in ('', '\n', 'D:/one\nD:/two\n', 'D:/one\r\n'):
            with patch.object(paths.subprocess, 'check_output', return_value=value):
                with self.assertRaises(ValueError):
                    paths.cygpath('-am', 'input')
        with patch.object(paths, 'msys_host', return_value=True), \
                patch.object(paths, 'cygpath', return_value='/d/still-posix'):
            with self.assertRaises(ValueError):
                paths.native_path(__file__)

    def test_unc_canonicalization_preserves_identity_without_realpath(self):
        unc = '//server/share/input'
        with patch.object(paths, 'msys_host', return_value=True), \
                patch.object(paths, 'cygpath', return_value=unc), \
                patch.object(Path, 'samefile', return_value=True) as same, \
                patch.object(Path, 'resolve', side_effect=AssertionError('UNC realpath')):
            self.assertEqual(str(paths.canonical_path(unc)), unc)
            self.assertEqual(paths.native_path(unc), unc)
            self.assertEqual(same.call_count, 2)
        with patch.object(paths, 'msys_host', return_value=True), \
                patch.object(paths, 'cygpath', return_value=unc), \
                patch.object(Path, 'samefile', return_value=False):
            with self.assertRaises(ValueError):
                paths.canonical_path(unc)

    def test_compose_serializes_every_native_descriptor(self):
        with tempfile.TemporaryDirectory(prefix='rach2-context-paths.') as scratch:
            root = Path(scratch).resolve()
            source = root/'source.c'
            source.write_bytes(b'unchanged source\n')
            snapshot = root/'snapshot.json'
            snapshot.write_text(json.dumps(dict(schema_version=1, commit='a'*40,
                    inputs=[dict(path='source.c', **context.digest(source))])))
            for name in ('etude', 'site', 'record'):
                (root/name).write_bytes(name.encode())
            trace = root/'trace'
            trace.write_text(json.dumps(dict(schema_version=1, kind='start',
                    method='native-load-v1', run_id='path-test'))+'\n')
            args = argparse.Namespace(tree=root, commit='a'*40, run_id='path-test',
                source_snapshot=snapshot, source_sha256=context.digest(snapshot)['sha256'],
                output=root/'output', etude=root/'etude', tool=[], site=root/'site',
                record=root/'record', preparation=None, purpose='focused', selection_export=None,
                cache_method='verified', ordinary_preparation='ordinary', one_time_production='none',
                outer_trace=trace, outer_stop_request=root/'stop', physical_trace=None,
                physical_stop_request=None)

            def convert(option, value):
                self.assertEqual(option, '-am')
                return 'D:/context/'+Path(value).relative_to(root).as_posix()

            with patch.object(paths, 'msys_host', return_value=True), \
                    patch.object(paths, 'cygpath', side_effect=convert), redirect_stdout(io.StringIO()):
                context.compose(args)
            doc = json.loads((args.output/'context.json').read_text())
            for name in ('source', 'artifacts', 'site_profile', 'tools', 'cache_state'):
                self.assertTrue(doc[name]['path'].startswith('D:/context/'), name)
            self.assertEqual(doc['outer_observation']['trace'], 'D:/context/trace')
            self.assertEqual(doc['outer_observation']['stop_request'], 'D:/context/stop')
            tools = json.loads((args.output/'tools.json').read_text())
            self.assertEqual(tools['inputs'][0]['path'], 'D:/context/etude')
            site = json.loads((args.output/'site.json').read_text())
            self.assertEqual(site['policy']['path'], 'D:/context/site')
            inputs = json.loads((args.output/'source.json').read_text())['inputs']
            self.assertEqual(inputs[0]['path'], 'source.c')
            self.assertEqual(inputs[0]['sha256'], context.digest(source)['sha256'])
            cache = json.loads((args.output/'cache.json').read_text())
            self.assertFalse(cache['controlled'])
            self.assertEqual({v['state'] for v in cache['states'].values()}, {'unknown'})
            self.assertIsNone(doc['cache_evidence'])

    def test_cache_protocol_is_bound_and_profile_is_stable(self):
        with tempfile.TemporaryDirectory(prefix='rach2-context-cache.') as scratch:
            root = Path(scratch)
            evidence = root/'evidence'
            evidence.write_text('complete conditioning record\n')
            receipt = dict(run_id='sample-1', commit='a'*40, machine_id='host', boot_id='boot')
            doc = dict(format='etude-cache-protocol', schema_version=1, **receipt,
                       states={k: dict(state='warm', procedure='fixture')
                               for k in ('artifacts', 'build', 'tools', 'os')},
                       evidence=[dict(**context.digest(evidence), locations=[str(evidence)])])
            path = root/'protocol.json'
            args = argparse.Namespace(cache_protocol=path, cache_method='fixture',
                    ordinary_preparation='ordinary', one_time_production='none')

            def check(number):
                out = root/str(number)
                out.mkdir()
                path.write_text(json.dumps(doc))
                return context.cache_context(args, receipt, out)

            first, proof = check(1)
            self.assertTrue(json.loads(Path(first['path']).read_text())['controlled'])
            self.assertIsNotNone(proof)
            receipt['run_id'] = doc['run_id'] = 'sample-2'
            second, changed_proof = check(2)
            self.assertEqual(first['sha256'], second['sha256'])
            self.assertNotEqual(proof['sha256'], changed_proof['sha256'])
            doc['states']['os']['state'] = 'unknown'
            unknown, _ = check(3)
            self.assertFalse(json.loads(Path(unknown['path']).read_text())['controlled'])
            doc['run_id'] = 'another-run'
            with self.assertRaises(ValueError):
                check(4)
            doc['run_id'] = receipt['run_id']
            evidence.write_text('changed conditioning record\n')
            with self.assertRaises(ValueError):
                check(5)


if __name__ == '__main__':
    unittest.main()
