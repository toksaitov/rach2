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


if __name__ == '__main__':
    unittest.main()
