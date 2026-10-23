#!/usr/bin/env python3
"""Find a spaced double hyphen used as an em-dash in the prose of Python
files: their comments, docstrings and messages. Run by textcheck.sh, which
gives the paths on stdin, one to a line."""

import ast
import io
import os
import sys
import tokenize

# The calls whose string arguments a person reads: output, exits and the
# error and logging helpers, matched by the name called.
MESSAGE_CALLS = {"print", "exit", "error", "warning", "warn", "fail", "die",
                 "info", "debug", "critical", "exception"}

# The keyword arguments of argparse that are prose. Its usage is a command
# example, where an end-of-options terminator is code.
MESSAGE_KEYWORDS = {"help", "description", "epilog"}

DASH = " -- "


def literals(node):
    """Yield the string literals in an expression, an f-string as one."""
    if isinstance(node, ast.JoinedStr):
        yield node
    elif isinstance(node, ast.Constant) and isinstance(node.value, str):
        yield node
    else:
        for child in ast.iter_child_nodes(node):
            yield from literals(child)


def called(func):
    """Return the name a call calls: `f` of `f()` and of `a.b.f()`."""
    if isinstance(func, ast.Name):
        return func.id
    if isinstance(func, ast.Attribute):
        return func.attr
    return None


def standard_stream(func):
    """Return true when a call is the write method of sys.stdout or stderr."""
    return (isinstance(func, ast.Attribute) and func.attr == "write"
            and isinstance(func.value, ast.Attribute)
            and func.value.attr in ("stdout", "stderr"))


def prose(tree):
    """Yield the string literals of a module that are prose.

    A docstring is any string written as a statement. A message is a string
    in the arguments of a call that MESSAGE_CALLS names or a write to a
    standard stream, in a keyword of MESSAGE_KEYWORDS, or in the exception a
    raise statement raises. Every other string is code or data, such as a
    command or a script the program generates."""
    for node in ast.walk(tree):
        if isinstance(node, ast.Expr):
            if isinstance(node.value, ast.Constant) and isinstance(node.value.value, str):
                yield node.value
        elif isinstance(node, ast.Raise) and node.exc is not None:
            yield from literals(node.exc)
        elif isinstance(node, ast.Call):
            if called(node.func) in MESSAGE_CALLS or standard_stream(node.func):
                for arg in node.args:
                    yield from literals(arg)
            for keyword in node.keywords:
                if keyword.arg in MESSAGE_KEYWORDS:
                    yield from literals(keyword.value)


def check(path):
    """Return the numbers of the lines of path with a dash in prose."""
    with open(path, encoding="ascii", errors="replace") as f:
        source = f.read()
    try:
        tree = ast.parse(source, path)
    except SyntaxError as e:
        print(f"{path}:{e.lineno or 0}:  [not parsed as Python]")
        return set()
    lines = set()
    for token in tokenize.generate_tokens(io.StringIO(source).readline):
        if token.type == tokenize.COMMENT and DASH in token.string:
            lines.add(token.start[0])
    for node in prose(tree):
        text = ast.get_source_segment(source, node) or ""
        at = text.find(DASH)
        while at >= 0:
            lines.add(node.lineno + text.count("\n", 0, at))
            at = text.find(DASH, at + 1)
    return lines


def main(paths):
    """Print each finding as `file:line:  [prose em-dash]`."""
    for path in paths:
        if not os.path.isfile(path):
            continue
        for line in sorted(check(path)):
            print(f"{path}:{line}:  [prose em-dash]")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.stdin.read().splitlines()))
