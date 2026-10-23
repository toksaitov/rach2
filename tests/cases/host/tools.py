"""Prepare native tools used by C host fixtures; never run a fixture here."""
from pathlib import Path
import os
import hashlib
import json
import shutil
import sys
import shlex
import subprocess

SOURCES = {'mkfatfixture': ['tests/host/mkfatfixture.c'],
           'etude-workload': ['tests/host/fixtures/etude_workload.c',
                              'tests/host/fixtures/guest_workload.c'],
           'mkexfatfixture': ['tests/host/mkexfatfixture.c'],
           'setidcheck': ['tools/inspect/setidcheck.c'],
           'etude-volume': ['tools/etude/volume/etude_volume.c', 'tools/etude/volume/volume.c']}
EXTERNAL = {'tar', 'mdir', 'mcopy', 'mke2fs', 'debugfs', 'dumpe2fs', 'e2fsck', 'python3'}
BUILT = {}


def external_tool(name):
    """Find a declared native binary during preparation, never during a test."""
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


def prepare(names, compiler, flags, dependencies, digest, print_commands=False):
    """Return source and executable fingerprints for all declared tool names."""
    root = Path('build/tests/host-suite/tools')
    inputs = {}
    for name in names:
        if name in BUILT:
            inputs.update(BUILT[name])
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
            provenance = root / (name + '.provenance.json')
            provenance.write_text(json.dumps(dict(source=str(source), sha256=digest(output)),
                                             sort_keys=True) + '\n')
            inputs.update({str(p): dict(path=str(p), sha256=digest(p)) for p in [output, provenance]})
            continue
        if name not in SOURCES:
            raise ValueError(f'no native host tool preparation recipe: {name}')
        output = str(root / name)
        sources = SOURCES[name]
        temporary = output + f'.{os.getpid()}.tmp'
        tool_flags = flags
        if name == 'etude-workload':
            # This payload has no test judge or sanitizer fault case.
            tool_flags = [f for f in flags if not f.startswith((
                '-fsanitize=', '-fno-sanitize-recover=', '-g'))]
        command = [*tool_flags, '-Wall', '-Wextra', '-Werror', '-o', temporary, *sources]
        print('host prepare: ' + shlex.join([*compiler, *command]), flush=True)
        if print_commands:
            continue
        root.mkdir(parents=True, exist_ok=True)
        subprocess.run([*compiler, *command], check=True)
        if not Path(temporary).exists() and Path(temporary + '.exe').exists():
            Path(temporary + '.exe').replace(temporary)
        Path(temporary).replace(output)
        paths = sorted(set([output, *sources, *dependencies(compiler, command, sources)]))
        BUILT[name] = {p: dict(path=p, sha256=digest(Path(p))) for p in paths}
        inputs.update(BUILT[name])
    return [inputs[p] for p in sorted(inputs)]


if __name__ == '__main__':
    try:
        if len(sys.argv) != 3 or sys.argv[1] != '--external-exfat':
            raise ValueError('usage: tools.py --external-exfat RECORD')
        external_exfat(sys.argv[2])
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'host tools: {error}', file=sys.stderr)
        sys.exit(1)
