"""Choose the execution host while retaining every declared check identity."""
import json
import os
import re
import sys
from pathlib import Path

# These tests launch the collection host's QEMU and must stay there.
BOOTS = {'etude_qemu', 'etude_scenario', 'etude_watchdog', 'etude_legacyset'}
# Filesystem tools and POSIX build products run on the GNU helper.
PRODUCTS = {'ext2', 'fat32', 'etude_volume', 'etude_compare', 'etude_step',
            'etude_native', 'doom_platform'}
# These fixtures write a script solely to require its rejection before launch.
SCRIPT_REFUSALS = {'tests/host/etude_collection_hosttest.c',
                   'tests/host/etude_hostdriver_hosttest.c'}


def windows_host():
    """MSYS Python prepares native Windows programs despite its POSIX os.name."""
    target = os.environ.get('HOST_TEST_PLATFORM')
    if target:
        return target == 'windows'
    return os.name == 'nt' or sys.platform in ('msys', 'cygwin')


def without_comments(text):
    """Remove C comments while preserving quoted strings and characters."""
    token = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*'
    return re.sub(token, lambda m: ' ' if m[0].startswith('/') else m[0],
                  text, flags=re.DOTALL)


def interpreter(text, path=None):
    text = without_comments(text)
    return (re.search(r'\b(?:system|popen)\s*\(', text) or
            re.search(r'["\'](?:/bin/)?(?:sh|bash|python3?|perl)["\']', text) or
            (str(path) not in SCRIPT_REFUSALS and
             re.search(r'#![^\n"\']*\b(?:sh|bash|python3?|perl)\b', text)) or
            'ETUDE_SHELL' in text)


def script_backend(identity):
    """Require one declared owner before assigning a scripts check's host."""
    parts = {'host.host.scripts', 'host.host.scripts_native', 'host.host.scripts_images',
             'host.host.scripts_build', 'host.host.scripts_tools'}
    cases = json.loads(Path('tests/cases/host.host.json').read_text())['cases']
    owners = [case['case_id'] for case in cases if case['case_id'] in parts
              for assertion in case['assertions'] if assertion['assertion_id'] == identity]
    if len(owners) != 1:
        raise ValueError('a scripts check needs exactly one declared partition: ' + identity)
    return 'native' if owners[0] == 'host.host.scripts_native' else 'gnu'


def backend(identity, sources, group):
    """The Windows adapter fixtures execute natively, including file checks."""
    if group == 'scripts':
        return script_backend(identity)
    if identity in BOOTS or identity in {'etude_platform', 'etude_context', 'etude_observe', 'usbstamp'}:
        return 'native'
    if identity in PRODUCTS or windows_host():
        return 'gnu'
    for source in sources:
        path = Path(source)
        if path.suffix == '.c' and path.is_file() and interpreter(path.read_text(errors='replace'), path):
            return 'gnu'
    return 'native'
