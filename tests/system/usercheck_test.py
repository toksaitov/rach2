#!/usr/bin/env python3
"""Compiler fixtures for the user-pointer checker, run during preparation."""
import importlib.util
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('usercheck', ROOT / 'tools/verify/usercheck.py')
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)

CASES = {
    'arithmetic': (True, 'int __user *f(int __user *p) { return p + 1; }'),
    'dereference': (False, 'int f(int __user *p) { return *p; }'),
    'index': (False, 'int f(int __user *p) { return p[2]; }'),
    'member': (False, 'struct s { int x; }; int f(struct s __user *p) { return p->x; }'),
    'assignment': (False, 'int *f(int __user *p) { int *q = p; return q; }'),
    'argument': (False, 'void g(int *); void f(int __user *p) { g(p); }'),
    'return': (False, 'int *f(int __user *p) { return p; }'),
    'function': (False, 'void g(int *); void f(void) { void (*p)(int __user *) = g; (void)p; }'),
    'cast': (False, 'int *f(int __user *p) { return (int *)p; }'),
    'typedef': (False, 'typedef int __user *up; int *f(up p) { return (int *)p; }'),
    'preserve': (True, 'void __user *f(int __user *p) { return (void __user *)p; }'),
    'integer': (False, 'int *f(int __user *p) { return (int *)(unsigned long)p; }'),
    'comment': (False, 'int *copy_from_user(int __user *p) { return (int /* __force */ *)p; }'),
    'operand-comment': (False, 'int *copy_from_user(int __user *p) { return (int *) /* __force */ p; }'),
    'wrong-function': (False, 'int *f(int __user *p) { return (int __force *)p; }'),
    'trusted-copy': (True, 'int *copy_from_user(int __user *p) { return (int __force *)p; }'),
    'trusted-integer': (True, 'unsigned long copy_to_user(int __user *p) { return (__force unsigned long)p; }'),
}


def main():
    header = (ROOT / 'rach2/sys/lib/kuser.h').as_posix()
    with tempfile.TemporaryDirectory(prefix='rach2-usercheck.') as scratch:
        path = Path(scratch) / 'usercopy.c'
        for arch in ('i686', 'x86_64'):
            for name, (wanted, body) in CASES.items():
                path.write_text(f'#include "{header}"\n{body}\n')
                passed, output = CHECKER.check(path, arch, trusted=path)
                print(f'{arch}: {name}: {"accepted" if passed else "rejected"}')
                if passed != wanted:
                    raise ValueError(f'{arch}: {name}: unexpected result\n{output}')
            path.write_text(f'#include "{header}"\n{CASES["trusted-copy"][1]}\n')
            passed, output = CHECKER.check(path, arch)
            if passed:
                raise ValueError(f'{arch}: __force accepted outside usercopy.c')
            print(f'{arch}: wrong-file: rejected')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
