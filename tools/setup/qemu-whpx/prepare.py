#!/usr/bin/env python3
"""Prepare fixed QEMU sources and private UCRT build libraries."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            h.update(block)
    return h.hexdigest()


def archive(cache, item):
    path = cache / item['filename']
    if not path.exists():
        with urllib.request.urlopen(item['url']) as source, path.open('xb') as dest:
            shutil.copyfileobj(source, dest)
    if path.stat().st_size != item['size_bytes'] or digest(path) != item['sha256']:
        raise ValueError('archive digest or length differs: ' + str(path))
    return path


def extract(path, dest, scratch):
    plain = scratch / 'package.tar'
    try:
        subprocess.run(['zstd', '-q', '-d', '-o', str(plain), str(path)], check=True)
        names = subprocess.check_output(['bsdtar', '-tf', str(plain)], text=True)
        members = []
        for name in names.splitlines():
            relative = Path(name)
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError('unsafe package path: ' + name)
            if name.startswith('ucrt64/'):
                members.append(name)
        if members:
            subprocess.run(['bsdtar', '-xf', str(plain), '-C', str(dest)] + members,
                           check=True)
    finally:
        if plain.exists():
            plain.unlink()


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: prepare.py OUTPUT ARCHIVE_CACHE')
    here = Path(__file__).resolve().parent
    pins = json.loads((here / 'pins.json').read_text())
    out = Path(sys.argv[1]).resolve()
    cache = Path(sys.argv[2]).resolve()
    out.mkdir()
    cache.mkdir(parents=True, exist_ok=True)
    deps = out / 'dependencies'
    deps.mkdir()
    for item in pins['packages']:
        path = archive(cache, item)
        extract(path, deps, out)
        print('prepared ' + path.name, flush=True)
    source = archive(cache, pins['source'])
    shutil.copyfile(source, out / source.name)
    for name in ['build.sh', 'pins.json', 'whpx-live-xsave-size.patch']:
        shutil.copyfile(here / name, out / name)
    bundle = out / 'qemu-ucrt-dependencies.tar.gz'
    subprocess.run(['bsdtar', '-czf', str(bundle), '-C', str(deps), 'ucrt64'], check=True)
    record = {'filename': bundle.name, 'sha256': digest(bundle),
              'size_bytes': bundle.stat().st_size}
    (out / 'dependencies.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record), flush=True)


if __name__ == '__main__':
    main()
