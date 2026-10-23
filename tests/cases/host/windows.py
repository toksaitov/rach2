"""Prepare the native Windows fixtures and every executable they start."""
from pathlib import Path
import re
import shlex
import shutil
import subprocess

FLAGS = ['-std=c99', '-O2', '-D_DEFAULT_SOURCE', '-D_WIN32_WINNT=0x0A00', '-static',
         '-Itools/etude/include', '-Itools/etude', '-Itools/etude/win', '-Iutils',
         '-Itests/host/compat']
LIBS = ['-lws2_32', '-lbcrypt']


def source_list(name):
    """Read the shared Make source lists, including their variable references."""
    text = Path('tools/etude/sources.mk').read_text().replace('\\\n', ' ')
    values = dict(re.findall(r'^(\w+)\s*=\s*(.*?)$', text, re.MULTILINE))

    def expand(value):
        return re.sub(r'\$\((\w+)\)', lambda m: expand(values[m[1]]), value)
    return shlex.split(expand(values[name]))


def platform_entry(base, entry):
    """The common platform assertion executes the Windows behavior fixtures."""
    root = base / 'platform'
    program = str(root / 'winfixture.exe')
    sources = source_list('ETUDE_WIN_FIXTURE_SRC')
    item = entry('etude_platform', 'native Windows execution and platform contracts',
                 program, sources, alone=True)
    item['command'] = [*FLAGS, '-o', program, *sources, *LIBS]
    item['windows_fixture'] = True
    item['instrumentation'] = []
    item['data'] = ['tools/etude/sources.mk', 'tests/cases/host/windows.py']
    return item


def boot_entry(item, flags):
    """Use the native executor and state the installed compiler's instrumentation."""
    posix = {'tools/etude/posix/exec.c', 'tools/etude/posix/keeper.c',
             'tools/etude/posix/process.c', 'tools/etude/capture.c'}
    sources = [('tools/etude/win/bridge.c' if s == 'tools/etude/posix/bridge.c' else s)
               for s in item['sources'] if s not in posix]
    sources = list(dict.fromkeys([*sources, *source_list('ETUDE_WIN_EXEC_SRC')]))
    item['sources'] = sources
    item['command'] = [*FLAGS, '-DRACH2_HOSTTEST', *flags, '-o', item['program'], *sources, *LIBS]
    item['data'] = ['tools/etude/sources.mk', 'tests/cases/host/windows.py']
    item['instrumentation'] = []
    return item


def companions(compiler, base, dependencies, digest, print_commands=False):
    """Compile helpers without executing them; hash sources and resulting bytes."""
    root = base / 'platform'
    recipes = [
        ('wintarget.exe', ['tools/etude/win/wintarget.c'], ['-municode', *LIBS]),
        ('winstand.exe', ['tools/etude/win/winstand.c', 'tools/etude/volume/volume.c'], []),
        ('etude.exe', source_list('ETUDE_WIN_SRC'), LIBS),
        ('etude-volume.exe', source_list('ETUDE_VOLUME_SRC'), []),
        ('image-command.exe', ['tools/etude/win/image_command.c', 'tools/etude/win/hold.c',
                               'tools/etude/win/winfmt.c'], []),
    ]
    inputs = set()
    if not print_commands:
        root.mkdir(parents=True, exist_ok=True)
    for name, sources, libraries in recipes:
        output = str(root / name)
        command = [*FLAGS, '-o', output, *sources, *libraries]
        print('host prepare: ' + shlex.join([*compiler, *command]), flush=True)
        if print_commands:
            continue
        subprocess.run([*compiler, *command], check=True)
        inputs.update([output, *sources, *dependencies(compiler, command, sources)])
    if print_commands:
        return []
    schema = root / 'history.schema.json'
    shutil.copyfile('tools/etude/schemas/history.schema.json', schema)
    inputs.update([str(schema), 'tools/etude/schemas/history.schema.json'])
    return [dict(path=p, sha256=digest(Path(p))) for p in sorted(inputs)]
