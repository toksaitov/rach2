#!/usr/bin/env python3
"""Check the preparation product's import identities and native build vectors."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import shlex
import struct
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/etude'))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/ci/fixtures'))
selected = [a for a in sys.argv[1:] if not a.startswith('-')]
if not selected or any(a.startswith(('ProducerTests', 'RouteActionTests')) for a in selected):
    from routine_producer_test import ProducerTests
    from routine_route_actions_test import RouteActionTests
    binary = os.environ.get('ETUDE_HOST_PREPARE',
        str(Path(__file__).resolve().parents[2] / 'build/tools/etude/etude'))
    os.environ.setdefault('ETUDE_PRODUCER_TEST_BINARY', binary)
    os.environ.setdefault('ETUDE_ROUTE_TEST_BINARY', binary)


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

    def test_native_exfat_filename(self):
        with tempfile.TemporaryDirectory(prefix='rach2-exfat-record.') as work:
            tree = Path(work)
            self.native_tree(tree)
            root = tree / 'build/tests/host-suite/tools/exfat_external'
            source = tree / 'tests/host/mkexfat_external.sh'
            source.write_text('exit 91\n')
            paths = ['exfat_external.img', 'exfat_external.manifest', 'writer',
                     'exfat_src/hello.txt', 'exfat_src/pattern.bin', 'exfat_src/Sub Dir/inner.txt',
                     'exfat_src/empty', 'exfat_src/e\u00e9t\u00e9.txt',
                     'exfat_src/A name of some length that takes more than one name entry.txt']
            for name in paths:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'fixture bytes\n')
            marker = tree / 'build/tests/host/exfat_writer'
            marker.parent.mkdir(parents=True)
            expected = dict(producer_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                            files={name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                                   for name in paths})
            marker.write_text(json.dumps(expected, sort_keys=True) + '\n')
            original = marker.read_bytes()
            repo = Path(__file__).resolve().parents[2]
            binary = Path(os.environ.get('ETUDE_HOST_PREPARE', str(repo / 'build/tools/etude/etude'))).resolve()
            result = subprocess.run([str(binary), 'prepare', '-H', '--only', 'external-exfat',
                                     'tools', str(marker)], cwd=tree, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(marker.read_bytes(), original)
            name = 'tests/host/e\u00e9t\u00e9.txt'
            (tree / name).write_bytes(b'input bytes\n')
            self.native_source(tree, 'probe_hosttest.c', 'host-inputs: ' + name + '\n')
            record, _ = self.native_prepare(tree, args=['--gnu'])
            inputs = record['checks'][0]['inputs']
            self.assertIn(dict(path_hex=os.fsencode(name).hex(),
                               sha256=hashlib.sha256((tree / name).read_bytes()).hexdigest()), inputs)
            self.assertTrue(all('path' not in row or row['path'].isascii() for row in inputs))

    @staticmethod
    def describe_file(path):
        return dict(sha256=hashlib.sha256(path.read_bytes()).hexdigest(), size_bytes=path.stat().st_size)

    def import_fixture(self, directory, goals=None, checks=None):
        repo = Path(__file__).resolve().parents[2]
        tree, producer = Path(directory) / 'native', Path(directory) / 'producer'
        tree.mkdir()
        producer.mkdir()
        goals = list(goals or ['build/images/root.disk'])
        (tree / 'tests/cases').mkdir(parents=True)
        (tree / '.gitignore').write_text('build/\nvendor/\n')
        (tree / 'source.c').write_text('#include "value.h"\nint main(void) { return VALUE; }\n')
        (tree / 'value.h').write_text('#define VALUE 0\n')
        artifacts = [dict(node=name.removeprefix('build/x86_64/').removeprefix('build/'),
                          per_architecture=name.startswith('build/x86_64/')) for name in goals]
        manifest = dict(cases=[dict(architectures=['i686', 'x86_64'], prepare=artifacts)])
        manifest_path = tree / 'tests/cases/host.host.json'
        manifest_path.write_text(json.dumps(manifest))
        native = 'build/tests/cases/machine/a'
        (tree / 'Makefile').write_text(
            native + ': source.c value.h build/images/root.disk\n'
            '\t@mkdir -p $(@D)\n\t@printf "compile\\n" >> build/compile.calls\n'
            '\t$(CC) -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -o $@ $<\n'
            '.PHONY: build/images/root.disk\nbuild/images/root.disk:\n\t@mkdir -p $(@D)\n\t@printf changed > $@\n')
        files, texts, nested = {}, {}, {}
        compiler = Path(shutil.which('cc')).resolve()
        for name in goals:
            path = producer / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if name.endswith('.inventory.json'):
                inventory = dict(schema_version=1, manifest_sha256=self.describe_file(manifest_path)['sha256'],
                                 compiler=dict(executable=str(compiler), sha256=self.describe_file(compiler)['sha256']),
                                 checks=checks or [])
                text = json.dumps(inventory)
                path.write_text(text)
                texts[name] = text
                fixture = 'inventory-' + str(len(texts)) + '.json'
                (tree / fixture).write_text(text)
                with (tree / 'Makefile').open('a') as stream:
                    stream.write(name + ': ' + fixture + '\n\t@mkdir -p $(@D)\n\tcp $< $@\n')
                for check in checks or []:
                    for row in check['inputs']:
                        relative = os.fsdecode(bytes.fromhex(row['path_hex'])) if 'path_hex' in row else row['path']
                        if Path(relative).is_absolute() or relative.startswith('../') or '/..' in relative:
                            continue
                        item = producer / relative
                        item.parent.mkdir(parents=True, exist_ok=True)
                        item.write_bytes(b'nested fixture bytes\n')
                        nested[relative] = self.describe_file(item)
            else:
                path.write_bytes(b'producer bytes\n')
            files[name] = self.describe_file(path)
        subprocess.run(['git', 'init', '-q', str(tree)], check=True)
        subprocess.run(['git', '-C', str(tree), 'config', 'user.name', 'Preparation fixture'], check=True)
        subprocess.run(['git', '-C', str(tree), 'config', 'user.email', 'fixture@example.invalid'], check=True)
        binary = Path(os.environ.get('ETUDE_HOST_PREPARE', str(repo / 'build/tools/etude/etude'))).resolve()
        state = SimpleNamespace(tree=tree, producer=producer, binary=binary, goals=goals, files=files,
                                texts=texts, nested=nested, count=0)
        self.import_source(state)
        state.record = 'etude-prepare 1\ntree /fixture-producer\n' + ''.join(
            'file {sha256} {size_bytes} 1 0 '.format(**value) + name + '\n'
            for name, value in files.items())
        state.receipt = dict(format='etude-preparation-receipt', schema_version=1,
                             outcome='pass', cleanup='confirmed', commit=state.commit,
                             run_id='fixture-ordinary-production', machine_id='fixture-machine', boot_id='fixture-boot',
                             record_sha256=hashlib.sha256(state.record.encode()).hexdigest(),
                             start=dict(utc='2026-10-24T00:00:00Z', monotonic_us=1, resolution_us=1),
                             end=dict(utc='2026-10-24T00:00:01Z', monotonic_us=5, resolution_us=1),
                             conditioning=dict(inhouse='clean', backend='ordinary', provenance=None))
        state.metadata = dict(format='etude-preparation-producer', schema_version=1, commit=state.commit,
                              source=state.source, record=state.record, receipt=state.receipt,
                              validator=dict(path=str(binary), **self.describe_file(binary)),
                              auxiliary={}, nested=nested, inventories=texts)
        return state

    def import_source(self, state):
        subprocess.run(['git', '-C', str(state.tree), 'add', '.'], check=True)
        subprocess.run(['git', '-C', str(state.tree), 'commit', '-qm', 'Fixture source',
                        '--allow-empty'], check=True)
        state.commit = subprocess.check_output(['git', '-C', str(state.tree), 'rev-parse', 'HEAD'],
                                               text=True).strip()
        snapshot = state.tree.parent / ('source-' + state.commit + '.json')
        subprocess.run([str(state.binary), 'prepare', 'source', '--tree', str(state.tree), '--commit',
                        state.commit, '--output', str(snapshot)], check=True, stdout=subprocess.PIPE)
        state.source = {row['path']: {key: value for key, value in row.items() if key != 'path'}
                        for row in json.loads(snapshot.read_text())['inputs']}
        if hasattr(state, 'metadata'):
            state.metadata.update(commit=state.commit, source=state.source)
            state.receipt['commit'] = state.commit

    def import_run(self, state, expected=0, conditioning='retained', producer=None, env=None, arches='i686', newline='\n'):
        state.count += 1
        output = state.tree / 'build/import-proof' / str(state.count)
        work = output / 'work'
        work.mkdir(parents=True)
        metadata = state.producer / 'producer.json'
        metadata.write_text(json.dumps(state.metadata))
        config = dict(format='etude-preparation-import', schema_version=1, commit=state.commit,
                      producer=dict(transport='local', tree=str(state.producer), metadata=str(metadata),
                                    sha256=self.describe_file(metadata)['sha256']))
        if producer:
            config['producer'].update(producer)
        (output / 'config.json').write_text(json.dumps(config))
        request = output / 'request'
        request.write_bytes((newline.join(state.goals) + newline).encode())
        provenance = output / 'provenance.json'
        actual = dict(os.environ, CC=str(Path(shutil.which('cc')).resolve()),
                      MAKEFLAGS='', MFLAGS='', MAKELEVEL='')
        actual.update(env or {})
        result = subprocess.run(
            [str(state.binary), 'prepare', 'import', '--tree', str(state.tree),
             '--config', str(output / 'config.json'), '--request', str(request), '--work', str(work),
             '--provenance', str(provenance), '--commit', state.commit, '--arches', arches,
             '--conditioning', conditioning], env=actual, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, expected, result.stdout)
        self.assertEqual(provenance.exists(), expected == 0)
        return json.loads(provenance.read_text()) if not expected else None

    def test_request_lines(self):
        with tempfile.TemporaryDirectory(prefix='rach2-request-lines.') as work:
            state = self.import_fixture(work)
            self.import_run(state, newline='\r\n')
            original = state.goals[:]
            state.goals = [original[0].replace('images', 'ima\rges')]
            self.import_run(state, expected=1)

    def test_clean_goal_conditioning(self):
        with tempfile.TemporaryDirectory(prefix='rach2-clean-import.') as work:
            state = self.import_fixture(work)
            original = dict(state.receipt)
            for regime in ('clean', 'retained', 'unknown', None):
                state.receipt.clear()
                state.receipt.update(original)
                state.receipt.pop('conditioning', None)
                if regime:
                    state.receipt['conditioning'] = dict(inhouse=regime, backend='ordinary', provenance=None)
                before = json.dumps(state.receipt, sort_keys=True)
                self.import_run(state, expected=0 if regime == 'clean' else 1, conditioning='clean')
                self.import_run(state)
                self.assertEqual(json.dumps(state.receipt, sort_keys=True), before)
            state.receipt['conditioning'] = dict(inhouse='clean', backend='import', provenance={'sha256': 'fixture'})
            self.import_run(state, expected=1, conditioning='clean')

    def test_roles(self):
        portable = ['build/tests/cases/machine/install.disk', 'build/x86_64/tests/cases/machine/install.disk',
                    'build/tests/cases/machine/test-password', 'build/tests/cases/host/fstool',
                    'build/tests/host-suite/schemas/case.schema.json', 'build/tests/hello']
        native = ['build/tests/cases/machine/disk', 'build/tools/etude/etude', 'build/krffs/readfile.krffs',
                  'build/tests/host-suite/scripts/kstack', 'build/tests/host-suite/sanitizer/tools.json']
        with tempfile.TemporaryDirectory(prefix='rach2-import-roles.') as work:
            state = self.import_fixture(work, portable + native)
            with (state.tree / 'Makefile').open('a') as stream:
                stream.write('%:\n\t@mkdir -p $(@D)\n\tcp /usr/bin/true $@\n')
                name = native[-1]
                stream.write('.PHONY: ' + name + '\n' + name + ':\n\t@mkdir -p $(@D)\n\t' +
                             str(state.binary) + ' prepare -H --only sanitizer tools $(@D)\n')
            self.import_source(state)
            proof = self.import_run(state)
            self.assertEqual(proof['roles'], dict.fromkeys(portable, 'portable') | dict.fromkeys(native, 'native'))

    def test_sanitizer_file_goal(self):
        repo = Path(__file__).resolve().parents[2]
        name = 'build/tests/host-suite/sanitizer/tools.json'
        declarations = [json.loads(path.read_text()) for path in (repo / 'tests/cases').glob('*.json')]
        self.assertTrue(any(a['node'] == 'tests/host-suite/sanitizer/tools.json'
                            for document in declarations for case in document['cases']
                            for a in case.get('prepare', [])))
        with tempfile.TemporaryDirectory(prefix='rach2-sanitizer-goal.') as work:
            state = self.import_fixture(work, [name])
            with (state.tree / 'Makefile').open('a') as stream:
                stream.write('.PHONY: ' + name + '\n' + name + ':\n\t@mkdir -p $(@D)\n\t' +
                             str(state.binary) + ' prepare -H --only sanitizer tools $(@D)\n')
            self.import_source(state)
            proof = self.import_run(state)
            self.assertEqual(proof['artifacts'][name], self.describe_file(state.tree / name))
            manifest = json.loads((state.tree / name).read_text())
            manifest['platform'] = 'foreign'
            (state.tree / name).write_text(json.dumps(manifest))
            with (state.tree / 'Makefile').open('a') as stream:
                stream.write(name + ': ; @true\n')
            self.import_source(state)
            self.import_run(state, expected=1)

    def test_sanitizer_source_binding(self):
        name = 'build/tests/host-suite/sanitizer/tools.json'
        with tempfile.TemporaryDirectory(prefix='rach2-sanitizer-binding.') as work:
            state = self.import_fixture(work, [name])
            (state.tree / name).parent.mkdir(parents=True)
            subprocess.run([str(state.binary), 'prepare', '-H', '--only', 'sanitizer',
                            'tools', str((state.tree / name).parent)], cwd=state.tree, check=True)
            with (state.tree / 'Makefile').open('a') as stream:
                stream.write(name + ': ; @true\n')
            self.import_source(state)
            manifest = json.loads((state.tree / name).read_text())
            self.import_run(state)
            for rows in (manifest['inputs'] * 2, [dict(path='../elsewhere', sha256='a' * 64)]):
                (state.tree / name).write_text(json.dumps(dict(manifest, inputs=rows)))
                self.import_run(state, expected=1)
            (state.tree / name).write_text(json.dumps(manifest))
            if sys.platform == 'darwin':
                snapshot = (state.tree / name).parent / 'atos'
                before = snapshot.read_bytes()
                snapshot.write_bytes(before + b'changed')
                self.import_run(state, expected=1)
                snapshot.write_bytes(before)
                proof = Path(str(snapshot) + '.provenance.json')
                previous = proof.read_bytes()
                value = json.loads(previous)
                value['source'] = str(Path('/usr/bin/true').resolve())
                proof.write_text(json.dumps(value))
                self.import_run(state, expected=1)
                proof.write_bytes(previous)
                snapshot.unlink()
                snapshot.symlink_to('/usr/bin/atos')
                self.import_run(state, expected=1)

    def test_record_identity(self):
        with tempfile.TemporaryDirectory(prefix='rach2-record-identity.') as work:
            state = self.import_fixture(work)
            good = state.record
            line = good.splitlines()[-1] + '\n'
            self.import_run(state)
            for bad in (good + line, good.replace('build/images/root.disk', '../root'),
                        good.replace(state.files[state.goals[0]]['sha256'], 'z' * 64),
                        'etude-prepare 2\n', good.replace(' 15 1 0 ', ' 99999999999999999999999 1 0 ')):
                state.metadata['record'] = bad
                self.import_run(state, expected=1)
            state.metadata['record'] = good

    def test_receipt_binding(self):
        with tempfile.TemporaryDirectory(prefix='rach2-receipt-binding.') as work:
            state = self.import_fixture(work)
            original = dict(state.receipt)
            self.import_run(state)
            for key, value in [('cleanup', 'unknown'), ('outcome', 'fail'), ('commit', '2' * 40),
                               ('record_sha256', '0' * 64), ('end', dict(state.receipt['end'], monotonic_us=0)),
                               ('run_id', ''), ('boot_id', 'other boot'),
                               ('start', dict(state.receipt['start'], resolution_us=0))]:
                state.receipt.clear()
                state.receipt.update(original)
                state.receipt[key] = value
                self.import_run(state, expected=1)

    def test_changed_file(self):
        with tempfile.TemporaryDirectory(prefix='rach2-changed-import.') as work:
            state = self.import_fixture(work)
            self.import_run(state)
            path = state.producer / state.goals[0]
            path.write_bytes(b'different bytes\n')
            self.import_run(state, expected=1)
            path.unlink()
            path.symlink_to('/dev/null')
            self.import_run(state, expected=1)

    def test_inventory_chain(self):
        path = 'build/tests/host-suite/tests.inventory.json'
        nested = 'build/tests/host-suite/tools/exfat_external/exfat_src/e\u00e9t\u00e9.txt'
        digest = hashlib.sha256(b'nested fixture bytes\n').hexdigest()
        checks = [dict(assertion_id='exfat', backend='gnu',
                       inputs=[dict(path_hex=os.fsencode(nested).hex(), sha256=digest)])]
        with tempfile.TemporaryDirectory(prefix='rach2-inventory-chain.') as work:
            state = self.import_fixture(work, [path], checks)
            proof = self.import_run(state)
            self.assertEqual(proof['roles'][path], 'inventory')
            self.assertEqual(proof['imported'][nested], state.nested[nested])
            self.assertEqual((state.tree / nested).read_bytes(), b'nested fixture bytes\n')
            original = state.metadata['inventories'][path]
            state.metadata['inventories'][path] = original + ' '
            self.import_run(state, expected=1)
            changed = json.loads(original)
            changed['manifest_sha256'] = '0' * 64
            state.metadata['inventories'][path] = json.dumps(changed)
            self.import_run(state, expected=1)

    def test_alignment_inventory_chain(self):
        prefix = 'vendor/e2fsprogs/e2fsprogs-1.47.4/lib/'
        other = 'build/x86_64/vendor-work/e2fsprogs/e2fsprogs-1.47.4/lib/'
        inputs = [dict(path=root + name, sha256=hashlib.sha256(b'nested fixture bytes\n').hexdigest())
                  for root in (prefix, other) for name in
                  ('ext2fs/unix_io.c', 'ext2fs/ext2_types.h', 'ext2fs/ext2fs.h', 'et/com_err.h')]
        checks = [dict(assertion_id=name, backend='gnu', inputs=inputs)
                  for name in ('e2fsprogs_alignment', 'e2fsprogs_alignment64')]
        with tempfile.TemporaryDirectory(prefix='rach2-alignment-chain.') as work:
            path = 'build/tests/host-suite/tests.inventory.json'
            state = self.import_fixture(work, [path], checks)
            proof = self.import_run(state)
            self.assertTrue({row['path'] for row in inputs} <= proof['imported'].keys())
            original = state.metadata['inventories'][path]
            for rows in ([dict(path='../escape.h', sha256='a' * 64)],
                         [dict(path=prefix + '../escape.h', sha256='a' * 64)],
                         [dict(path=prefix + 'ext2fs/ext2fs.h', sha256='bad')],
                         [dict(path=prefix + 'ext2fs/ext2fs.h', sha256=sha) for sha in ('a' * 64, 'b' * 64)]):
                document = json.loads(original)
                document['checks'][0]['inputs'] = rows
                text = json.dumps(document)
                state.metadata['inventories'][path] = text
                state.metadata['record'] = state.record.replace(
                    '{sha256} {size_bytes}'.format(**state.files[path]),
                    hashlib.sha256(text.encode()).hexdigest() + ' ' + str(len(text)))
                state.receipt['record_sha256'] = hashlib.sha256(state.metadata['record'].encode()).hexdigest()
                self.import_run(state, expected=1)

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
        with tempfile.TemporaryDirectory(prefix='rach2-native-argv.') as work:
            state = self.import_fixture(work, goals)
            with (state.tree / 'Makefile').open('a') as stream:
                stream.write('%:\n\t@mkdir -p $(@D)\n\tcp /usr/bin/true $@\n')
            self.import_source(state)
            proof = self.import_run(state)
            commands = proof['native_build']
            self.assertGreater(len(commands), 1)
            selected = [arg for command in commands for arg in command if arg in goals]
            self.assertEqual(selected, sorted(goals))
            self.assertTrue(all(len(subprocess.list2cmdline(command)) < 28000 for command in commands))
        portable = ['build/images/' + str(i) + '-' + 'a' * 220 + '.disk' for i in range(130)]
        with tempfile.TemporaryDirectory(prefix='rach2-frozen-argv.') as work:
            state = self.import_fixture(work, portable + ['build/tests/cases/machine/a'])
            self.import_run(state, expected=1)
            self.assertFalse((state.tree / 'build/compile.calls').exists())

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
                    digest = self.describe_file(image)['sha256']
                    native.root_image(tree, image, 'krffs', 1, text, os.environ.copy(), work, tools)
                    self.assertEqual(self.describe_file(image)['sha256'], digest)
                for line in (f'file 0444 {native.ROOT_PADDING} {payload}\n',
                             f'dir 0755 {native.ROOT_PADDING}\n'):
                    with self.assertRaisesRegex(ValueError, 'native root reserves'):
                        native.root_image(tree, image, 'krffs', 1, line, os.environ.copy(), work, tools)
                payload.write_bytes(bytes(10 * 1048576 - 3 * 357))
                with self.assertRaisesRegex(ValueError, 'insufficient free space'):
                    native.root_image(tree, image, 'krffs', 1, text, os.environ.copy(), work, tools)

    def test_native_compilation(self):
        goals = ['build/images/root.disk', 'build/tests/cases/machine/a']
        with tempfile.TemporaryDirectory(prefix='rach2-native-build.') as work:
            state = self.import_fixture(work, goals)
            target = state.tree / goals[1]
            target.parent.mkdir(parents=True)
            target.write_bytes(b'old foreign executable')
            future = max((state.tree / name).stat().st_mtime_ns for name in ('source.c', 'value.h')) + 2000000000
            os.utime(target, ns=(future, future))
            self.import_run(state)
            self.assertNotEqual(target.read_bytes(), b'old foreign executable')
            before = target.stat()
            self.import_run(state)
            self.assertEqual((before.st_ino, before.st_mtime_ns), (target.stat().st_ino, target.stat().st_mtime_ns))
            for name, text, status in [('value.h', '#define VALUE 1\n', 1),
                                      ('source.c', '#include "value.h"\nint main(void) { return VALUE + 1; }\n', 2)]:
                path = state.tree / name
                path.write_text(text)
                newer = target.stat().st_mtime_ns + 2000000000
                os.utime(path, ns=(newer, newer))
                self.import_source(state)
                self.import_run(state)
                self.assertEqual(subprocess.run([str(target)]).returncode, status)
            self.assertEqual((state.tree / 'build/compile.calls').read_text().splitlines(), ['compile'] * 3)
            self.assertEqual((state.tree / goals[0]).read_bytes(), b'producer bytes\n')

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
        with tempfile.TemporaryDirectory(prefix='rach2-idle-producer.') as work:
            state = self.import_fixture(work)
            self.import_run(state)
            self.import_run(state, expected=1, producer=dict(tree=str(state.tree)))
            alias = state.tree.parent / 'alias'
            alias.symlink_to(state.tree, target_is_directory=True)
            self.import_run(state, expected=1, producer=dict(tree=str(alias)))
            (state.producer / 'build/.run.lock').symlink_to(state.producer / 'absent')
            self.import_run(state, expected=1)

    def test_refused_wsl_transports(self):
        with tempfile.TemporaryDirectory(prefix='rach2-wsl-refusal.') as work:
            state = self.import_fixture(work)
            for mode in ('wsl', 'other'):
                self.import_run(state, expected=1, producer=dict(transport=mode))

    def import_remote(self, state, transport):
        directory = state.tree.parent / 'commands'
        directory.mkdir(exist_ok=True)
        log = directory / 'calls.jsonl'
        common = 'import hashlib,json,pathlib,shlex,shutil,sys\n'
        common += 'log=pathlib.Path(' + repr(str(log)) + ')\n'
        common += 'with log.open("a") as out:out.write(json.dumps([pathlib.Path(sys.argv[0]).name,*sys.argv[1:]])+"\\n")\n'
        metadata = state.producer / 'producer.json'
        ssh = common + 'words=shlex.split(sys.argv[-1])\n'
        ssh += 'if words[0]=="env":\n'
        ssh += ' assert words[:8]==["env","MSYS_NO_PATHCONV=1","wsl.exe","--distribution","Ubuntu-26.04","--user","testing","--exec"]\n'
        ssh += ' words=words[8:]\n'
        ssh += 'if words==["/immutable/etude","lock","tree","/producer","prepare-import","/bin/cat","/metadata"]:\n'
        ssh += ' sys.stdout.buffer.write(pathlib.Path(' + repr(str(metadata)) + ').read_bytes())\n'
        ssh += 'elif words==["/usr/bin/sha256sum","/immutable/etude"]:\n'
        ssh += ' print(hashlib.sha256(pathlib.Path(' + repr(str(state.binary)) + ').read_bytes()).hexdigest()+"  /immutable/etude")\n'
        ssh += 'else:raise SystemExit(91)\n'
        rsync = common + 'assert sys.argv.count("--compress")==1 and "--sparse" in sys.argv and "--protect-args" in sys.argv\n'
        rsync += 'server=shlex.split(next(a.split("=",1)[1] for a in sys.argv if a.startswith("--rsync-path=")))\n'
        rsync += 'assert server[-6:]==["/immutable/etude","lock","tree","/producer","prepare-import","/usr/bin/rsync"]\n'
        rsync += 'assert sys.argv[-2]=="fixture:/producer/"\n'
        rsync += 'listing=pathlib.Path(next(a.split("=",1)[1] for a in sys.argv if a.startswith("--files-from=")))\n'
        rsync += 'destination=pathlib.Path(sys.argv[-1])\n'
        rsync += 'for name in listing.read_text().splitlines():\n'
        rsync += ' source=pathlib.Path(' + repr(str(state.producer)) + ')/name\n'
        rsync += ' target=destination/name\n target.parent.mkdir(parents=True,exist_ok=True)\n shutil.copy2(source,target)\n'
        for name, text in [('ssh', ssh), ('rsync', rsync)]:
            path = directory / name
            path.write_text('#!' + sys.executable + '\n' + text)
            path.chmod(0o755)
        state.metadata['validator']['path'] = '/immutable/etude'
        fields = dict(transport=transport, host='fixture', etude='/immutable/etude',
                      tree='/producer', metadata='/metadata')
        if transport == 'wsl-ssh':
            fields.update(distribution='Ubuntu-26.04', user='testing')
        return fields, dict(PATH=str(directory) + os.pathsep + os.environ['PATH']), log

    def test_posix_wsl_ssh(self):
        with tempfile.TemporaryDirectory(prefix='rach2-wsl-command.') as work:
            state = self.import_fixture(work)
            producer, env, log = self.import_remote(state, 'wsl-ssh')
            self.import_run(state, producer=producer, env=env)
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertEqual([row[0] for row in calls], ['ssh', 'ssh', 'rsync'])
            query = shlex.split(calls[0][-1])
            self.assertEqual(query[:8], ['env', 'MSYS_NO_PATHCONV=1', 'wsl.exe', '--distribution',
                                        'Ubuntu-26.04', '--user', 'testing', '--exec'])
            for key in ('distribution', 'user'):
                for value in ('', '-bad', 'bad value', 'bad;command'):
                    self.import_run(state, expected=1, producer=dict(producer, **{key: value}), env=env)
            self.assertEqual(len(log.read_text().splitlines()), 3)

    def test_ssh_compression(self):
        with tempfile.TemporaryDirectory(prefix='rach2-ssh-command.') as work:
            state = self.import_fixture(work)
            producer, env, log = self.import_remote(state, 'ssh')
            self.import_run(state, producer=producer, env=env)
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            argv = calls[-1]
            self.assertEqual(argv.count('--compress'), 1)
            self.assertIn('--sparse', argv)
            server = next(word.split('=', 1)[1] for word in argv if word.startswith('--rsync-path='))
            self.assertEqual(shlex.split(server), ['/immutable/etude', 'lock', 'tree',
                                                  '/producer', 'prepare-import', '/usr/bin/rsync'])
            self.assertEqual(argv[-3:], ['--', 'fixture:/producer/', str(state.tree) + '/'])
            state.metadata['validator']['path'] = '/different/etude'
            self.import_run(state, expected=1, producer=producer, env=env)

    def test_native_local_copy(self):
        with tempfile.TemporaryDirectory(prefix='rach2-local-copy.') as work:
            state = self.import_fixture(work)
            proof = self.import_run(state)
            self.assertEqual(proof['imported'][state.goals[0]], state.files[state.goals[0]])
            self.assertNotEqual((state.tree / state.goals[0]).stat().st_ino,
                                (state.producer / state.goals[0]).stat().st_ino)
            self.assertEqual((state.tree / state.goals[0]).read_bytes(), (state.producer / state.goals[0]).read_bytes())

    def test_encoded_inputs(self):
        path = 'build/tests/host-suite/tests.inventory.json'
        for item in (dict(path='/outside'), dict(path_hex='0'), dict(path_hex='XX'),
                     dict(path_hex='00'), dict(path_hex='61', path='a'), dict(path='../escape')):
            with tempfile.TemporaryDirectory(prefix='rach2-encoded-input.') as work:
                checks = [dict(assertion_id='exfat', backend='gnu', inputs=[])]
                state = self.import_fixture(work, [path], checks)
                document = json.loads(state.metadata['inventories'][path])
                document['checks'][0]['inputs'] = [dict(item, sha256='a' * 64)]
                text = json.dumps(document)
                state.metadata['inventories'][path] = text
                state.metadata['record'] = state.record.replace(
                    '{sha256} {size_bytes}'.format(**state.files[path]),
                    hashlib.sha256(text.encode()).hexdigest() + ' ' + str(len(text)))
                state.receipt['record_sha256'] = hashlib.sha256(state.metadata['record'].encode()).hexdigest()
                self.import_run(state, expected=1)

    def test_actual_architecture_buckets(self):
        goals = ['build/tests/cases/machine/a', 'build/x86_64/tests/cases/machine/b',
                 'build/tests/host-suite/tests.inventory.json']
        with tempfile.TemporaryDirectory(prefix='rach2-architecture-buckets.') as work:
            state = self.import_fixture(work, goals)
            with (state.tree / 'Makefile').open('a') as stream:
                stream.write(goals[1] + ': source.c value.h\n\t@mkdir -p $(@D)\n\t$(CC) -o $@ $<\n')
            self.import_source(state)
            proof = self.import_run(state)
            self.assertEqual([next(a for a in argv if a.startswith('ARCH=')) for argv in proof['native_build']],
                             ['ARCH=i686', 'ARCH=x86_64', 'ARCH=i686'])
            self.assertTrue(all('-B' not in argv for argv in proof['native_build']))
            proof = self.import_run(state, arches='x86_64')
            self.assertTrue(all('ARCH=x86_64' in argv for argv in proof['native_build']))



if __name__ == '__main__':
    signal.signal(signal.SIGTERM, lambda signum, frame: sys.exit(128 + signum))
    unittest.main()
