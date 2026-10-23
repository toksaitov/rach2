#!/usr/bin/env python3
"""Record the binary digest from the build's verified Codex package."""

import hashlib
import os
from pathlib import Path
import sys
import tarfile
import tempfile


def prepare(archive, destination):
    """Hash exactly one regular binary, publishing only a complete digest."""
    with tarfile.open(archive, 'r:gz') as package:
        members = [m for m in package.getmembers()
                   if m.name in ('bin/codex', './bin/codex')]
        if len(members) != 1 or not members[0].isfile() or not members[0].size:
            raise ValueError('Codex package must contain one regular bin/codex')
        digest = hashlib.sha256()
        with package.extractfile(members[0]) as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='ascii',
                                         dir=destination.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(digest.hexdigest() + '\n')
        os.replace(temporary, destination)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: prepare_codex_digest.py VERIFIED_PACKAGE OUTPUT')
    prepare(sys.argv[1], Path(sys.argv[2]))
