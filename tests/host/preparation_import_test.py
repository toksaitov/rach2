#!/usr/bin/env python3
"""Check the preparation product's import identities and native build vectors."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import struct
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/etude'))
import preparation_import as product
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/ci/fixtures'))
from routine_route_test import RouteTests


class ImportTests(unittest.TestCase):
    @staticmethod
    def native_tree(tree):
        repo = Path(__file__).resolve().parents[2]
        for name in ('mk', 'tools/etude', 'tests/host', 'tests/cases/host'):
            (tree / name).mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo / 'mk/host-check.mk', tree / 'mk/host-check.mk')
        (tree / 'tools/etude/sources.mk').write_text('')
        (tree / 'tests/cases/host.host.json').write_text('{}\n')
        (tree / 'cc').symlink_to(Path(shutil.which('cc')).resolve())

    @staticmethod
    def native_source(tree, name, metadata='', body='int main(void) { return 0; }\n'):
        (tree / 'tests/host' / name).write_text('/* ' + metadata.replace('\n', '\n * ') + '*/\n' + body)

    def native_prepare(self, tree, group='tests', args=(), env=None, expected=0):
        repo = Path(__file__).resolve().parents[2]
        binary = Path(os.environ.get('ETUDE_HOST_PREPARE', str(repo / 'build/tools/etude/etude'))).resolve()
        output = 'build/tests/host-suite/' + group + '.inventory.json'
        actual = os.environ.copy()
        actual.update(CC=str(tree / 'cc'))
        actual.pop('HOST_TEST_PLATFORM', None)
        actual.update(env or {})
        if actual.get('HOST_TEST_PLATFORM') == 'windows':
            actual['CC'] = subprocess.list2cmdline([sys.executable, str(tree / 'compiler.py')])
            actual['OS'] = 'Windows_NT'
        result = subprocess.run([str(binary), 'prepare', '-H', *args, group, output],
                                cwd=tree, env=actual, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, expected, result.stdout)
        record = json.loads((tree / output).read_text()) if not expected and '--print-commands' not in args else None
        return record, result.stdout

    @staticmethod
    def windows_tree(tree):
        compiler = Path(shutil.which('cc')).resolve()
        sdk = tree.parent / 'sdk'
        sdk.mkdir(exist_ok=True)
        (sdk / 'external.h').write_text('#define EXTERNAL 0\n')
        (tree / 'include').mkdir(exist_ok=True)
        (tree / 'include/shadow.h').write_text('#define SHADOW 0\n')
        win = tree / 'tools/etude/win'
        win.mkdir(exist_ok=True)
        (tree / 'tools/etude/volume').mkdir(exist_ok=True)
        (tree / 'tools/etude/schemas').mkdir(exist_ok=True)
        (tree / 'tools/etude/schemas/history.schema.json').write_text('{}\n')
        (win / 'value.h').write_text('#define VALUE 0\n')
        (win / 'wintarget.c').write_text('#include "value.h"\n#include <shadow.h>\n'
                                       '#include <external.h>\nint main(void) { return VALUE + SHADOW + EXTERNAL; }\n')
        for name in ('winstand.c', 'fixture.c', 'main.c', 'image_command.c'):
            (win / name).write_text('int main(void) { return 0; }\n')
        for name in ('hold.c', 'winfmt.c', 'files.c'):
            (win / name).write_text('typedef int fixture_declaration;\n')
        (tree / 'tools/etude/volume/volume.c').write_text('typedef int fixture_declaration;\n')
        (tree / 'tools/etude/volume/etude_volume.c').write_text('int main(void) { return 0; }\n')
        (tree / 'tools/etude/sources.mk').write_text(
            'ETUDE_WIN_FIXTURE_SRC = tools/etude/win/fixture.c\n'
            'ETUDE_WIN_EXEC_SRC = tools/etude/win/files.c\n'
            'ETUDE_WIN_FILE_SRC = tools/etude/win/files.c\n'
            'ETUDE_WIN_SRC = tools/etude/win/main.c\n'
            'ETUDE_VOLUME_SRC = tools/etude/volume/etude_volume.c tools/etude/volume/volume.c\n')
        # A test compiler models Win32 flags and automatic executable suffixes.
        wrapper = tree / 'compiler.py'
        wrapper.write_text('import os, subprocess, sys\n'
                           'args = [a for a in sys.argv[1:] if a not in '
                           "(\'-static\', \'-municode\', \'-lws2_32\', \'-lbcrypt\')]\n"
                           'args += ["-Iinclude", "-isystem", ' + repr(str(sdk)) + ']\n'
                           'result = subprocess.run([' + repr(str(compiler)) + ', *args])\n'
                           'if result.returncode == 0 and "-o" in args:\n'
                           '    output = args[args.index("-o") + 1]\n'
                           '    os.replace(output, output + ".exe")\n'
                           'sys.exit(result.returncode)\n')
        (tree / 'cc').unlink(missing_ok=True)
        (tree / 'cc').symlink_to(compiler)
        return wrapper

    def test_etude_partition_modes(self):
        repo = Path(__file__).resolve().parents[2]
        manifest = json.loads((repo / 'tests/cases/host.host.json').read_text())
        owners = {}
        for case in manifest['cases']:
            if case['case_id'] not in ('host.host.etude', 'host.host.etude_quiet'):
                continue
            limit = next(w['limit'] for w in case['progress'] if w['witness_id'] == 'checks')
            self.assertEqual(limit, len(case['assertions']))
            for row in case['assertions']:
                if row['assertion_id'] != 'listed':
                    self.assertNotIn(row['assertion_id'], owners)
                    owners[row['assertion_id']] = case['case_id'].endswith('_quiet')
        self.assertEqual({name for name, alone in owners.items() if alone},
                         {'etude_platform', 'etude_remote', 'etude_repeat'})
        with tempfile.TemporaryDirectory(prefix='rach2-partition.') as work:
            tree = Path(work)
            self.native_tree(tree)
            for source in (repo / 'tests/host').glob('etude_*_hosttest.c'):
                if 'host-arch:' in source.read_text():
                    continue
                identity = source.stem.removesuffix('_hosttest')
                self.assertIn(identity, owners)
                alone = 'host-alone: native isolation\n' if 'host-alone:' in source.read_text() else ''
                self.native_source(tree, source.name, alone)
            for windows in (False, True):
                env = {'HOST_TEST_PLATFORM': 'windows'} if windows else {}
                if windows:
                    self.windows_tree(tree)
                record, _ = self.native_prepare(tree, 'etude', env=env)
                self.assertEqual({row['assertion_id'] for row in record['checks']}, set(owners))
                for row in record['checks']:
                    self.assertEqual(row['mode'], 'alone' if owners[row['assertion_id']] else 'shared')

    def test_script_partition_routes(self):
        repo = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix='rach2-script-route.') as work:
            tree = Path(work)
            self.native_tree(tree)
            for name in ('host.list', 'adapters.json'):
                shutil.copy2(repo / 'tests/cases/host' / name, tree / 'tests/cases/host' / name)
            shutil.copy2(repo / 'tests/cases/host.host.json', tree / 'tests/cases/host.host.json')
            import re
            recipe = (repo / 'mk/host-fixtures.mk').read_text()
            for name in set(re.findall(r'(?:rach2|tests)/[A-Za-z0-9_./-]+\.(?:c|h|py)', recipe)):
                source = repo / name
                if source.is_file():
                    (tree / name).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, tree / name)
            shutil.copy2(repo / 'mk/host-fixtures.mk', tree / 'mk/host-fixtures.mk')
            _, output = self.native_prepare(tree, 'scripts', ['--print-commands'])
            self.assertIn('fixtures/chown_plan.c', output)
            manifest = json.loads((tree / 'tests/cases/host.host.json').read_text())
            names = [line.split('|', 1)[0] for line in
                     (tree / 'tests/cases/host/host.list').read_text().splitlines()
                     if line and not line.startswith('#')]
            self.assertEqual((len(names), len(set(names))), (45, 45))
            case = next(row for row in manifest['cases'] if row['case_id'] == 'host.host.scripts_images')
            assertion = next(row for row in case['assertions'] if row['assertion_id'] == 'mkext2_overlay')
            case['assertions'].remove(assertion)
            for extra in ([], [assertion, assertion]):
                case['assertions'].extend(extra)
                (tree / 'tests/cases/host.host.json').write_text(json.dumps(manifest))
                self.native_prepare(tree, 'scripts', ['--print-commands'], expected=1)

    def test_gnu_inventory_predecessor(self):
        with tempfile.TemporaryDirectory(prefix='rach2-gnu-reuse.') as work:
            root = Path(work)
            old, new = root / 'old', root / 'new'
            for tree in (old, new):
                self.native_tree(tree)
                (tree / 'include').mkdir()
                (tree / 'include/choice.h').write_text('#define VALUE 0\n')
                self.native_source(tree, 'first_hosttest.c', 'host-cflags: -Iinclude\n'
                                   'host-inputs: runtime.txt\n',
                                   '#include "choice.h"\nint main(void) { return VALUE; }\n')
                self.native_source(tree, 'second_hosttest.c')
                (tree / 'runtime.txt').write_text('initial runtime bytes\n')
            record, _ = self.native_prepare(old, args=['--gnu'])
            original = [(old / row['program']).read_bytes() for row in record['checks']]
            def retry(expected, statuses):
                (new / 'build/tests/host-suite/tests.inventory.json').unlink(missing_ok=True)
                current, output = self.native_prepare(new, args=['--gnu', '--previous', str(old)])
                self.assertEqual(output.count(' -o '), expected)
                self.assertEqual([subprocess.run([str(new / row['program'])]).returncode
                                  for row in current['checks']], statuses)
                return current
            retry(0, [0, 0])
            (new / 'runtime.txt').write_text('new runtime bytes\n')
            current = retry(0, [0, 0])
            self.assertNotIn('runtime.txt', [row.get('path') for row in current['checks'][0]['compile_inputs']])
            (new / 'include/choice.h').write_text('#define VALUE 1\n')
            retry(1, [1, 0])
            (new / 'include/choice.h').write_text('#define VALUE 0\n')
            (new / 'tests/host/choice.h').write_text('#define VALUE 2\n')
            retry(1, [2, 0])
            (new / 'tests/host/choice.h').unlink()
            self.native_source(new, 'second_hosttest.c', body='int main(void) { return 3; }\n')
            retry(1, [0, 3])
            self.native_source(new, 'first_hosttest.c', 'host-cflags: -Iinclude -DCHANGED=1\n',
                               '#include "choice.h"\nint main(void) { return VALUE; }\n')
            retry(2, [0, 3])
            self.assertEqual([(old / row['program']).read_bytes() for row in record['checks']], original)
            (old / record['checks'][0]['program']).write_bytes(b'corrupt predecessor')
            retry(2, [0, 3])

    def test_shared_tool_predecessor(self):
        with tempfile.TemporaryDirectory(prefix='rach2-tool-predecessor.') as work:
            root = Path(work)
            old, new = root / 'old', root / 'new'
            for tree in (old, new):
                self.native_tree(tree)
                self.native_source(tree, 'probe_hosttest.c', 'host-tools: mkfatfixture\n')
                (tree / 'tests/host/value.h').write_text('#define VALUE 0\n')
                (tree / 'tests/host/mkfatfixture.c').write_text(
                    '#include "value.h"\nint main(void) { return VALUE; }\n')
            self.native_prepare(old, args=['--gnu'])
            program = Path('build/tests/host-suite/tools/mkfatfixture')
            marker = Path(str(program) + '.build.json')
            before = ((old / program).read_bytes(), (old / marker).read_bytes())
            _, output = self.native_prepare(new, args=['--gnu', '--previous', str(old)])
            self.assertEqual(output.count(' -o '), 0)
            self.assertEqual(((new / program).read_bytes(), (new / marker).read_bytes()), before)
            self.assertNotEqual((old / program).stat().st_ino, (new / program).stat().st_ino)
            (new / 'tests/host/value.h').write_text('#define VALUE 1\n')
            _, output = self.native_prepare(new, args=['--gnu'])
            self.assertEqual(output.count(' -o '), 1)
            self.assertEqual(subprocess.run([str(new / program)]).returncode, 1)
            self.assertEqual(((old / program).read_bytes(), (old / marker).read_bytes()), before)

    def test_clean_goal_conditioning(self):
        original = dict(format='etude-preparation-receipt', run_id='original-production',
                        start=dict(monotonic_us=10), end=dict(monotonic_us=20))
        for state in ('clean', 'retained', 'unknown', None):
            receipt = dict(original)
            if state:
                receipt['conditioning'] = dict(inhouse=state, backend='ordinary', provenance=None)
            before = json.dumps(receipt, sort_keys=True)
            if state == 'clean':
                product.clean_goals(receipt, {'image': 'portable'}, 'clean')
            else:
                with self.assertRaises(ValueError):
                    product.clean_goals(receipt, {'image': 'portable'}, 'clean')
            product.clean_goals(receipt, {'image': 'portable'}, 'retained')
            product.clean_goals(receipt, {'program': 'native', 'checks': 'inventory'}, 'clean')
            self.assertEqual(json.dumps(receipt, sort_keys=True), before)
        receipt = dict(original, conditioning=dict(inhouse='clean', backend='import',
                                                   provenance={'sha256': 'fixture'}))
        with self.assertRaises(ValueError):
            product.clean_goals(receipt, {'image': 'portable'}, 'clean')

    def test_roles(self):
        for value in ('build/tests/cases/machine/install.disk',
                      'build/x86_64/tests/cases/machine/install.disk',
                      'build/tests/cases/machine/test-password',
                      'build/tests/cases/host/fstool',
                      'build/tests/host-suite/schemas/case.schema.json',
                      'build/tests/style/sparse.json', 'build/tests/hello'):
            self.assertEqual(product.role(value), 'portable', value)
        for value in ('build/tests/cases/machine/disk', 'build/tools/etude/etude',
                      'build/krffs/readfile.krffs', 'build/tests/host-suite/scripts/kstack',
                      'build/tests/host-suite/sanitizer/tools.json'):
            self.assertEqual(product.role(value), 'native', value)
        self.assertEqual(product.role('build/tests/host-suite/tests.inventory.json'), 'inventory')

    def test_sanitizer_file_goal(self):
        repo = Path(__file__).resolve().parents[2]
        name = 'build/tests/host-suite/sanitizer/tools.json'
        goals = product.manifest_goals(repo)
        self.assertIn(name, goals)
        self.assertNotIn(str(Path(name).parent), goals)
        recipe = (repo / 'Makefile').read_text().split('.PHONY: host-sanitizer-force\n')[1].split('\n\n')[0]
        with tempfile.TemporaryDirectory(prefix='rach2-sanitizer-goal.') as work:
            tree = Path(work)
            (tree / 'tests/cases').mkdir(parents=True)
            (tree / 'tests/cases/host').symlink_to(repo / 'tests/cases/host', target_is_directory=True)
            (tree / 'tests/host').symlink_to(repo / 'tests/host', target_is_directory=True)
            (tree / 'Makefile').write_text('.PHONY: host-sanitizer-force\n' + recipe + '\n')
            with patch.dict(os.environ, {'MAKEFLAGS': '', 'MFLAGS': '', 'MAKELEVEL': ''}):
                product.build_native(tree, {name: 'native'}, [])
            self.assertTrue((tree / name).is_file())
            artifacts = product.validate_native(tree, {name: 'native'})
            self.assertEqual(artifacts[name], product.describe(tree / name))
            manifest = json.loads((tree / name).read_text())
            manifest['platform'] = 'foreign'
            (tree / name).write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                product.validate_native(tree, {name: 'native'})

    def test_sanitizer_source_binding(self):
        repo = Path(__file__).resolve().parents[2]
        name = 'build/tests/host-suite/sanitizer/tools.json'
        resolve = Path.resolve
        with tempfile.TemporaryDirectory(prefix='rach2-sanitizer-source.') as work:
            tree = Path(work).resolve()
            source = tree / 'system-atos'
            source.write_bytes(b'\xcf\xfa\xed\xfenative fixture bytes')
            previous = Path.cwd()
            try:
                os.chdir(tree)
                def native_path(path, *args, **kwargs):
                    return source if path == Path('/usr/bin/atos') else resolve(path, *args, **kwargs)
                with patch.object(sys, 'platform', 'darwin'), \
                        patch.object(product, 'windows_host', return_value=False), \
                        patch.object(Path, 'resolve', native_path):
                    snapshot = tree / Path(name).parent / 'atos'
                    snapshot.parent.mkdir(parents=True)
                    shutil.copy2(source, snapshot)
                    provenance = Path(str(snapshot) + '.provenance.json')
                    provenance.write_text(json.dumps(dict(source=str(source), sha256=product.digest(source))))
                    bound = [dict(path=str(p.relative_to(tree)), sha256=product.digest(p))
                             for p in (snapshot, provenance)]
                    (tree / name).write_text(json.dumps(dict(schema_version=1, platform='darwin', inputs=bound)))
                    expected = {name, str(Path(name).parent / 'atos'),
                                str(Path(name).parent / 'atos.provenance.json')}
                    self.assertEqual(set(product.validate_native(tree, {name: 'native'})), expected)
                    for changed in (tree / Path(name).parent / 'atos', source):
                        before = changed.read_bytes()
                        changed.write_bytes(before + b'changed')
                        with self.assertRaises(ValueError):
                            product.validate_native(tree, {name: 'native'})
                        changed.write_bytes(before)
                    manifest = json.loads((tree / name).read_text())
                    for rows in ([], manifest['inputs'] * 2,
                                 [dict(path='../elsewhere', sha256='a' * 64)]):
                        (tree / name).write_text(json.dumps(dict(manifest, inputs=rows)))
                        with self.assertRaises(ValueError):
                            product.validate_native(tree, {name: 'native'})
                    (tree / name).write_text(json.dumps(manifest))
                    snapshot = tree / Path(name).parent / 'atos'
                    snapshot.unlink()
                    snapshot.symlink_to(source)
                    with self.assertRaises(ValueError):
                        product.validate_native(tree, {name: 'native'})
            finally:
                os.chdir(previous)

    def test_record_identity(self):
        line = 'file ' + 'a' * 64 + ' 5 1 0 build/images/root.disk\n'
        good = 'etude-prepare 1\ntree /producer\n' + line
        self.assertEqual(product.record_files(good)['build/images/root.disk']['size_bytes'], 5)
        for bad in (good + line, good.replace('build/images/root.disk', '../root'),
                    good.replace('a' * 64, 'z' * 64), 'etude-prepare 2\n'):
            with self.assertRaises(ValueError):
                product.record_files(bad)

    def test_receipt_binding(self):
        record = 'etude-prepare 1\ntree /producer\n'
        receipt = dict(format='etude-preparation-receipt', schema_version=1,
                       outcome='pass', cleanup='confirmed', commit='1' * 40,
                       run_id='production-1', machine_id='linux', boot_id='boot-1',
                       record_sha256=hashlib.sha256(record.encode()).hexdigest(),
                       start=dict(utc='2026-10-24T00:00:00Z', monotonic_us=1, resolution_us=1),
                       end=dict(utc='2026-10-24T00:00:01Z', monotonic_us=5, resolution_us=1))
        product.receipt_check(receipt, '1' * 40, record)
        for key, value in [('cleanup', 'unknown'), ('outcome', 'fail'),
                           ('commit', '2' * 40), ('record_sha256', '0' * 64),
                           ('end', dict(receipt['end'], monotonic_us=0)),
                           ('run_id', ''), ('boot_id', 'other boot'),
                           ('start', dict(receipt['start'], resolution_us=0))]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                product.receipt_check(dict(receipt, **{key: value}), '1' * 40, record)

    def test_changed_file(self):
        with tempfile.TemporaryDirectory(prefix='rach2-import-', dir=None) as work:
            path = Path(work) / 'image'
            path.write_bytes(b'one')
            expected = product.describe(path)
            product.verify(path, expected)
            path.write_bytes(b'two')
            with self.assertRaises(ValueError):
                product.verify(path, expected)
            path.unlink()
            path.symlink_to('/dev/null')
            with self.assertRaises(ValueError):
                product.describe(path)

    def test_inventory_chain(self):
        path = 'build/tests/host-suite/tests.inventory.json'
        text = json.dumps(dict(manifest_sha256='m', checks=[dict(assertion_id='exfat', inputs=[
            dict(path='build/tests/host-suite/tools/exfat_external/image', sha256='f' * 64)])]))
        files = {path: dict(sha256=hashlib.sha256(text.encode()).hexdigest(), size_bytes=len(text))}
        self.assertEqual(product.nested_inputs({path: text}, files, 'm'),
                         {'build/tests/host-suite/tools/exfat_external/image': 'f' * 64})
        for changed, manifest in ((text + ' ', 'm'), (text, 'different')):
            with self.assertRaises(ValueError):
                product.nested_inputs({path: changed}, files, manifest)

    def test_alignment_inventory_chain(self):
        def anchored(checks, arch='i686'):
            prefix = 'build/x86_64' if arch == 'x86_64' else 'build'
            name = prefix + '/tests/host-suite/tests.inventory.json'
            text = json.dumps(dict(manifest_sha256='m', checks=checks))
            return {name: text}, {name: dict(sha256=hashlib.sha256(text.encode()).hexdigest(),
                                            size_bytes=len(text.encode()))}

        expected, inventories, files = {}, {}, {}
        for arch, prefix, sha in [('i686', product.E2FSPROGS_LIBS[0], 'a' * 64),
                                  ('x86_64', product.E2FSPROGS_LIBS[1], 'b' * 64)]:
            paths = [prefix + 'ext2fs/unix_io.c', prefix + 'ext2fs/ext2_types.h',
                     prefix + 'ext2fs/ext2fs.h', prefix + 'et/com_err.h']
            inputs = [dict(path=path, sha256=sha) for path in paths]
            inputs += [dict(path='tests/host/compat/e2fsprogs/config.h', sha256='c' * 64),
                       dict(path=prefix + 'unrelated.o', sha256='d' * 64)]
            checks = [dict(assertion_id=name, inputs=inputs)
                      for name in sorted(product.ALIGNMENT_CHECKS)]
            more, anchors = anchored(checks, arch)
            inventories.update(more)
            files.update(anchors)
            expected.update({path: sha for path in paths})
        self.assertEqual(product.nested_inputs(inventories, files, 'm'), expected)
        for bad in ('../escape.h', product.E2FSPROGS_LIBS[0] + '../escape.h'):
            changed, anchors = anchored([dict(assertion_id='e2fsprogs_alignment',
                                              inputs=[dict(path=bad, sha256='a' * 64)])])
            with self.assertRaises(ValueError):
                product.nested_inputs(changed, anchors, 'm')
        for inputs in ([dict(path=product.E2FSPROGS_LIBS[0] + 'ext2fs/ext2_types.h', sha256='bad')],
                       [dict(path=product.E2FSPROGS_LIBS[0] + 'ext2fs/ext2_types.h', sha256=sha)
                        for sha in ('a' * 64, 'b' * 64)]):
            changed, anchors = anchored([dict(assertion_id='e2fsprogs_alignment', inputs=inputs)])
            with self.assertRaises(ValueError):
                product.nested_inputs(changed, anchors, 'm')

    def test_native_program_publication(self):
        with tempfile.TemporaryDirectory(prefix='rach2-native-publication.') as work:
            tree = Path(work)
            self.native_tree(tree)
            self.native_source(tree, 'etude_platform_hosttest.c', 'host-alone: native isolation\n')
            self.windows_tree(tree)
            record, _ = self.native_prepare(tree, 'etude', env={'HOST_TEST_PLATFORM': 'windows'})
            program = tree / record['checks'][0]['program']
            self.assertTrue(program.is_file())
            self.assertFalse(Path(str(program) + '.exe').exists())

    def test_shared_native_tool_reuse(self):
        with tempfile.TemporaryDirectory(prefix='rach2-native-tool.') as work:
            tree = Path(work)
            self.native_tree(tree)
            self.native_source(tree, 'probe_hosttest.c', 'host-tools: mkfatfixture\n')
            (tree / 'tests/host/mkfatfixture.c').write_text('int main(void) { return 0; }\n')
            first, _ = self.native_prepare(tree, args=['--gnu'])
            program = tree / 'build/tests/host-suite/tools/mkfatfixture'
            marker = Path(str(program) + '.build.json')
            before = (program.read_bytes(), marker.read_bytes(), program.stat().st_mtime_ns)
            second, output = self.native_prepare(tree, args=['--gnu'])
            self.assertEqual(first, second)
            self.assertEqual(output.count(' -o '), 0)
            self.assertEqual(before, (program.read_bytes(), marker.read_bytes(), program.stat().st_mtime_ns))
            program.write_bytes(before[0] + b'changed')
            self.native_prepare(tree, args=['--gnu'], expected=1)
            program.write_bytes(before[0])
            for invalid in ({}, dict(schema_version=1, recipe={}, sha256=hashlib.sha256(before[0]).hexdigest())):
                marker.write_text(json.dumps(invalid))
                self.native_prepare(tree, args=['--gnu'], expected=1)
            marker.write_bytes(before[1])
            (tree / 'tests/host/mkfatfixture.c').write_text('invalid C syntax\n')
            self.native_prepare(tree, args=['--gnu'], expected=1)
            self.assertEqual((program.read_bytes(), marker.read_bytes()), before[:2])

    def test_windows_companion_reuse(self):
        with tempfile.TemporaryDirectory(prefix='rach2-companion-cache.') as work:
            tree = Path(work)
            self.native_tree(tree)
            self.native_source(tree, 'etude_platform_hosttest.c', 'host-alone: native isolation\n')
            self.windows_tree(tree)
            env = {'HOST_TEST_PLATFORM': 'windows'}
            first, _ = self.native_prepare(tree, 'etude', env=env)
            root = tree / 'build/tests/host-suite/etude/platform'
            names = ['wintarget.exe', 'winstand.exe', 'etude.exe', 'etude-volume.exe', 'image-command.exe']
            programs = [root / name for name in names]
            before = [(p.read_bytes(), p.stat().st_mtime_ns) for p in programs]
            second, output = self.native_prepare(tree, 'etude', env=env)
            self.assertEqual(first, second)
            self.assertEqual(output.count(' -o '), 0)
            self.assertEqual(before, [(p.read_bytes(), p.stat().st_mtime_ns) for p in programs])
            header = tree / 'tools/etude/win/value.h'
            header.write_text('#define VALUE 1\n')
            _, output = self.native_prepare(tree, 'etude', env=env)
            self.assertEqual(output.count(' -o '), 1)
            self.assertEqual(subprocess.run([str(programs[0])]).returncode, 1)
            header.write_text('#define VALUE 0\n')
            (tree / 'tools/etude/win/shadow.h').write_text('#define SHADOW 2\n')
            _, output = self.native_prepare(tree, 'etude', env=env)
            self.assertEqual(output.count(' -o '), 1)
            self.assertEqual(subprocess.run([str(programs[0])]).returncode, 2)
            sdk = tree.parent / 'sdk'
            (sdk / 'external.h').write_text('#define EXTERNAL 3\n')
            _, output = self.native_prepare(tree, 'etude', env=env)
            self.assertEqual(output.count(' -o '), 1)
            self.assertEqual(subprocess.run([str(programs[0])]).returncode, 5)
            marker = Path(str(programs[0]) + '.build.json')
            before = (programs[0].read_bytes(), marker.read_bytes())
            programs[0].write_bytes(before[0] + b'changed')
            self.native_prepare(tree, 'etude', env=env, expected=1)
            programs[0].write_bytes(before[0])
            marker.write_text('{}')
            self.native_prepare(tree, 'etude', env=env, expected=1)
            marker.write_bytes(before[1])
            (tree / 'tools/etude/win/wintarget.c').write_text('invalid C syntax\n')
            self.native_prepare(tree, 'etude', env=env, expected=1)
            self.assertEqual((programs[0].read_bytes(), marker.read_bytes()), before)

    def test_windows_vectors(self):
        goals = ['build/tests/cases/machine/scenario' + str(i) for i in range(900)]
        base = ['make', '-o' + 'a' * 20000]
        commands = list(product.batches(base, goals))
        self.assertGreater(len(commands), 1)
        self.assertEqual([goal for argv in commands for goal in argv[len(base):]], goals)
        self.assertTrue(all(len(subprocess.list2cmdline(argv)) < 28000 for argv in commands))
        with self.assertRaises(ValueError):
            list(product.batches(['make', 'a' * 29000], goals))

    def test_native_krffs_root_alignment(self):
        repo = Path(__file__).resolve().parents[2]
        spec = importlib.util.spec_from_file_location('native_prepare_fixture', repo / 'tests/native/prepare.py')
        native = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(native)
        products = Path(os.environ.get('HOST_PRODUCTS', '/host-products'))
        tools = {'mkfs': products / 'mkfs.krffs', 'stage': products / 'stageimage.krffs'}
        self.assertTrue(all(os.access(path, os.X_OK) for path in tools.values()))

        def nodes(path):
            result, offset = [], 0
            with path.open('rb') as image:
                while offset < path.stat().st_size:
                    image.seek(offset)
                    head = image.read(357)
                    size, back, data, parent = struct.unpack_from('<QQQQ', head, 325)
                    self.assertGreaterEqual(size, 357)
                    self.assertLessEqual(offset + size, path.stat().st_size)
                    if head[2] != 1:
                        result.append((offset, head[11:267].split(b'\0', 1)[0],
                                       struct.unpack_from('<HII', head, 267),
                                       head[277:325], size, data, parent, image.read(data)))
                    offset += size
            return result

        with tempfile.TemporaryDirectory(prefix='rach2-native-root.') as directory:
            tree = Path(directory).resolve()
            work = tree / 'work'
            work.mkdir()
            image, payload = tree / 'root.img', tree / 'program'
            before = []
            invoke = native.invoke

            def stage(args, **kwargs):
                result = invoke(args, **kwargs)
                if str(args[0]) == str(tools['stage']) and Path(args[1]).name == 'root.manifest':
                    before[:] = nodes(image)
                return result

            text = (f'dir 0750:51000:52000 /owned\n'
                    f'file 04755:51000:52000 /program {payload}\n')
            with patch.object(native, 'invoke', side_effect=stage):
                for length in (0, 1, 1318, 2668, 3025):
                    payload.write_bytes(b'x' * length)
                    native.root_image(tree, image, 'krffs', 1, text, os.environ.copy(), work, tools)
                    used, free = native.krffs_free_tail(image)
                    self.assertEqual(used % 4096, 0)
                    self.assertGreaterEqual(free, 1048576)
                    after = nodes(image)
                    self.assertEqual(after[:-1], before)
                    pad = after[-1]
                    self.assertEqual(pad[1], native.ROOT_PADDING[1:].encode())
                    self.assertEqual(pad[2], (0o444, 0, 0))
                    self.assertEqual(pad[-1], bytes(pad[5]))
                    self.assertLess(pad[5], 4096)
                    original_free = before[-1][0] + before[-1][4]
                    if length == 1318:
                        self.assertEqual(original_free % 4096, 2389)
                        for file_bytes in (0, 1):
                            old_time = original_free + 4 * 357 + file_bytes + 277
                            new_time = used + 4 * 357 + file_bytes + 277
                            self.assertNotEqual(old_time // 4096, (old_time + 15) // 4096)
                            self.assertEqual(new_time // 4096, (new_time + 15) // 4096)
                    digest = product.digest(image)
                    native.root_image(tree, image, 'krffs', 1, text, os.environ.copy(), work, tools)
                    self.assertEqual(product.digest(image), digest)
                for line in (f'file 0444 {native.ROOT_PADDING} {payload}\n',
                             f'dir 0755 {native.ROOT_PADDING}\n'):
                    with self.assertRaisesRegex(ValueError, 'native root reserves'):
                        native.root_image(tree, image, 'krffs', 1, line, os.environ.copy(), work, tools)
                payload.write_bytes(bytes(10 * 1048576 - 3 * 357))
                with self.assertRaisesRegex(ValueError, 'insufficient free space'):
                    native.root_image(tree, image, 'krffs', 1, text, os.environ.copy(), work, tools)

    def test_native_compilation(self):
        with tempfile.TemporaryDirectory(prefix='rach2-import-') as work:
            tree = Path(work)
            source, header = tree / 'source.c', tree / 'value.h'
            source.write_text('#include "value.h"\nint main(void) { return VALUE; }\n')
            header.write_text('#define VALUE 0\n')
            portable = tree / 'portable'
            portable.write_bytes(b'producer bytes')
            (tree / 'Makefile').write_text(
                'build/tests/cases/machine/a: source.c value.h portable\n'
                '\tmkdir -p $(@D)\n\tprintf "compile\\n" >> compile.calls\n'
                '\t$(CC) -o $@ $<\n'
                '.PHONY: portable\nportable:\n\tprintf changed > $@\n')
            target = tree / 'build/tests/cases/machine/a'
            target.parent.mkdir(parents=True)
            target.write_bytes(b'old foreign executable')
            newest = max(source.stat().st_mtime_ns, header.stat().st_mtime_ns) + 2000000000
            os.utime(target, ns=(newest, newest))
            roles = {'build/tests/cases/machine/a': 'native'}
            with patch.dict(os.environ, {'MAKEFLAGS': '', 'MFLAGS': '', 'MAKELEVEL': ''}):
                commands = product.build_native(tree, roles, ['portable'])
                self.assertEqual(len(commands), 1)
                product.validate_native(tree, roles)
                self.assertNotEqual(target.read_bytes(), b'old foreign executable')
                before = target.stat()
                product.build_native(tree, roles, ['portable'])
                self.assertEqual((before.st_ino, before.st_mtime_ns),
                                 (target.stat().st_ino, target.stat().st_mtime_ns))
                self.assertEqual((tree / 'compile.calls').read_text().splitlines(), ['compile'])
                for changed, text in ((header, '#define VALUE 1\n'),
                                      (source, '#include "value.h"\nint main(void) { return VALUE + 1; }\n')):
                    changed.write_text(text)
                    newer = target.stat().st_mtime_ns + 2000000000
                    os.utime(changed, ns=(newer, newer))
                    product.build_native(tree, roles, ['portable'])
                    product.validate_native(tree, roles)
                    os.utime(changed, ns=(1, 1))
            self.assertEqual((tree / 'compile.calls').read_text().splitlines(), ['compile'] * 3)
            self.assertEqual(portable.read_bytes(), b'producer bytes')

    def test_forced_openssl_dependency(self):
        repo = Path(__file__).resolve().parents[2]
        real_make = Path(os.environ.get('ETUDE_MAKE_REAL') or shutil.which('make')).resolve()
        with tempfile.TemporaryDirectory(prefix='rach2-import-') as work:
            tree = Path(work)
            script = tree / 'tests/system/fakeapi/build_openssl.sh'
            script.parent.mkdir(parents=True)
            shutil.copy2(repo / 'tests/system/fakeapi/build_openssl.sh', script)
            wrapper = tree / 'bin/make'
            wrapper.parent.mkdir()
            shutil.copy2(repo / 'tools/etude/make.sh', wrapper)
            wrapper.chmod(0o755)
            uname = wrapper.parent / 'uname'
            uname.write_text('case "$1" in -s) echo Linux;; -m) echo x86_64;; *) exit 2;; esac\n')
            uname.chmod(0o755)
            vendor = tree / 'vendor/gnu'
            vendor.mkdir(parents=True)
            (vendor / 'fetch.sh').write_text("printf '%s\\n' unused-archive\n")
            pin = 'a' * 64
            (vendor / 'sources.txt').write_text('openssl 1 unused ' + pin + '\n')
            source = tree / ('build/vendor/host-openssl/source-' + pin)
            (source / 'include/openssl').mkdir(parents=True)
            (source / 'Configure').write_text('exit 0;\n')
            (source / 'configdata.mk').write_text('# configured\n')
            os.utime(source / 'Configure', (1, 1))
            os.utime(source / 'configdata.mk', (2, 2))
            (source / 'Makefile').write_text(
                'include configdata.mk\n'
                'configdata.mk: Configure\n\t@echo forced-upstream-configuration; exit 17\n'
                '.PHONY: build_libs\n'
                'build_libs:\n'
                '\t@printf "%s\\n" "$$ETUDE_BUILD_JOBS" "$$ETUDE_MAKE_REAL" > policy\n'
                '\t@printf ssl > libssl.a; printf crypto > libcrypto.a\n')
            (tree / 'Makefile').write_text(
                '.PHONY: all\nall:\n\tsh tests/system/fakeapi/build_openssl.sh\n')
            env = dict(os.environ, PATH=str(wrapper.parent) + os.pathsep + os.environ['PATH'],
                       ETUDE_BUILD_JOBS='2', ETUDE_MAKE_REAL=str(real_make),
                       MAKEFLAGS='', MFLAGS='', MAKELEVEL='0', OS='')
            subprocess.run([str(real_make), '-B', '-k', '-j2'], cwd=tree, env=env, check=True)
            self.assertEqual((source / 'policy').read_text().splitlines(), ['2', str(real_make)])
            self.assertEqual((tree / 'build/vendor/host-openssl/lib/libssl.a').read_bytes(), b'ssl')
            self.assertTrue((tree / 'build/vendor/host-openssl/ready').is_file())

    def test_idle_local_producer(self):
        with tempfile.TemporaryDirectory(prefix='rach2-import-') as work:
            tree = Path(work)
            root = tree / 'producer'
            dest = tree / 'native'
            root.mkdir()
            dest.mkdir()
            producer = dict(transport='local', tree=str(root))
            self.assertEqual(product.idle_local(producer, dest), root.resolve())
            with self.assertRaises(ValueError):
                product.idle_local(producer, root)
            alias = tree / 'alias'
            alias.symlink_to(root, target_is_directory=True)
            with self.assertRaises(ValueError):
                product.idle_local(dict(producer, tree=str(alias)), root)
            (root / 'build').mkdir()
            (root / 'build/.run.lock').symlink_to(root / 'absent')
            with self.assertRaises(ValueError):
                product.idle_local(producer, dest)

    def test_refused_wsl_transports(self):
        for mode in ('wsl',):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                product.validate_producer(dict(transport=mode))

    def test_posix_wsl_ssh(self):
        producer = dict(transport='wsl-ssh', tree='/producer',
                        metadata='/producer/build/producer.json', etude='/immutable/etude',
                        host='testing', distribution='Ubuntu-26.04', user='testing')
        with patch.object(product, 'windows_host', return_value=False):
            product.validate_producer(producer)
            words = product.remote_command(producer, ['/bin/cat', '/metadata'])
            self.assertEqual(words[:4], ['ssh', '-o', 'BatchMode=yes', 'testing'])
            self.assertEqual(words[4], 'env MSYS_NO_PATHCONV=1 wsl.exe --distribution '
                             'Ubuntu-26.04 --user testing --exec /bin/cat /metadata')
            with tempfile.TemporaryDirectory(prefix='rach2-import-') as work:
                with patch.object(product, 'run') as run:
                    product.transfer(producer, ['build/images/a.disk'], Path(work), Path(work))
                argv = run.call_args.args[0]
                self.assertEqual(argv.count('--compress'), 1)
                server = next(arg for arg in argv if arg.startswith('--rsync-path='))
                self.assertIn('--exec /immutable/etude lock tree /producer '
                              'prepare-import /usr/bin/rsync', server)
        for key in ('distribution', 'user'):
            for bad in (None, '', '-bad', 'bad value', 'bad;command'):
                with self.subTest(key=key, bad=bad), self.assertRaises(ValueError):
                    product.validate_producer(dict(producer, **{key: bad}))
        with patch.object(product, 'windows_host', return_value=True):
            with self.assertRaises(ValueError):
                product.validate_producer(producer)

    def test_ssh_compression(self):
        producer = dict(transport='ssh', tree='/producer',
                        metadata='/producer/metadata', etude='/immutable/etude', host='testing')
        with tempfile.TemporaryDirectory(prefix='rach2-import-') as work:
            with patch.object(product, 'run') as run:
                product.transfer(producer, ['build/images/a.disk'], Path(work), Path(work))
            argv = run.call_args.args[0]
            self.assertEqual(argv.count('--compress'), 1)
            self.assertIn('--sparse', argv)
            self.assertIn('--rsync-path=/immutable/etude lock tree /producer '
                          'prepare-import /usr/bin/rsync', argv)
            self.assertEqual(argv[-3:], ['--', 'testing:/producer/', work + '/'])

    def test_native_local_copy(self):
        with tempfile.TemporaryDirectory(prefix='rach2-import-') as work:
            tree = Path(work)
            root = tree / 'producer'
            dest = tree / 'native'
            root.mkdir()
            dest.mkdir()
            producer = dict(transport='local', tree=str(root))
            with patch.object(product, 'windows_host', return_value=True), \
                 patch.object(product, 'native_path', side_effect=lambda p: str(p)), \
                 patch.object(product, 'cygpath', side_effect=lambda option, value: value) as cygpath, \
                 patch.object(product, 'run') as run:
                product.transfer(producer, ['build/images/a.disk'], dest, tree)
            argv = run.call_args.args[0]
            self.assertNotIn('--compress', argv)
            self.assertNotIn('--force-local', argv)
            self.assertEqual(argv[-3:], ['--', str(root.resolve()) + '/', str(dest) + '/'])
            self.assertEqual(cygpath.call_count, 3)
            self.assertNotIn('ssh', argv)
            self.assertNotIn('wsl.exe', argv)
            with patch.object(product, 'windows_host', return_value=True):
                with self.assertRaises(ValueError):
                    product.validate_producer(dict(transport='ssh', tree='/producer',
                                                   metadata='/producer/metadata', etude='/etude', host='x'))

    def test_encoded_inputs(self):
        value = 'build/tests/na' + chr(239) + 've'
        self.assertEqual(product.inventory_path(dict(path_hex=os.fsencode(value).hex())), value)
        for item in (dict(path='/outside'), dict(path_hex='0'), dict(path_hex='XX'),
                     dict(path_hex='00'), dict(path_hex='61', path='a'), dict(path='../escape')):
            with self.assertRaises(ValueError):
                product.inventory_path(item)

    def test_actual_architecture_buckets(self):
        roles = {'build/tests/cases/machine/a': 'native',
                 'build/x86_64/tests/cases/machine/b': 'native',
                 'build/tests/host-suite/tests.inventory.json': 'inventory'}
        with patch.object(product, 'run') as run, patch.object(product, 'windows_host', return_value=True):
            commands = product.build_native(Path('.'), roles, ['build/x86_64/images/root.disk'])
        self.assertEqual(run.call_count, 3)
        self.assertEqual([next(x for x in argv if x.startswith('ARCH=')) for argv in commands],
                         ['ARCH=i686', 'ARCH=x86_64', 'ARCH=i686'])
        self.assertTrue(all('OS=Windows_NT' in argv and
                            '-obuild/x86_64/images/root.disk' in argv for argv in commands))
        self.assertTrue(all('-B' not in argv for argv in commands))
        with patch.object(product, 'run'):
            commands = product.build_native(Path('.'), roles, [], 'x86_64')
        self.assertTrue(all('ARCH=x86_64' in argv for argv in commands))


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, lambda signum, frame: sys.exit(128 + signum))
    unittest.main()
