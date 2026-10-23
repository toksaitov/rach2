#!/usr/bin/env python3
"""Build the native host checks and publish their immutable execution inventory.

This is a build tool. It never executes a test, checker selftest or fuzz input.
The native host case consumes the resulting JSON and runs prepared binaries.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import fixtures
import windows
import tools
import routing
import boot_inputs

ROOT = Path(__file__).resolve().parents[3]
GROUPS = ('tests', 'etude', 'boots', 'checkers', 'fuzz', 'scripts')
CHECKERS = ('tools/inspect/elfcheck.c', 'tools/inspect/cfgcheck.c',
            'tools/inspect/setidcheck.c', 'tools/verify/prosecheck.c',
            'tools/etude/win/pecheck.c')
COMMON = ['-std=c99', '-D_DEFAULT_SOURCE', '-D_FORTIFY_SOURCE=0',
          '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g', '-O1']
INCLUDES = ['rach2/sys', 'rach2/sys/arch/x86', 'rach2/sys/arch/i686',
            'rach2/sys/lib', 'rach2/sys/proc', 'utils', 'krffs', 'tests/host/compat']


def digest(path):
    """Hash a complete input or output of preparation."""
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def metadata(path, tag, arch):
    """Read repeated source metadata without silently dropping a wrapped list."""
    lines = path.read_text().splitlines()
    words = []
    for i, line in enumerate(lines):
        found = re.search(re.escape(tag) + r':([^*]*)', line)
        if not found:
            continue
        if i + 1 < len(lines) and re.match(r'\s*\*\s+[\w./-]+\.[ch](?:\s|$)', lines[i + 1]):
            raise ValueError(f'{path}: repeat {tag}: on a continued list')
        words.extend(shlex.split(found[1]))
    if arch == 'x86_64':
        words = [('build/x86_64/vendor-work/' + w[7:] if w.startswith('vendor/') else w)
                 for w in words]
    return words


def group_of(path):
    text = path.read_text()
    return 'boots' if 'host-arch:' in text else 'etude' if path.name.startswith('etude_') else 'tests'


def entry(identity, label, program, sources, arguments=(), alone=False):
    return dict(assertion_id=identity, label=label, mode='alone' if alone else 'shared',
                program=program, arguments=list(arguments), sha256=None, inputs=[],
                sources=sources, unsupported=None)


def checks(group, arch, base):
    """List every check, including entries still awaiting native adapters."""
    if group in ('tests', 'etude', 'boots'):
        paths = [p for p in sorted(Path('tests/host').glob('*_hosttest.c')) if group_of(p) == group]
        paths.sort(key=lambda p: ('host-alone:' in p.read_text(),
                                  'host-first:' not in p.read_text(), p.name))
        for path in paths:
            identity = path.stem.removesuffix('_hosttest')
            if identity == 'etude_platform' and routing.windows_host():
                yield windows.platform_entry(base, entry)
                continue
            deps = metadata(path, 'host-deps', arch)
            flags = metadata(path, 'host-cflags', arch)
            required_tools = metadata(path, 'host-tools', arch)
            out = str(base / path.stem)
            item = entry(identity, path.stem, out, [str(path), *deps],
                         alone='host-alone:' in path.read_text())
            item['command'] = [*fixtures.platform_flags(COMMON), '-DRACH2_HOSTTEST', '-Wno-empty-body', '-ffreestanding',
                               *[w for d in INCLUDES for w in ('-I', d)], *flags,
                               '-o', out, str(path), *deps]
            if routing.windows_host() and identity in routing.BOOTS:
                item = windows.boot_entry(item, flags)
            if group == 'boots':
                item['data'] = [*item.get('data', []), *boot_inputs.files(identity, arch)]
            item['tools'] = required_tools
            item.setdefault('data', []).extend(metadata(path, 'host-inputs', arch))
            yield item
    elif group == 'checkers':
        for source in CHECKERS:
            name = Path(source).stem
            out = str(base / (name + '_selftest'))
            item = entry(name, name + ' selftest under the sanitizers', out, [source], ['--selftest'])
            item['command'] = [*COMMON, '-Wall', '-Wextra', '-Werror', '-o', out, source]
            yield item
    elif group == 'fuzz':
        for source in sorted(Path('tests/fuzz').glob('*_fuzz.c')):
            name = source.stem.removesuffix('_fuzz')
            corpus = sorted(p for p in Path('tests/fuzz/corpus', name).rglob('*') if p.is_file())
            out = str(base / name)
            deps = metadata(source, 'fuzz-deps', arch)
            item = entry(name, name + ' corpus replayed', out, [str(source), *deps], map(str, corpus))
            if not corpus:
                raise ValueError(f'{name}: the fuzz harness has no corpus')
            item['command'] = [*fixtures.platform_flags(COMMON), '-DRACH2_HOSTTEST', '-DFIXTURE_NO_MAIN', '-DFUZZ_REPLAY',
                               '-Wno-empty-body', '-ffreestanding',
                               *[w for d in INCLUDES for w in ('-I', d)],
                               *metadata(source, 'fuzz-cflags', arch), '-o', out, str(source), *deps]
            item['data'] = list(map(str, corpus))
            item['corpus'] = str(Path('tests/fuzz/corpus', name))
            yield item
    else:
        adapters_path = Path('tests/cases/host/adapters.json')
        adapters = json.loads(adapters_path.read_text())
        for line in Path('tests/cases/host/host.list').read_text().splitlines():
            if not line or line.startswith('#'):
                continue
            name, label, old = line.split('|', 2)
            if name in fixtures.NAMES:
                command, sources, origins, program = fixtures.recipe(name, base, COMMON)
                item = entry(name, label, program, sources)
                item['command'] = command
                item['data'] = [*origins, 'tests/cases/host/host.list', str(adapters_path)]
            else:
                adapter = adapters.get(name)
                item = entry(name, label, adapter['program'] if adapter else None, [],
                             adapter.get('arguments', []) if adapter else [])
                item['command'] = None
                item['inputs'] = [dict(path='tests/cases/host/host.list',
                                      sha256=digest(Path('tests/cases/host/host.list'))),
                                  dict(path=str(adapters_path), sha256=digest(adapters_path))]
                if not adapter:
                    item['unsupported'] = f'no native adapter for the behavioral check: {old}'
            yield item


def valid_previous(old, item, compiler):
    """Reuse a prepared binary only while its compiler, inputs and bytes match."""
    if not old or old.get('command') != [*compiler, *item['command']]:
        return False
    try:
        return old['sha256'] == digest(Path(item['program'])) and all(
            digest(Path(value['path'])) == value['sha256'] for value in old['inputs'])
    except (KeyError, OSError):
        return False


def compiler_inputs(compiler, command, sources):
    """Scan each translation unit; a shared -MF would retain only the last."""
    flags = []
    skip = False
    for word in command:
        if skip:
            skip = False
        elif word == '-o':
            skip = True
        elif word not in sources and word != '-c':
            flags.append(word)
    dependencies = set()
    for source in sources:
        if not source.endswith('.c'):
            continue
        result = subprocess.run([*compiler, *flags, '-MM', '-MT', 'host-input', source],
                                check=True, text=True, capture_output=True)
        text = result.stdout.replace('\\\n', ' ')
        dependencies.update(shlex.split(text.split(':', 1)[1]))
    return dependencies


def prepare(args):
    os.chdir(ROOT)
    base = Path(args.output).parent / args.group
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    if not compiler:
        raise ValueError('CC names no compiler')
    if not args.print_commands:
        base.mkdir(parents=True, exist_ok=True)
    previous = {}
    try:
        previous = {x['assertion_id']: x for x in json.loads(Path(args.output).read_text())['checks']}
    except (OSError, KeyError, ValueError):
        pass
    inventory = []
    for item in checks(args.group, args.arch, base):
        if args.only and item['assertion_id'] != args.only:
            continue
        command = item.pop('command')
        item['instrumentation'] = sorted({name for flag in command or []
                                           if flag.startswith('-fsanitize=')
                                           for name in flag.split('=', 1)[1].split(',')})
        backend = routing.backend(item['assertion_id'], item['sources'], args.group)
        if args.gnu and item['assertion_id'] not in routing.BOOTS | {'etude_platform'}:
            backend = 'gnu'
        item['backend'] = backend
        windows_fixture = item.pop('windows_fixture', False)
        if backend == 'gnu' and not args.gnu:
            item['program'] = None
            item['instrumentation'] = None
            item['arguments'] = []
            item['sha256'] = None
            item['inputs'] = [dict(path=p, sha256=digest(Path(p))) for p in item['sources']
                              if Path(p).is_file()]
            item.pop('tools', None)
            item.pop('sources', None)
            item.pop('data', None)
            item['command'] = None
            inventory.append(item)
            continue
        if backend == 'native' and args.gnu:
            item['program'] = None
            item['instrumentation'] = None
            item['command'] = None
            item['unsupported'] = 'this assertion executes on the collection host'
            item.pop('tools', None)
            item.pop('sources', None)
            item.pop('data', None)
            inventory.append(item)
            continue
        tool_inputs = tools.prepare(item.pop('tools', []), compiler, fixtures.platform_flags(COMMON), compiler_inputs,
                                   digest, args.print_commands)
        companions = windows.companions(compiler, base, compiler_inputs, digest,
                                        args.print_commands) if windows_fixture else []
        if args.print_commands:
            if command:
                print(shlex.join([*compiler, *command]))
            continue
        if command:
            probe = dict(item, command=command)
            old = previous.get(item['assertion_id'])
            if valid_previous(old, probe, compiler):
                item['sha256'], item['inputs'] = old['sha256'], old['inputs']
                data_inputs = {value['path']: value for value in item['inputs']}
                for data in item.get('data', []):
                    data_inputs[data] = dict(path=data, sha256=digest(Path(data)))
                item['inputs'] = [data_inputs[key] for key in sorted(data_inputs)]
            else:
                for source in item['sources']:
                    if not Path(source).is_file():
                        raise ValueError(f"{item['assertion_id']}: missing source {source}")
                full = [*compiler, *command]
                print('host prepare: ' + shlex.join(full), flush=True)
                subprocess.run(full, check=True)
                program = Path(item['program'])
                if not program.exists() and Path(str(program) + '.exe').exists():
                    Path(str(program) + '.exe').replace(program)
                item['sha256'] = digest(program)
                dependencies = compiler_inputs(compiler, command, item['sources'])
                sources = sorted(set([*item['sources'], *dependencies, *item.get('data', [])]))
                item['inputs'] = [dict(path=p, sha256=digest(Path(p))) for p in sources]
            item['inputs'] = list({value['path']: value for value in
                                   [*item['inputs'], *companions, *tool_inputs]}.values())
            for value in ([] if windows_fixture or args.gnu else item['inputs']):
                path = Path(value['path'])
                if path.suffix != '.c':
                    continue
                # The Windows file adapter's legacy shell lookup is not called by native checks.
                if routing.windows_host() and path == Path('tools/etude/win/files.c'):
                    continue
                text = path.read_text(errors='replace')
                if (re.search(r'\b(?:system|popen)\s*\(', text) or
                        re.search(r'["\'](?:/bin/)?(?:sh|bash|python3?|perl)["\']', text) or
                        re.search(r'#![^\n"\']*\b(?:sh|bash|python3?|perl)\b', text) or
                        'ETUDE_SHELL' in text):
                    item['unsupported'] = ('a compiled source still requires an interpreter; '
                                           'a native fixture adapter is required: ' + str(path))
                    break
            item['command'] = [*compiler, *command]
        elif item['program']:
            item['sha256'] = digest(Path(item['program']))
        item.pop('sources', None)
        item.pop('data', None)
        inventory.append(item)
    if args.print_commands or args.only:
        return
    record = dict(schema_version=1, group=args.group, architecture=args.arch if args.group in ('tests', 'boots') else 'none',
                  manifest_sha256=digest(Path('tests/cases/host.host.json')), checks=inventory)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + '.tmp')
    temporary.write_text(json.dumps(record, sort_keys=True, separators=(',', ':')) + '\n')
    temporary.replace(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--arch', choices=('i686', 'x86_64'), default=os.environ.get('ARCH', 'i686'))
    parser.add_argument('--gnu', action='store_true', help='prepare the declared GNU helper checks')
    parser.add_argument('--print-commands', action='store_true')
    parser.add_argument('--only', help='prepare one check for a component build')
    parser.add_argument('group', choices=GROUPS)
    parser.add_argument('output')
    args = parser.parse_args()
    try:
        prepare(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'host prepare: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
