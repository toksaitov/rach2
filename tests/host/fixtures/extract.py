"""extract.py: copy definitions out of kernel sources unchanged, for a
fixture of tests/host/fixtures that compiles a function no host test can
compile whole.

Usage: python3 extract.py OUT -f FILE ITEM... [-f FILE ITEM...]...

Each ITEM is read from the FILE given before it and written to OUT in the
order given, separated by blank lines:

    NAME              the whole function NAME, from its first line to the
                      brace that closes its body;
    struct NAME       the definition of the structure NAME;
    #define REGEX     every #define line whose name REGEX matches at its
                      start, in the file's order.

A function or a structure that is not defined exactly once in its file, and
a #define that matches no line, stop the script with status 1 and write
nothing.
"""
import re
import sys

# A comment, a string or a character constant, whose braces do not count,
# or a brace.
TOKENS = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'


def fail(what):
    sys.stderr.write('extract: ' + what + '\n')
    sys.exit(1)


def function(text, name, path):
    """The text of function `name`, which `text` must define once."""
    heads = list(re.finditer(r'^[A-Za-z_][\w *]*\b' + re.escape(name) + r'\([^;]*?\n\{',
                             text, re.M))
    if len(heads) != 1:
        fail('%s: function %s is defined %d times' % (path, name, len(heads)))
    head = heads[0]
    depth = 0
    for token in re.finditer(TOKENS, text[head.end() - 1:], re.S):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return text[head.start():head.end() - 1 + token.end()]
    fail('%s: function %s does not end' % (path, name))


def structure(text, name, path):
    """The definition of structure `name`, which `text` must have once."""
    found = re.findall(r'^struct ' + re.escape(name) + r' \{.*?\n\};', text, re.M | re.S)
    if len(found) != 1:
        fail('%s: struct %s is defined %d times' % (path, name, len(found)))
    return found[0]


def defines(text, pattern, path):
    """Every #define line whose name `pattern` matches at its start."""
    found = re.findall(r'^#define (?:' + pattern + r').*', text, re.M)
    if not found:
        fail('%s: no #define matches %s' % (path, pattern))
    return '\n'.join(found)


def main(argv):
    if len(argv) < 4 or argv[1] != '-f':
        fail('usage: extract.py OUT -f FILE ITEM... [-f FILE ITEM...]...')
    out, parts, path, text = argv[0], [], None, None
    args = iter(argv[1:])
    for arg in args:
        if arg == '-f':
            path = next(args, None)
            if path is None:
                fail('-f needs a file')
            with open(path) as f:
                text = f.read()
        elif arg.startswith('struct '):
            parts.append(structure(text, arg[len('struct '):], path))
        elif arg.startswith('#define '):
            parts.append(defines(text, arg[len('#define '):], path))
        else:
            parts.append(function(text, arg, path))
    with open(out, 'w') as f:
        f.write('\n\n'.join(parts) + '\n')


main(sys.argv[1:])
