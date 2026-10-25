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
import shutil
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
KERNEL_INCLUDES = ['rach2/sys', 'rach2/sys/arch/x86', 'rach2/sys/arch/i686',
                   'rach2/sys/lib', 'rach2/sys/proc']
INCLUDES = ['utils', 'krffs', 'tests/host/compat']


def include_flags(kernel):
    """Keep kernel fixture headers out of the system header search."""
    quoted = [w for d in KERNEL_INCLUDES for w in ('-iquote', d)] if kernel else []
    return [*quoted, *[w for d in INCLUDES for w in ('-I', d)]]


def digest(path):
    """Hash a complete input or output of preparation."""
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def input_path(path):
    """Normalize compiler spellings without resolving the build store's links."""
    path = Path(os.path.normpath(path))
    if path.is_absolute():
        path = path.relative_to(ROOT)
    if not path.parts or '..' in path.parts or any(':' in part or '\\' in part for part in path.parts):
        raise ValueError(f'input path is not relative to the tree: {path}')
    return path.as_posix()


def decoded_input(value):
    """Read an encoded filename before testing whether a binary is reusable."""
    value = dict(value)
    if 'path_hex' in value:
        if 'path' in value:
            raise ValueError('an input has two path representations')
        value['path'] = os.fsdecode(bytes.fromhex(value.pop('path_hex')))
    value['path'] = input_path(value['path'])
    return value


def encoded_input(value):
    """Keep arbitrary fixture filename bytes outside the ASCII JSON strings."""
    value = decoded_input(value)
    if not value['path'].isascii():
        value['path_hex'] = os.fsencode(value.pop('path')).hex()
    return value


