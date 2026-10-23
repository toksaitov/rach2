#!/usr/bin/env python3
"""Check the kernel's user-pointer contract with Clang, during preparation."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TRUSTED = ROOT / 'rach2/sys/mm/usercopy.c'
COPIES = {'copy_from_user', 'copy_to_user', 'strncpy_from_user', 'strnlen_user'}


def user_type(node):
    types = node.get('type', {})
    spelling = types.get('desugaredQualType', types.get('qualType', ''))
    # Clang retains the annotation macro's name in its printed AST types.
    return 'address_space(1)' in spelling or re.search(r'\b__user\b', spelling) is not None


def force_cast(node, source):
    """Read only the parsed cast's type range, never its operand or comments."""
    begin = node.get('range', {}).get('begin', {})
    children = node.get('inner', [])
    if not children or 'offset' not in begin:
        return False
    end = children[0].get('range', {}).get('begin', {})
    if 'offset' not in end or end['offset'] <= begin['offset']:
        return False
    spelling = source[begin['offset']:end['offset']]
    spelling = re.sub(rb'/\*.*?\*/|//[^\n]*', b' ', spelling, flags=re.S)
    return re.fullmatch(rb'\s*\([^()]*\b__force\b[^()]*\)\s*', spelling) is not None


def audit(node, source, trusted, function=None):
    failures = []
    if node.get('kind') == 'FunctionDecl':
        function = node.get('name')
        trusted = trusted and 'includedFrom' not in node.get('loc', {})
    children = node.get('inner', [])
    if node.get('kind') == 'CStyleCastExpr' and children and \
            user_type(children[0]) and not user_type(node):
        if not (trusted and function in COPIES and force_cast(node, source)):
            offset = node.get('range', {}).get('begin', {}).get('offset', 0)
            line = source[:offset].count(b'\n') + 1
            failures.append(f'{line}: error: user-pointer cast requires __force in a user-copy helper')
    for child in children:
        failures.extend(audit(child, source, trusted, function))
    return failures


def check(path, arch, compiler='clang', trusted=TRUSTED):
    target = 'i686-elf' if arch == 'i686' else 'x86_64-elf'
    resource = subprocess.check_output([compiler, '-print-resource-dir'], text=True).strip()
    command = [compiler, '--target=' + target, '-ansi', '-ffreestanding', '-nostdinc',
               '-isystem', str(Path(resource) / 'include'), '-DRACH2_USERCHECK',
               '-Werror=noderef', '-fsyntax-only', '-Xclang', '-ast-dump=json', str(path)]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    diagnostics = result.stderr.decode(errors='replace')
    if result.returncode:
        return False, diagnostics
    findings = audit(json.loads(result.stdout), path.read_bytes(), path.resolve() == trusted.resolve())
    return not findings and not diagnostics, diagnostics + ''.join(f'{path}:{s}\n' for s in findings)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=('i686', 'x86_64'), required=True)
    parser.add_argument('--clang', default='clang')
    parser.add_argument('sources', type=Path, nargs='+')
    args = parser.parse_args()
    failed = False
    for path in args.sources:
        passed, diagnostics = check(path, args.arch, args.clang)
        sys.stdout.write(diagnostics)
        failed |= not passed
    return int(failed)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f'usercheck: {error}')
