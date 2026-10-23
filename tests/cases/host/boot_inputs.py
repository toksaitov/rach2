"""Runtime files read by host checks that boot the collection host's QEMU."""
from pathlib import Path

SCHEMA_SOURCE = Path('tools/etude/schemas')
SCHEMA_STAGE = Path('build/tests/host-suite/schemas')


def files(identity, arch):
    """Hash each prepared runtime input beside the check's compiled sources."""
    build = Path('build/x86_64' if arch == 'x86_64' else 'build')
    inputs = ['tests/cases/host/boot_inputs.py']
    if identity in {'etude_legacyset', 'etude_qemu', 'etude_scenario', 'etude_watchdog'}:
        inputs += [str(build / 'rach2/kernel'), str(build / 'images/test.krffs')]
    if identity == 'etude_scenario':
        inputs += [str(build / 'images/host-etude-scenario.disk'), str(build / 'tests/hello')]
    if identity == 'etude_legacyset':
        inputs += ['build/tools/etude/etude', str(build / 'tools/etude/guest/etude-guest'),
                   str(build / 'tests/rngprobe')]
        for schema in sorted(SCHEMA_SOURCE.glob('*.schema.json')):
            inputs += [str(schema), str(SCHEMA_STAGE / schema.name)]
    return inputs
