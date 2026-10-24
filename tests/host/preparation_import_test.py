#!/usr/bin/env python3
"""Check the preparation product's import identities and native build vectors."""
import hashlib
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/etude'))
import preparation_import as product
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/ci/fixtures'))
from routine_route_test import RouteTests


class ImportTests(unittest.TestCase):
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
                      'build/krffs/readfile.krffs', 'build/tests/host-suite/scripts/kstack'):
            self.assertEqual(product.role(value), 'native', value)
        self.assertEqual(product.role('build/tests/host-suite/tests.inventory.json'), 'inventory')

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
            (tree / 'source.c').write_text('int main(void) { return 0; }\n')
            (tree / 'Makefile').write_text(
                'build/tests/cases/machine/a: source.c\n'
                '\tmkdir -p $(@D)\n\t$(CC) -o $@ $<\n')
            target = tree / 'build/tests/cases/machine/a'
            target.parent.mkdir(parents=True)
            target.write_bytes(b'old foreign executable')
            roles = {'build/tests/cases/machine/a': 'native'}
            with patch.dict(os.environ, {'MAKEFLAGS': '', 'MFLAGS': '', 'MAKELEVEL': ''}):
                commands = product.build_native(tree, roles, [])
            self.assertEqual(len(commands), 1)
            product.validate_native(tree, roles)
            self.assertNotEqual(target.read_bytes(), b'old foreign executable')

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
        self.assertTrue(all('-B' in argv for argv in commands[:2]))
        self.assertNotIn('-B', commands[-1])
        with patch.object(product, 'run'):
            commands = product.build_native(Path('.'), roles, [], 'x86_64')
        self.assertTrue(all('ARCH=x86_64' in argv for argv in commands))


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, lambda signum, frame: sys.exit(128 + signum))
    unittest.main()
