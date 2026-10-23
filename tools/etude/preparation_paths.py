"""Filesystem and serialized native paths for preparation tools."""
import os
from pathlib import Path
import re
import subprocess
import sys


def msys_host():
    return sys.platform in ('msys', 'cygwin')


def cygpath(option, value):
    result = subprocess.check_output(['cygpath', option, '--', os.fspath(value)],
                                     text=True).rstrip('\n')
    if not result or '\n' in result or '\r' in result or '\0' in result:
        raise ValueError('cygpath returned an invalid path')
    return result


def filesystem_path(value):
    """Read native Windows CLI operands with the current Python filesystem."""
    text = os.fspath(value)
    if msys_host() and (re.match(r'^[A-Za-z]:[\\/]', text) or
                        text.startswith(('//', '\\\\'))):
        text = cygpath('-au', text)
    return Path(text)


def native_path(value, strict=True):
    """Serialize the canonical absolute path used by the native C runner."""
    path = filesystem_path(value).resolve(strict=strict)
    if msys_host():
        result = cygpath('-am', path)
        if not (re.match(r'^[A-Za-z]:/', result) or result.startswith('//')):
            raise ValueError('cygpath did not return an absolute Windows path')
        return result
    return path.as_posix()
