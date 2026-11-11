#!/usr/bin/env python3
"""Apply GE-Proton's DirectInput changes to its pinned Wine source."""

import pathlib
import re
import subprocess
import sys
import tarfile


def patches(archive, patterns):
    """Read the single patch matching each pattern, in the given order."""
    with tarfile.open(archive) as source:
        members = source.getmembers()
        result = []
        for pattern in patterns:
            matches = [m for m in members if pathlib.PurePosixPath(m.name).match(pattern)]
            if len(matches) != 1:
                raise ValueError(f"expected one patch for {pattern}, got {len(matches)}")
            result.append((matches[0].name, source.extractfile(matches[0]).read()))
        return result


def main():
    """Apply only the production files shared by DirectInput 7 and 8."""
    ge, staging, tree = sys.argv[1:]
    selected = patches(staging, [f"*/patches/dinput-scancode/{n:04d}-*.patch" for n in (1, 2, 3)])
    selected += patches(ge, [f"*/patches/proton-ds5-haptic/{n:04d}-*.patch" for n in (197, 198, 199, 202, 209)])
    for name, data in selected:
        chunks = re.split(rb"(?=^diff --git )", data, flags=re.M)[1:]
        kept = []
        for chunk in chunks:
            path = chunk.splitlines()[0].split()[2]
            if path == b"a/include/wine/hid.h" or (path.startswith(b"a/dlls/dinput/") and not path.startswith(b"a/dlls/dinput/tests/")):
                kept.append(chunk)
        if not kept:
            raise ValueError(f"no DirectInput source in {name}")
        print(f"gnu: proton: applying {pathlib.PurePosixPath(name).name}", flush=True)
        subprocess.run(["patch", "--batch", "--fuzz=0", "-p1"], input=b"".join(kept), cwd=tree, check=True)


if __name__ == "__main__":
    main()
