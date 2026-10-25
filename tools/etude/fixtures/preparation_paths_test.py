"""Bounded preparation filesystem and native path regressions."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
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


if __name__ == '__main__':
    unittest.main()
