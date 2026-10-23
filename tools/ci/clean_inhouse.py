#!/usr/bin/env python3
"""Remove in-house release products while retaining caches and run evidence."""
import argparse
from pathlib import Path
import shutil
import re
import sys

# These directories contain build products, not failed-run evidence. Vendor
# stamps and their validated stores keep their ordinary source dependency rules.
ARCH_OUTPUTS = ('utils', 'ish', 'ports', 'doom', 'images', 'iso', 'man')
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'etude'))
from preparation_paths import filesystem_path

TEST_OUTPUTS = ('cases', 'host', 'host-suite', 'system', 'style', 'fuzz', 'native', 'musl')


def remove(path, keep):
    if path.absolute() == keep:
        return
    if path.absolute() in keep.parents:
        if path.is_dir() and not path.is_symlink():
            for child in path.iterdir():
                remove(child, keep)
        return
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)


def probes(tree):
    text = (tree / 'tests/Makefile').read_text().replace('\\\n', ' ')
    match = re.search(r'^PROGS = (.*)$', text, re.MULTILINE)
    if not match:
        raise ValueError('guest probe product declaration is missing')
    names = match.group(1).split()
    if any(not re.fullmatch(r'[A-Za-z0-9_-]+', name) for name in names):
        raise ValueError('guest probe product declaration is not a literal name list')
    return [name + suffix for name in names for suffix in ('', '.d')] + ['.config']


def clean(tree, arches, output):
    tree = tree.resolve(strict=True)
    output = output.resolve(strict=True)
    build = tree / 'build'
    if not (tree / 'Makefile').is_file() or build.is_symlink():
        raise ValueError('clean needs a real checkout build directory')
    for arch in ('i686', 'x86_64') if arches == 'both' else (arches,):
        base = build / 'x86_64' if arch == 'x86_64' else build
        if base.is_symlink():
            raise ValueError('architecture build directory is a symlink')
        for name in ARCH_OUTPUTS:
            remove(base / name, output)
        if base.is_dir():
            for path in base.iterdir():
                if path.name == 'rach2' or path.name.startswith('rach2-'):
                    remove(path, output)
        for name in (*TEST_OUTPUTS, *probes(tree)):
            remove(base / 'tests' / name, output)
        for name in ('.preflight',):
            remove(base / name, output)
    for name in ('tools', 'krffs'):
        remove(build / name, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tree', type=filesystem_path)
    parser.add_argument('arches', choices=('i686', 'x86_64', 'both'))
    parser.add_argument('output', type=filesystem_path)
    args = parser.parse_args()
    clean(args.tree, args.arches, args.output)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        print('ci: clean: ' + str(error), file=sys.stderr)
        sys.exit(2)
