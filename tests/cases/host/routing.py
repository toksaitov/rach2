"""Choose the execution host while retaining every declared check identity."""
import os
import re
import sys
from pathlib import Path

# These tests launch the collection host's QEMU and must stay there.
BOOTS = {'etude_qemu', 'etude_scenario', 'etude_watchdog', 'etude_legacyset'}
# Filesystem tools and POSIX build products run on the GNU helper.
PRODUCTS = {'ext2', 'fat32', 'etude_volume', 'etude_compare', 'etude_step',
            'etude_native'}
# These fixtures write a script solely to require its rejection before launch.
SCRIPT_REFUSALS = {'tests/host/etude_collection_hosttest.c',
                   'tests/host/etude_hostdriver_hosttest.c'}


def windows_host():
    """MSYS Python prepares native Windows programs despite its POSIX os.name."""
    target = os.environ.get('HOST_TEST_PLATFORM')
    if target:
        return target == 'windows'
    return os.name == 'nt' or sys.platform in ('msys', 'cygwin')


def interpreter(text, path=None):
    return (re.search(r'\b(?:system|popen)\s*\(', text) or
            re.search(r'["\'](?:/bin/)?(?:sh|bash|python3?|perl)["\']', text) or
            (str(path) not in SCRIPT_REFUSALS and
             re.search(r'#![^\n"\']*\b(?:sh|bash|python3?|perl)\b', text)) or
            'ETUDE_SHELL' in text)


def backend(identity, sources, group):
    """The Windows adapter fixtures execute natively, including file checks."""
    if group == 'scripts' or identity in BOOTS or identity in {'etude_platform', 'usbstamp'}:
        return 'native'
    if identity in PRODUCTS or windows_host():
        return 'gnu'
    for source in sources:
        path = Path(source)
        if path.suffix == '.c' and path.is_file() and interpreter(path.read_text(errors='replace'), path):
            return 'gnu'
    return 'native'
