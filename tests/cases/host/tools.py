"""Prepare native tools used by C host fixtures; never run a fixture here."""
from pathlib import Path
import os
import hashlib
import json
import shutil
import sys
import shlex
import subprocess
import fixtures

SOURCES = {'mkfatfixture': ['tests/host/mkfatfixture.c'],
           'etude-workload': ['tests/host/fixtures/etude_workload.c',
                              'tests/host/fixtures/guest_workload.c'],
           'mkexfatfixture': ['tests/host/mkexfatfixture.c'],
           'setidcheck': ['tools/inspect/setidcheck.c'],
           'etude-volume': ['tools/etude/volume/etude_volume.c', 'tools/etude/volume/volume.c']}
EXTERNAL = {'tar', 'mdir', 'mcopy', 'mke2fs', 'debugfs', 'dumpe2fs', 'e2fsck', 'python3'}


def external_tool(name):
    """Find a declared native binary during preparation, never during a test."""
    if name == 'atos' and sys.platform == 'darwin':
        source = Path('/usr/bin/atos')
        if not source.is_file():
            raise ValueError('the native sanitizer symbolizer is missing: ' + str(source))
        return source.resolve()
    explicit = os.environ.get('HOST_TEST_PREPARED_TOOLS')
    if explicit:
        path = Path(explicit) / name
        if not path.exists() and Path(str(path) + '.exe').exists():
            path = Path(str(path) + '.exe')
        if not path.is_file():
            raise ValueError(f'prepared native host tool is missing: {path}')
        return path.resolve()
    raise ValueError(f'{name}: independent tools require the declared GNU helper; '
                     'set HOST_TEST_PREPARED_TOOLS during GNU preparation')


def copied_tool(root, name, source, digest):
    """Publish a native executable and its actual resolved-source provenance."""
    output = root / name
    root.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + f'.{os.getpid()}.tmp')
    shutil.copyfile(source, temporary)
    temporary.chmod(source.stat().st_mode)
    temporary.replace(output)
    magic = output.read_bytes()[:4]
    if magic not in (b'\x7fELF', b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf',
                     b'\xce\xfa\xed\xfe', b'\xfe\xed\xfa\xce', b'\xca\xfe\xba\xbe') and not magic.startswith(b'MZ'):
        raise ValueError(f'prepared host tool is not a native executable: {source}')
    if digest(source) != digest(output):
        raise ValueError('native tool changed while copying: ' + str(source))
    provenance = root / (name + '.provenance.json')
    provenance.write_text(json.dumps(dict(source=str(source), sha256=digest(output)),
                                     sort_keys=True) + '\n')
    return {str(p): dict(path=str(p), sha256=digest(p)) for p in (output, provenance)}


def sanitizer_tools(root, digest):
    """Bind the native sanitizer tool set, including an empty non-Mac set."""
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    inputs = copied_tool(root, 'atos', external_tool('atos'), digest) if sys.platform == 'darwin' else {}
    manifest = root / 'tools.json'
    manifest.write_text(json.dumps(dict(schema_version=1, platform=sys.platform,
                                      inputs=[inputs[p] for p in sorted(inputs)]), sort_keys=True) + '\n')
    inputs[str(manifest)] = dict(path=str(manifest), sha256=digest(manifest))
    return inputs


def external_exfat(marker):
    """Produce independent exFAT bytes once and retain a source-bound record."""
    root = Path('build/tests/host-suite/tools/exfat_external')
    source = Path('tests/host/mkexfat_external.sh')
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    try:
        old = json.loads(Path(marker).read_text())
        if old['producer_sha256'] == source_hash and old['files'] and all(
                p.is_file() and old['files'].get(str(p.relative_to(root))) ==
                hashlib.sha256(p.read_bytes()).hexdigest() for p in root.rglob('*') if p.is_file()) and \
                set(old['files']) == {str(p.relative_to(root)) for p in root.rglob('*') if p.is_file()}:
            return
    except (OSError, KeyError, ValueError):
        pass
    temporary = root.with_name(root.name + f'.{os.getpid()}.tmp')
    temporary.mkdir(parents=True)
    try:
        writer = temporary / 'writer'
        subprocess.run(['sh', str(source), '-p', str(writer.resolve())], check=True)
        subprocess.run(['sh', str(source), str(temporary.resolve())], check=True)
        files = {str(p.relative_to(temporary)): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in temporary.rglob('*') if p.is_file()}
        if not files or not (temporary / 'exfat_external.img').is_file() or not (
                temporary / 'exfat_external.manifest').is_file():
            raise ValueError('independent exFAT fixture is incomplete')
        if root.exists():
            shutil.rmtree(root)
        temporary.replace(root)
        Path(marker).parent.mkdir(parents=True, exist_ok=True)
        Path(marker).write_text(json.dumps(dict(producer_sha256=source_hash, files=files),
                                          sort_keys=True) + '\n')
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


