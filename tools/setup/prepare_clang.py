#!/usr/bin/env python3
"""Extract the pinned native Windows source checker into a private prefix."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
PINS = ROOT / 'tools/setup/llvm21.json'


def digest(path):
    with path.open('rb') as stream:
        result = hashlib.sha256()
        for data in iter(lambda: stream.read(1048576), b''):
            result.update(data)
    return result.hexdigest()


def native(path):
    if sys.platform in ('msys', 'cygwin'):
        return subprocess.check_output(['cygpath', '-m', str(path)], text=True).strip()
    return str(path)


def payload(prefix):
    paths = sorted(path for path in prefix.rglob('*') if path.is_file() and path.name != 'clang.json')
    if any(path.is_symlink() for path in paths):
        raise ValueError('the compiler prefix contains a symbolic link')
    return {path.relative_to(prefix).as_posix(): digest(path) for path in paths}


def identity(prefix):
    pins = json.loads(PINS.read_text())
    record = json.loads((prefix / 'clang.json').read_text())
    if record.get('origin') != pins or record.get('files') != payload(prefix):
        raise ValueError('the private Windows compiler changed; use a fresh prefix')
    return record


def fetch(item, cache):
    path = cache / item['file']
    if not path.exists():
        temporary = path.with_suffix(path.suffix + '.part')
        print('clang preparation: downloading ' + item['url'], flush=True)
        try:
            with urllib.request.urlopen(item['url']) as source, temporary.open('wb') as output:
                shutil.copyfileobj(source, output)
            if digest(temporary) != item['sha256']:
                raise ValueError('download digest mismatch: ' + item['file'])
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)
    if digest(path) != item['sha256']:
        raise ValueError('cached download digest mismatch: ' + str(path))
    return path


def prepare(prefix, cache, sevenzip):
    if prefix.exists():
        identity(prefix)
        print('clang preparation: unchanged: ' + str(prefix))
        return
    pins = json.loads(PINS.read_text())
    cache.mkdir(parents=True, exist_ok=True)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    package = fetch(pins['package'], cache)
    fetch(pins['signature'], cache)
    with tempfile.TemporaryDirectory(dir=prefix.parent, prefix='rach2-clang.') as scratch:
        stage = Path(scratch) / 'prefix'
        stage.mkdir()
        # 7-Zip reads the NSIS archive; the installer is never executed.
        subprocess.run([str(sevenzip), 'x', '-y', '-o' + native(stage), native(package),
                        'bin/clang.exe', 'lib/clang/21/include/*', '-r'], check=True)
        if not (stage / 'bin/clang.exe').is_file() or \
                not (stage / 'lib/clang/21/include/stddef.h').is_file():
            raise ValueError('the pinned archive did not provide the compiler and headers')
        for item in pins['licenses']:
            shutil.copy2(fetch(item, cache), stage / item['file'])
        environment = {**os.environ, 'PATH': os.environ.get('SYSTEMROOT', 'C:/Windows') + '/System32'}
        version = subprocess.check_output([str(stage / 'bin/clang.exe'), '--version'],
                                          env=environment, text=True).splitlines()[0]
        if not version.startswith('clang version ' + pins['version']):
            raise ValueError('the extracted compiler has a different version: ' + version)
        record = dict(origin=pins, compiler_version=version, files=payload(stage))
        (stage / 'clang.json').write_text(json.dumps(record, indent=2) + '\n')
        stage.rename(prefix)
    print('clang preparation: ' + version)
    print('clang preparation: receipt: ' + str(prefix / 'clang.json'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', type=Path)
    parser.add_argument('--cache', type=Path, default=ROOT / 'build/downloads/host-tools')
    parser.add_argument('--sevenzip', type=Path, default=Path('C:/Program Files/7-Zip/7z.exe'))
    args = parser.parse_args()
    prepare(args.prefix.resolve(), args.cache.resolve(), args.sevenzip)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit('clang preparation: ' + str(error)) from error
