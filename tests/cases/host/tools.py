"""Prepare native tools used by C host fixtures; never run a fixture here."""
from pathlib import Path
import os
import shlex
import subprocess

SOURCES = {'mkfatfixture': ['tests/host/mkfatfixture.c'],
           'mkexfatfixture': ['tests/host/mkexfatfixture.c'],
           'setidcheck': ['tools/inspect/setidcheck.c'],
           'etude-volume': ['tools/etude/volume/etude_volume.c', 'tools/etude/volume/volume.c']}
BUILT = {}


def prepare(names, compiler, flags, dependencies, digest, print_commands=False):
    """Return source and executable fingerprints for all declared tool names."""
    root = Path('build/tests/host-suite/tools')
    inputs = {}
    for name in names:
        if name in BUILT:
            inputs.update(BUILT[name])
            continue
        if name not in SOURCES:
            raise ValueError(f'no native host tool preparation recipe: {name}')
        output = str(root / name)
        sources = SOURCES[name]
        temporary = output + f'.{os.getpid()}.tmp'
        command = [*flags, '-Wall', '-Wextra', '-Werror', '-o', temporary, *sources]
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