def compiled_program(root, name, compiler, command, sources, dependencies, digest,
                     identity, print_commands):
    """Publish or reuse a program only for its complete current build recipe."""
    output = str(root / name)
    if print_commands:
        print('host prepare: ' + shlex.join([*compiler, *command]), flush=True)
        return {}
    if identity is None:
        raise ValueError('compiled host tool needs the actual compiler identity: ' + name)
    external = {}
    paths = sorted(set([*sources, *dependencies(compiler, command, sources, external, include_system=True)]))
    source_inputs = [dict(path=path, sha256=digest(Path(path))) for path in paths]
    recipe = dict(compiler=identity, command=[*compiler, *command], inputs=source_inputs,
                  compiler_inputs=[external[path] for path in sorted(external)])
    marker = root / (name + '.build.json')
    if marker.exists():
        old = json.loads(marker.read_text())
        if old.get('schema_version') != 1 or 'recipe' not in old or 'sha256' not in old:
            raise ValueError('invalid compiled host tool record: ' + str(marker))
        if old['recipe'] == recipe:
            if not Path(output).is_file() or digest(Path(output)) != old['sha256']:
                raise ValueError('prepared host tool bytes changed: ' + output)
            return {path: dict(path=path, sha256=digest(Path(path)))
                    for path in [*paths, output, str(marker)]}
    root.mkdir(parents=True, exist_ok=True)
    temporary = output + f'.{os.getpid()}.tmp'
    actual = [temporary if word == output else word for word in command]
    print('host prepare: ' + shlex.join([*compiler, *actual]), flush=True)
    try:
        subprocess.run([*compiler, *actual], check=True)
        made = fixtures.publish_program(temporary)
        after = {}
        after_paths = sorted(set([*sources, *dependencies(compiler, command, sources, after, include_system=True)]))
        if after_paths != paths or after != external or any(
                digest(Path(item['path'])) != item['sha256'] for item in source_inputs):
            raise ValueError('host tool inputs changed during compilation: ' + name)
        made.replace(output)
        value = dict(schema_version=1, recipe=recipe, actual_command=[*compiler, *actual],
                     sha256=digest(Path(output)))
        temporary_record = marker.with_name(marker.name + f'.{os.getpid()}.tmp')
        try:
            temporary_record.write_text(json.dumps(value, sort_keys=True) + '\n')
            temporary_record.replace(marker)
        finally:
            temporary_record.unlink(missing_ok=True)
    finally:
        Path(temporary).unlink(missing_ok=True)
        Path(temporary + '.exe').unlink(missing_ok=True)
    return {path: dict(path=path, sha256=digest(Path(path)))
            for path in [*paths, output, str(marker)]}


def compiled_tool(root, name, compiler, flags, dependencies, digest, identity, print_commands):
    """Reuse shared tool bytes only for the same compiler, recipe and includes."""
    output = str(root / name)
    sources = SOURCES[name]
    if name == 'etude-workload':
        # This payload has no test judge or sanitizer fault case.
        flags = [flag for flag in flags if not flag.startswith((
            '-fsanitize=', '-fno-sanitize-recover=', '-g'))]
    command = [*flags, '-Wall', '-Wextra', '-Werror', '-o', output, *sources]
    return compiled_program(root, name, compiler, command, sources, dependencies, digest,
                            identity, print_commands)


def prepare(names, compiler, flags, dependencies, digest, print_commands=False, compiler_identity=None):
    """Return source and executable fingerprints for all declared tool names."""
    root = Path('build/tests/host-suite/tools')
    inputs = {}
    for name in names:
        if name == 'atos':
            if print_commands:
                print('host prepare: native sanitizer tool: /usr/bin/atos')
            else:
                inputs.update(sanitizer_tools('build/tests/host-suite/sanitizer', digest))
            continue
        if name == 'exfat_external':
            marker = Path('build/tests/host/exfat_writer')
            root_data = root / name
            if print_commands:
                print('host prepare: prepared independent exFAT data: ' + str(marker))
                continue
            if not marker.is_file() or not (root_data / 'exfat_external.img').is_file():
                raise ValueError('prepare build/tests/host/exfat_writer before the exFAT test')
            paths = [str(marker), 'tests/host/mkexfat_external.sh',
                     *[str(p) for p in root_data.rglob('*') if p.is_file()]]
            inputs.update({p: dict(path=p, sha256=digest(Path(p))) for p in paths})
            continue
        if name in EXTERNAL:
            if print_commands:
                print('host prepare: native external tool: ' + name)
                continue
            source = external_tool(name)
            inputs.update(copied_tool(root, name, source, digest))
            continue
        if name not in SOURCES:
            raise ValueError(f'no native host tool preparation recipe: {name}')
        inputs.update(compiled_tool(root, name, compiler, flags, dependencies, digest,
                                    compiler_identity, print_commands))
    return [inputs[p] for p in sorted(inputs)]


if __name__ == '__main__':
    try:
        if len(sys.argv) != 3 or sys.argv[1] not in ('--external-exfat', '--sanitizer'):
            raise ValueError('usage: tools.py --external-exfat RECORD | --sanitizer DIRECTORY')
        if sys.argv[1] == '--sanitizer':
            sanitizer_tools(sys.argv[2], lambda p: hashlib.sha256(p.read_bytes()).hexdigest())
        else:
            external_exfat(sys.argv[2])
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'host tools: {error}', file=sys.stderr)
        sys.exit(1)