def compiler_record(command):
    """Identify the executable that compiled a reusable native inventory."""
    executable = shutil.which(command[0])
    if not executable:
        raise ValueError('the host compiler is unavailable: ' + command[0])
    path = Path(executable).resolve()
    version = subprocess.run([*command, '--version'], check=True, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.strip()
    if not version:
        raise ValueError('the host compiler reported no version')
    return dict(command=command, executable=str(path), sha256=digest(path), version=version)


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
            sources = list(dict.fromkeys([str(path), *metadata(path, 'host-deps', arch)]))
            flags = metadata(path, 'host-cflags', arch)
            required_tools = metadata(path, 'host-tools', arch)
            out = str(base / path.stem)
            item = entry(identity, path.stem, out, sources,
                         alone='host-alone:' in path.read_text())
            item['command'] = [*fixtures.platform_flags(COMMON), '-DRACH2_HOSTTEST', '-Wno-empty-body', '-ffreestanding',
                               *include_flags(group == 'tests'), *flags,
                               '-o', out, *sources]
            if routing.windows_host() and (identity in routing.BOOTS or identity in {'etude_context', 'etude_observe'}):
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
            if name == 'prosecheck':
                item['data'] = ['CLAUDE.md', 'AGENTS.md']
            if name == 'setidcheck':
                item['tools'] = ['mke2fs', 'debugfs']
                item['data'] = ['tools/inspect/setid.allow', 'tools/setup/e2fsprogs_path.sh']
            yield item
    elif group == 'fuzz':
        for source in sorted(Path('tests/fuzz').glob('*_fuzz.c')):
            name = source.stem.removesuffix('_fuzz')
            corpus = sorted(p for p in Path('tests/fuzz/corpus', name).rglob('*') if p.is_file())
            out = str(base / name)
            sources = list(dict.fromkeys([str(source), *metadata(source, 'fuzz-deps', arch)]))
            item = entry(name, name + ' corpus replayed', out, sources, map(str, corpus))
            if not corpus:
                raise ValueError(f'{name}: the fuzz harness has no corpus')
            item['command'] = [*fixtures.platform_flags(COMMON), '-DRACH2_HOSTTEST', '-DFIXTURE_NO_MAIN', '-DFUZZ_REPLAY',
                               '-Wno-empty-body', '-ffreestanding',
                               *include_flags(True),
                               *metadata(source, 'fuzz-cflags', arch), '-o', out, *sources]
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


def compiler_paths(command):
    """Compare GNU compiler operands through their immutable store links."""
    return tools.compiler_paths(command)


def previous_inventory(args, identity):
    """Use the local inventory first, then a previous GNU store output."""
    candidates = [(Path(args.output), None)]
    if args.previous:
        if not args.gnu or Path(args.output).is_absolute():
            raise ValueError('a previous tree requires a relative GNU inventory')
        candidates.append((Path(args.previous) / args.output, Path(args.previous)))
    for path, origin in candidates:
        try:
            old = json.loads(path.read_text())
            compiler, current = dict(old['compiler']), dict(identity)
            if origin:
                compiler['command'] = compiler_paths(compiler['command'])
                current['command'] = compiler_paths(current['command'])
            if compiler != current:
                continue
            for check in old['checks']:
                check['inputs'] = [decoded_input(value) for value in check['inputs']]
                if 'compile_inputs' in check:
                    check['compile_inputs'] = [decoded_input(value) for value in check['compile_inputs']]
            return {x['assertion_id']: x for x in old['checks']}, origin
        except (OSError, KeyError, TypeError, ValueError):
            continue
    return {}, None


def valid_previous(old, item, commands, origin=None, compilation=None, compiler_count=0):
    """Reuse a prepared binary only while its compiler, inputs and bytes match."""
    if not old:
        return False
    try:
        previous = [*old.get('compile_commands', []), old['command']]
        expected = commands
        if origin:
            previous = [compiler_paths(argv) for argv in previous]
            expected = [compiler_paths(argv) for argv in expected]
        program = Path(item['program'])
        if origin:
            program = origin / input_path(program)
        if previous != expected or old['sha256'] != digest(program):
            return False
        if compilation is not None:
            return compilation_matches(old, item, origin, compilation, compiler_count)
        return all(
            digest(Path(value['path'])) == value['sha256']
            for value in [*old['inputs'], *old.get('compiler_inputs', [])])
    except (KeyError, OSError, TypeError, ValueError):
        return False


def compiler_flags(command, sources):
    """Retain the compile options without its output or translation units."""
    flags = []
    skip = False
    for word in command:
        if skip:
            skip = False
        elif word == '-o':
            skip = True
        elif word not in sources and word != '-c':
            flags.append(word)
    return flags


def compiler_inputs(compiler, command, sources, external=None, include_system=False, root=None):
    """Scan each translation unit; a shared -MF would retain only the last."""
    flags = compiler_flags(command, sources)
    dependencies = set()
    for source in sources:
        if not source.endswith('.c'):
            continue
        result = subprocess.run([*compiler, *flags, '-M' if include_system else '-MM', '-MT', 'host-input', source],
                                check=True, text=True, capture_output=True, cwd=root)
        text = result.stdout.replace('\\\n', ' ')
        for name in shlex.split(text.split(':', 1)[1]):
            path = Path(os.path.normpath(name))
            base = root or ROOT
            if path.is_absolute() and not path.is_relative_to(base):
                if external is not None:
                    external[str(path)] = dict(path=str(path), sha256=digest(path))
            else:
                dependencies.add(input_path(path.relative_to(base) if path.is_absolute() else path))
    return dependencies


def compilation_inputs(compiler, command, sources, root=None):
    """Separate compiler dependencies from files used only by the test."""
    external = {}
    paths = sorted(set([*sources, *compiler_inputs(compiler, command, sources, external, root=root)]))
    local = [dict(path=p, sha256=digest((root or ROOT) / p)) for p in paths]
    return local, [external[p] for p in sorted(external)]


def canonical_inputs(values):
    """Compare external dependencies through immutable store aliases."""
    return sorted([dict(v, path=str(Path(v['path']).resolve(strict=True))) for v in values],
                  key=lambda v: v['path'])


def compilation_matches(old, item, origin, compilation, compiler_count):
    """Check exact compile dependencies, including an older record's scan."""
    current, external = compilation
    recorded = old.get('compile_inputs')
    if recorded is None:
        if origin is None:
            return False
        command = old['command']
        recorded, old_external = compilation_inputs(command[:compiler_count], command[compiler_count:],
                                                    item['sources'], origin)
        # The earlier tree must still match the inputs its inventory sealed.
        if any(v not in old['inputs'] for v in recorded) or \
                canonical_inputs(old_external) != canonical_inputs(old.get('compiler_inputs', [])):
            return False
    return recorded == current and canonical_inputs(old.get('compiler_inputs', [])) == canonical_inputs(external)


def compiled_interpreter(compiler, command, sources, candidates):
    """Inspect active C code with the compiler's actual defines and includes."""
    wanted = {path.resolve(): path for path in candidates}
    flags = compiler_flags(command, sources)
    for source in sources:
        if not source.endswith('.c'):
            continue
        result = subprocess.run([*compiler, *flags, '-E', source], check=True,
                                text=True, capture_output=True)
        chunks = {path: [] for path in wanted}
        current = None
        for line in result.stdout.splitlines():
            marker = re.match(r'^#\s+\d+\s+"([^"]+)"', line)
            if marker:
                current = Path(marker[1]).resolve()
            elif current in chunks:
                chunks[current].append(line)
        for path, lines in chunks.items():
            if routing.interpreter('\n'.join(lines), wanted[path]):
                return wanted[path]
    return None


def prepare(args):
    os.chdir(ROOT)
    base = Path(args.output).parent / args.group
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    if not compiler:
        raise ValueError('CC names no compiler')
    identity = compiler_record(compiler) if not args.print_commands else None
    if not args.print_commands:
        base.mkdir(parents=True, exist_ok=True)
    previous, origin = previous_inventory(args, identity) if not args.print_commands else ({}, None)
    inventory = []
    for item in checks(args.group, args.arch, base):
        if args.only and item['assertion_id'] != args.only:
            continue
        command = item.pop('command')
        item['instrumentation'] = sorted({name for flag in command or []
                                           if flag.startswith('-fsanitize=')
                                           for name in flag.split('=', 1)[1].split(',')})
        backend = routing.backend(item['assertion_id'], item['sources'], args.group)
        if args.gnu and item['assertion_id'] not in routing.BOOTS | {'etude_platform', 'etude_context', 'etude_observe'}:
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
        required_tools = item.pop('tools', [])
        if sys.platform == 'darwin' and item['instrumentation']:
            required_tools.append('atos')
            item['sanitizer_symbolizer'] = 'build/tests/host-suite/sanitizer/atos'
        tool_inputs = tools.prepare(required_tools, compiler, fixtures.platform_flags(COMMON), compiler_inputs,
                                   digest, args.print_commands, compiler_identity=identity, previous=origin)
        companions = windows.companions(compiler, base, compiler_inputs, digest,
                                        args.print_commands, compiler_identity=identity) if windows_fixture else []
        commands = []
        if command:
            commands = (fixtures.build_commands(compiler, command, item['sources'])
                        if args.group == 'scripts' and item['assertion_id'] in fixtures.NAMES
                        else [[*compiler, *command]])
        if args.print_commands:
            for argv in commands:
                print(shlex.join(argv))
            continue
        if command:
            old = previous.get(item['assertion_id'])
            compilation = compilation_inputs(compiler, command, item['sources']) if args.gnu else None
            reuse = valid_previous(old, item, commands, origin, compilation, len(compiler))
            if reuse:
                if origin:
                    source, target = origin / item['program'], Path(item['program'])
                    shutil.copyfile(source, target)
                    target.chmod((source.stat().st_mode & 0o777) | 0o200)
                    if digest(target) != old['sha256']:
                        raise ValueError('the previous program changed during its copy')
                item['sha256'], item['inputs'] = old['sha256'], old['inputs']
                item['compiler_inputs'] = old.get('compiler_inputs', [])
                if origin:
                    item['compiler_inputs'] = [dict(value, path=str(Path(value['path']).resolve(strict=True)))
                                               for value in item['compiler_inputs']]
                data_inputs = {value['path']: value for value in item['inputs']}
                for data in item.get('data', []):
                    data_inputs[data] = dict(path=data, sha256=digest(Path(data)))
                item['inputs'] = [data_inputs[key] for key in sorted(data_inputs)]
            else:
                for source in item['sources']:
                    if not Path(source).is_file():
                        raise ValueError(f"{item['assertion_id']}: missing source {source}")
                for argv in commands:
                    print('host prepare: ' + shlex.join(argv), flush=True)
                    subprocess.run(argv, check=True)
                program = Path(item['program'])
                fixtures.publish_program(program)
                item['sha256'] = digest(program)
                external = {}
                dependencies = compiler_inputs(compiler, command, item['sources'], external)
                item['compiler_inputs'] = [external[p] for p in sorted(external)]
                sources = sorted(set([*item['sources'], *dependencies, *item.get('data', [])]))
                item['inputs'] = [dict(path=p, sha256=digest(Path(p))) for p in sources]
            if compilation is not None:
                if not reuse and compilation != compilation_inputs(compiler, command, item['sources']):
                    raise ValueError('host check inputs changed during compilation: ' + item['assertion_id'])
                item['compile_inputs'], item['compiler_inputs'] = compilation
                inputs = {v['path']: v for v in item['compile_inputs']}
                for data in item.get('data', []):
                    inputs[data] = dict(path=data, sha256=digest(Path(data)))
                item['inputs'] = [inputs[key] for key in sorted(inputs)]
            all_inputs = {value['path']: value for value in
                          [*item['inputs'], *companions, *tool_inputs]}
            item['inputs'] = [all_inputs[path] for path in sorted(all_inputs)]
            candidates = []
            for value in ([] if windows_fixture or args.gnu else item['inputs']):
                path = Path(value['path'])
                if path.suffix != '.c':
                    continue
                # Native checks do not call the adapter's shell lookup.
                if routing.windows_host() and path == Path('tools/etude/win/files.c'):
                    continue
                text = path.read_text(errors='replace')
                if routing.interpreter(text, path):
                    candidates.append(path)
            if candidates:
                path = compiled_interpreter(compiler, command, item['sources'], candidates)
                if path:
                    item['unsupported'] = ('a compiled source still requires an interpreter; '
                                           'a native fixture adapter is required: ' + str(path))
            item['command'] = commands[-1]
            if len(commands) > 1:
                item['compile_commands'] = commands[:-1]
        elif item['program']:
            item['sha256'] = digest(Path(item['program']))
        item.pop('sources', None)
        item.pop('data', None)
        inventory.append(item)
    if args.print_commands or args.only:
        return
    for check in inventory:
        check['inputs'] = [encoded_input(value) for value in check['inputs']]
        if 'compile_inputs' in check:
            check['compile_inputs'] = [encoded_input(value) for value in check['compile_inputs']]
    record = dict(schema_version=1, group=args.group, architecture=args.arch if args.group in ('tests', 'boots') else 'none',
                  manifest_sha256=digest(Path('tests/cases/host.host.json')), compiler=identity,
                  checks=inventory)
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
    parser.add_argument('--previous', help='reuse verified programs from a previous GNU prepared tree')
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
