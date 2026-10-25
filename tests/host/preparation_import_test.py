#!/usr/bin/env python3
"""Check the preparation product's import identities and native build vectors."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
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
    def test_gnu_inventory_predecessor(self):
        repo = Path(__file__).resolve().parents[2]
        sys.path.insert(0, str(repo / 'tests/cases/host'))
        import prepare as inventory
        compiler = Path(shutil.which('cc')).resolve()
        output = 'build/tests/host-suite/checkers.inventory.json'
        compiles, flags = [], []
        real_run = subprocess.run

        def execute(argv, **kwargs):
            if '-o' in argv and '-MM' not in argv and '-M' not in argv:
                compiles.append(argv)
            return real_run(argv, **kwargs)

        def checks(group, arch, base):
            for name in ('first', 'second'):
                source, program = name + '.c', str(base / name)
                item = inventory.entry(name, name, program, [source])
                item['command'] = ['-std=c99', '-O0', '-Iinclude', *flags,
                                   '-o', program, source]
                yield item

        def prepare(tree, previous=None):
            args = SimpleNamespace(output=output, group='checkers', arch='i686', gnu=True,
                                   print_commands=False, only=None, previous=previous)
            with patch.object(inventory, 'ROOT', tree), \
                    patch.dict(os.environ, {'CC': str(tree / 'cc')}):
                inventory.prepare(args)

        with tempfile.TemporaryDirectory(prefix='rach2-gnu-reuse.') as directory:
            root, cwd = Path(directory).resolve(), Path.cwd()
            old, new = root / 'old', root / 'new'
            programs = [Path(output).parent / 'checkers' / n for n in ('first', 'second')]

            def retry(expected, statuses):
                (new / output).unlink(missing_ok=True)
                count = len(compiles)
                prepare(new, old)
                self.assertEqual(len(compiles) - count, expected)
                self.assertEqual([real_run([str(new / p)]).returncode for p in programs], statuses)

            try:
                for tree in (old, new):
                    (tree / 'tests/cases').mkdir(parents=True)
                    (tree / 'tests/cases/host.host.json').write_text('{}\n')
                    (tree / 'cc').symlink_to(compiler)
                    (tree / 'include').mkdir()
                    (tree / 'include/choice.h').write_text('#define VALUE 0\n')
                    (tree / 'first.c').write_text('#include "choice.h"\nint main(void) { return VALUE; }\n')
                    (tree / 'second.c').write_text('int main(void) { return 0; }\n')
                with patch.object(inventory, 'checks', side_effect=checks), \
                        patch.object(subprocess, 'run', side_effect=execute):
                    prepare(old)
                    self.assertEqual(len(compiles), 2)
                    originals = [(old / p).read_bytes() for p in programs]
                    retry(0, [0, 0])
                    self.assertEqual([(new / p).read_bytes() for p in programs], originals)
                    self.assertNotEqual((old / programs[0]).stat().st_ino,
                                        (new / programs[0]).stat().st_ino)
                    (new / 'include/choice.h').write_text('#define VALUE 1\n')
                    retry(1, [1, 0])
                    (new / 'include/choice.h').write_text('#define VALUE 0\n')
                    (new / 'choice.h').write_text('#define VALUE 2\n')
                    retry(1, [2, 0])
                    (new / 'choice.h').unlink()
                    (new / 'second.c').write_text('int main(void) { return 3; }\n')
                    retry(1, [0, 3])
                    (new / 'second.c').write_text('int main(void) { return 0; }\n')
                    flags.append('-DCHANGED_RECIPE=1')
                    retry(2, [0, 0])
                    flags.clear()
                    identity = inventory.compiler_record

                    def changed_identity(command):
                        value = identity(command)
                        value['version'] = 'a different compiler identity'
                        return value

                    with patch.object(inventory, 'compiler_record', side_effect=changed_identity):
                        retry(2, [0, 0])
                    self.assertEqual([(old / p).read_bytes() for p in programs], originals)
                    (old / programs[0]).write_bytes(b'corrupt predecessor')
                    retry(1, [0, 0])
                for name in ('old-sdk', 'new-sdk'):
                    (root / name).symlink_to(old, target_is_directory=True)
                commands = [[str(compiler), '--sysroot=' + str(root / name),
                             '-B' + str(root / name), '-L' + str(root / name)]
                            for name in ('old-sdk', 'new-sdk')]
                self.assertEqual(*[inventory.compiler_paths(c) for c in commands])
            finally:
                os.chdir(cwd)

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
        sys.path.insert(0, str(repo / 'tests/cases/host'))
        spec = importlib.util.spec_from_file_location('sanitizer_fixture_tools', repo / 'tests/cases/host/tools.py')
        tools = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tools)
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
                        patch.object(tools, 'external_tool', return_value=source), \
                        patch.object(Path, 'resolve', native_path):
                    tools.sanitizer_tools(Path(name).parent, product.digest)
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
        repo = Path(__file__).resolve().parents[2]
        sys.path.insert(0, str(repo / 'tests/cases/host'))
        import fixtures
        with tempfile.TemporaryDirectory(prefix='rach2-native-publication-') as work:
            output = Path(work, 'program')
            output.write_bytes(b'old declared bytes')
            emitted = Path(str(output) + '.exe')
            emitted.write_bytes(b'MZnew compiler bytes')
            self.assertEqual(fixtures.publish_program(output), output)
            self.assertEqual(output.read_bytes(), b'MZnew compiler bytes')
            self.assertFalse(emitted.exists())
            self.assertEqual(fixtures.publish_program(output), output)
            output.unlink()
            with self.assertRaises(ValueError):
                fixtures.publish_program(output)

    def test_shared_native_tool_reuse(self):
        repo = Path(__file__).resolve().parents[2]
        sys.path.insert(0, str(repo / 'tests/cases/host'))
        import prepare as inventory

        def tool_module():
            spec = importlib.util.spec_from_file_location('fixture_tools', repo / 'tests/cases/host/tools.py')
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            module.SOURCES['probe'] = ['probe.c']
            return module

        original_run = subprocess.run
        compiles = []

        def execute(argv, **kwargs):
            if '-o' in argv and '-MM' not in argv and '-M' not in argv:
                compiles.append(argv)
            return original_run(argv, **kwargs)

        with tempfile.TemporaryDirectory(prefix='rach2-tool-reuse-') as work, \
                tempfile.TemporaryDirectory(prefix='rach2-tool-sdk-') as sdk:
            previous = Path.cwd()
            try:
                os.chdir(work)
                Path('value.h').write_text('#define VALUE 1\n')
                Path(sdk, 'external.h').write_text('#define EXTERNAL 1\n')
                Path('probe.c').write_text('#include "value.h"\n#include <external.h>\n'
                                           'int main(void) { return VALUE + EXTERNAL; }\n')
                compiler = inventory.compiler_record(['cc'])
                flags = ['-std=c99', '-g', '-isystem', sdk]
                args = (['probe'], ['cc'], flags, inventory.compiler_inputs, inventory.digest)
                with patch.object(inventory, 'ROOT', Path(work)), patch.object(subprocess, 'run', side_effect=execute):
                    first = tool_module().prepare(*args, compiler_identity=compiler)
                    target = Path('build/tests/host-suite/tools/probe')
                    marker = Path(str(target) + '.build.json')
                    before = target.stat()
                    second = tool_module().prepare(*args, compiler_identity=compiler)
                    self.assertEqual(first, second)
                    self.assertEqual(len(compiles), 1)
                    self.assertEqual((before.st_ino, before.st_mtime_ns),
                                     (target.stat().st_ino, target.stat().st_mtime_ns))
                    self.assertTrue(any(row['path'] == str(marker) for row in first))
                    Path('value.h').write_text('#define VALUE 2\n')
                    tool_module().prepare(*args, compiler_identity=compiler)
                    self.assertEqual(len(compiles), 2)
                    flags.append('-DSECOND_RECIPE=1')
                    tool_module().prepare(*args, compiler_identity=compiler)
                    self.assertEqual(len(compiles), 3)
                    compiler = dict(compiler, version=compiler['version'] + ' changed')
                    tool_module().prepare(*args, compiler_identity=compiler)
                    self.assertEqual(len(compiles), 4)
                    recorded = json.loads(marker.read_text())['recipe']['compiler_inputs']
                    self.assertTrue(any(row['path'] == str(Path(sdk, 'external.h')) for row in recorded))
                    Path(sdk, 'external.h').write_text('#define EXTERNAL 2\n')
                    tool_module().prepare(*args, compiler_identity=compiler)
                    self.assertEqual(len(compiles), 5)
                    whole = target.read_bytes()
                    target.write_bytes(whole + b'changed')
                    with self.assertRaisesRegex(ValueError, 'bytes changed'):
                        tool_module().prepare(*args, compiler_identity=compiler)
                    self.assertEqual(len(compiles), 5)
                    target.write_bytes(whole)
                    marker.write_text('{}')
                    with self.assertRaisesRegex(ValueError, 'invalid compiled host tool record'):
                        tool_module().prepare(*args, compiler_identity=compiler)
                    self.assertEqual(len(compiles), 5)
            finally:
                os.chdir(previous)

    def test_windows_companion_reuse(self):
        repo = Path(__file__).resolve().parents[2]
        sys.path.insert(0, str(repo / 'tests/cases/host'))
        import prepare as inventory
        windows = inventory.windows
        names = [row[0] for row in windows.companion_recipes()]
        self.assertEqual(names, ['wintarget.exe', 'winstand.exe', 'etude.exe',
                                 'etude-volume.exe', 'image-command.exe'])
        recipes = [(name, [f'program{i}.c'], []) for i, name in enumerate(names)]
        real_run, compiles = subprocess.run, []

        def execute(argv, **kwargs):
            linking = '-o' in argv and '-MM' not in argv and '-M' not in argv
            if linking:
                compiles.append(argv)
            result = real_run(argv, **kwargs)
            if linking:
                # Model the Windows compiler's automatic executable suffix.
                output = Path(argv[argv.index('-o') + 1])
                output.replace(str(output) + '.exe')
            return result

        with tempfile.TemporaryDirectory(prefix='rach2-companion-cache.') as work:
            tree, previous = Path(work).resolve() / 'tree', Path.cwd()
            tree.mkdir()
            try:
                os.chdir(tree)
                Path('include').mkdir()
                Path('include/choice.h').write_text('#define VALUE 0\n')
                sdk = tree.parent / 'sdk'
                sdk.mkdir()
                (sdk / 'external.h').write_text('#define EXTERNAL 0\n')
                Path('program0.c').write_text('#include "choice.h"\n#include <external.h>\n'
                                              'int main(void) { return VALUE + EXTERNAL; }\n')
                for i in range(1, 5):
                    Path(f'program{i}.c').write_text('int main(void) { return 0; }\n')
                schema = Path('tools/etude/schemas/history.schema.json')
                schema.parent.mkdir(parents=True)
                schema.write_text('{}\n')
                flags = ['-std=c99', '-O0', '-Iinclude', '-isystem', str(sdk)]
                identity = inventory.compiler_record(['cc'])
                base = Path('build/tests/host-suite/etude')
                root = base / 'platform'
                programs = [root / name for name in names]

                def prepare(expected, statuses):
                    count = len(compiles)
                    inputs = windows.companions(['cc'], base, inventory.compiler_inputs,
                                                inventory.digest, compiler_identity=identity)
                    self.assertEqual(len(compiles) - count, expected)
                    self.assertEqual([real_run([str(p.resolve())]).returncode for p in programs], statuses)
                    return inputs

                with patch.object(inventory, 'ROOT', tree), \
                        patch.object(windows, 'FLAGS', flags), \
                        patch.object(windows, 'companion_recipes', return_value=recipes), \
                        patch.object(subprocess, 'run', side_effect=execute):
                    self.assertEqual(windows.companions(['cc'], base, inventory.compiler_inputs,
                                                         inventory.digest, print_commands=True), [])
                    self.assertFalse(root.exists())
                    with patch.dict(inventory.tools.SOURCES, {'probe': ['program4.c']}):
                        count = len(compiles)
                        args = (Path('shared'), 'probe', ['cc'], ['-std=c99'],
                                inventory.compiler_inputs, inventory.digest, identity, False)
                        shared = inventory.tools.compiled_tool(*args)
                        self.assertEqual(inventory.tools.compiled_tool(*args), shared)
                        self.assertEqual(len(compiles) - count, 1)
                    first = prepare(5, [0] * 5)
                    self.assertTrue({str(schema), str(root / schema.name)} <=
                                    {row['path'] for row in first})
                    before = [(p.stat().st_ino, p.stat().st_mtime_ns) for p in programs]
                    self.assertEqual(prepare(0, [0] * 5), first)
                    self.assertEqual([(p.stat().st_ino, p.stat().st_mtime_ns) for p in programs], before)
                    self.assertTrue(all(str(p) + '.build.json' in {v['path'] for v in first}
                                        for p in programs))
                    Path('include/choice.h').write_text('#define VALUE 1\n')
                    prepare(1, [1, 0, 0, 0, 0])
                    Path('include/choice.h').write_text('#define VALUE 0\n')
                    Path('choice.h').write_text('#define VALUE 2\n')
                    prepare(1, [2, 0, 0, 0, 0])
                    Path('program1.c').write_text('int main(void) { return 3; }\n')
                    statuses = [2, 3, 0, 0, 0]
                    prepare(1, statuses)
                    flags.append('-DCHANGED_RECIPE=1')
                    prepare(5, statuses)
                    identity = dict(identity, version=identity['version'] + ' changed')
                    prepare(5, statuses)
                    marker = Path(str(programs[0]) + '.build.json')
                    recorded = json.loads(marker.read_text())['recipe']['compiler_inputs']
                    self.assertTrue(any(v['path'] == str(sdk / 'external.h') for v in recorded))
                    (sdk / 'external.h').write_text('#define EXTERNAL 3\n')
                    prepare(1, [5, 3, 0, 0, 0])
                    schema.write_text('{"changed":true}\n')
                    changed = prepare(0, [5, 3, 0, 0, 0])
                    self.assertEqual((root / schema.name).read_bytes(), schema.read_bytes())
                    self.assertNotEqual(changed, first)
                    whole, record = programs[0].read_bytes(), marker.read_bytes()
                    count = len(compiles)
                    programs[0].write_bytes(whole + b'changed')
                    with self.assertRaisesRegex(ValueError, 'bytes changed'):
                        prepare(0, [])
                    self.assertEqual(len(compiles), count)
                    programs[0].write_bytes(whole)
                    marker.write_text('{}')
                    with self.assertRaisesRegex(ValueError, 'invalid compiled host tool record'):
                        prepare(0, [])
                    marker.write_bytes(record)
                    count = len(compiles)
                    Path('program0.c').write_text('int main(void) { invalid C syntax; }\n')
                    with self.assertRaises(subprocess.CalledProcessError):
                        prepare(0, [])
                    self.assertEqual(len(compiles) - count, 1)
                    self.assertEqual(programs[0].read_bytes(), whole)
                    self.assertEqual(marker.read_bytes(), record)
                    self.assertFalse(list(root.glob('*.tmp*')))
            finally:
                os.chdir(previous)

    def test_windows_vectors(self):
        goals = ['build/tests/cases/machine/scenario' + str(i) for i in range(900)]
        base = ['make', '-o' + 'a' * 20000]
        commands = list(product.batches(base, goals))
        self.assertGreater(len(commands), 1)
        self.assertEqual([goal for argv in commands for goal in argv[len(base):]], goals)
        self.assertTrue(all(len(subprocess.list2cmdline(argv)) < 28000 for argv in commands))
        with self.assertRaises(ValueError):
            list(product.batches(['make', 'a' * 29000], goals))

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
