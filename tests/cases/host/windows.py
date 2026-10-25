"""Prepare the native Windows fixtures and every executable they start."""
from pathlib import Path
import re
import shlex
import shutil
import tools

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
    # The Windows scenario has a native serial actor for its stand-in row.
    # The real row still runs etude-guest inside the prepared Rach2 image.
    if 'tests/host/etude_scenario_hosttest.c' in sources:
        guest = {'tools/etude/guest/guest.c', 'tools/etude/guest/fixture.c',
                 'tools/etude/guest/owner.c'}
        sources = [s for s in sources if s not in guest]
        sources.append('tests/host/compat/scenario_guest.c')
    sources = list(dict.fromkeys([*sources, *source_list('ETUDE_WIN_EXEC_SRC')]))
    item['sources'] = sources
    item['command'] = [*FLAGS, '-DRACH2_HOSTTEST', *flags, '-o', item['program'], *sources, *LIBS]
    item['data'] = ['tools/etude/sources.mk', 'tests/cases/host/windows.py']
    item['instrumentation'] = []
    return item


def companion_recipes():
    """Keep every Windows fixture helper's sources and link operands explicit."""
    return [
        ('wintarget.exe', ['tools/etude/win/wintarget.c'], ['-municode', *LIBS]),
        ('winstand.exe', ['tools/etude/win/winstand.c', 'tools/etude/volume/volume.c'], []),
        ('etude.exe', source_list('ETUDE_WIN_SRC'), LIBS),
        ('etude-volume.exe', source_list('ETUDE_VOLUME_SRC'), []),
        ('image-command.exe', ['tools/etude/win/image_command.c', 'tools/etude/win/hold.c',
                               'tools/etude/win/winfmt.c'], []),
    ]


def companions(compiler, base, dependencies, digest, print_commands=False,
               compiler_identity=None):
    """Reuse helpers only after checking the full current recipe and bytes."""
    root = base / 'platform'
    inputs = {}
    for name, sources, libraries in companion_recipes():
        output = str(root / name)
        command = [*FLAGS, '-o', output, *sources, *libraries]
        inputs.update(tools.compiled_program(root, name, compiler, command, sources,
                                            dependencies, digest, compiler_identity,
                                            print_commands))
    if print_commands:
        return []
    schema = root / 'history.schema.json'
    shutil.copyfile('tools/etude/schemas/history.schema.json', schema)
    for path in (str(schema), 'tools/etude/schemas/history.schema.json'):
        inputs[path] = dict(path=path, sha256=digest(Path(path)))
    return [inputs[path] for path in sorted(inputs)]
