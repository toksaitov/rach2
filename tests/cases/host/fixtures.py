"""Prepare kernel function fixtures without running their behavioral checks."""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import sys
import routing

MODULE_PATH = Path('tests/host/fixtures/extract.py')
SPEC = importlib.util.spec_from_file_location('kernel_extract', MODULE_PATH)
EXTRACT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXTRACT)
SYS = 'rach2/sys/'
NAMES = ('chown', 'krffs_open', 'krffs_root_lock', 'krffs_stat', 'pagecache', 'kstack', 'endothers')


def extract(out, groups):
    """Keep the old extraction order and exact source text."""
    arguments = [str(out)]
    origins = []
    for source, items in groups:
        path = SYS + source
        arguments += ['-f', path, *([items] if items == '#define O_|CRED_|S_IF' else items.split('|'))]
        origins.append(path)
    EXTRACT.main(arguments)
    return origins


def platform_flags(common):
    """Use native Windows APIs without requesting unavailable sanitizer runtimes."""
    if not routing.windows_host():
        return list(common)
    flags = [flag for flag in common if not flag.startswith(('-fsanitize=', '-fno-sanitize-'))]
    return [*flags, '-D_WIN32_WINNT=0x0A00', '-static']


def recipe(name, base, common):
    """Return a compiler invocation, its inputs and the prepared program."""
    work = base / (name + '-sources')
    work.mkdir(parents=True, exist_ok=True)
    origins = [str(MODULE_PATH), 'tests/cases/host/fixtures.py']
    flags = [*platform_flags(common), '-Wall', '-Wextra', '-Werror', '-I', str(work)]
    source = 'tests/host/fixtures/' + name + '.c'
    deps = []
    # Kernel headers must not replace system headers included by pthreads.
    kernel = ['-DRACH2_HOSTTEST', '-ffreestanding', '-Wno-pointer-to-int-cast',
              '-Wno-int-to-pointer-cast', '-iquote', SYS[:-1], '-iquote', SYS + 'lib',
              '-iquote', SYS + 'proc']
    if name == 'chown':
        source = 'tests/host/fixtures/chown_plan.c'
        origins += extract(work / 'chown_plan.inc', [
            ('fs/vfs.h', '#define VFS_SET_|struct vfs_stat'),
            ('lib/kerrno.h', '#define EPERM'), ('proc/process.h', '#define proc_privileged'),
            ('syscall_fs.c', 'cred_privileged|cred_in_suppl|cred_in_group|vfs_chown_plan')])
        # The macro and structure do not depend on extraction order.
    elif name == 'kstack':
        origins += extract(work / 'kstack.inc', [
            ('proc/process.h', '#define KERNEL_STACK_SIZE'),
            ('arch/i686/cpu.h', '#define PAGE_SIZE'),
            ('syscall_proc.c', 'kstack_alloc|kstack_free|kstack_overrun_pid')])
    elif name == 'endothers':
        origins += extract(work / 'endothers.inc', [('syscall_proc.c', 'proc_end_others')])
    elif name == 'vfs_unmount':
        source = 'tests/system/fixtures/vfs_unmount.c'
        origins += extract(work / 'unmount.inc', [('fs/vfs.c',
            'streq|vfs_mount_gone|mount_base|mount_permanent|unmount_abandoned|unmount_target|'
            'unmount_below_gone|unmount_found|vfs_do_umount|unmount_bind|unmount_mount')])
    elif name == 'krffs_open':
        origins += extract(work / 'vfs_open.inc', [('fs/vfs.c',
            'vfs_mount_gone|mount_base|mount_rdonly|vfs_ref_hold|vfs_ref_put|vfs_enter|'
            'vfs_leave|ns_my_turn|vfs_ns_lock|vfs_ns_unlock|in_groups|may_search|toward_cwd|'
            'mount_covering|path_last|mount_named_at|mount_named|named_in_rest|climbs|node_put|'
            'walk_lock|walk_unlock|vfs_walk_unlock|ref_move_node|walk_move|walk_root|root_current|'
            'dotdot_stays|walk_leaf|walk_pages|walk_splice|walk_link|walk_last_comp|walk_is_link|'
            'walk_rest_len|walk_name_root|walk_name_add|walk_name_text|walk_name_end|resolve_path|'
            'vfs_resolve|vfs_ref_release|vfs_desc_open|vfs_desc_close|vfs_getattr|vfs_open_node|'
            'vfs_release|vfs_truncate|vfs_create')])
        origins += extract(work / 'syscall_open.inc', [
            ('syscall_fs.c', '#define O_|CRED_|S_IF'),
            ('syscall_internal.h', '#define CRED_'), ('syscall_fs.c',
             'name_is|device_name|open_device_name|open_link_target|node_make|open_create|'
             'open_mounted_node|path_starts|ofd_record_path|open_install|open_synth_node|do_open')])
        flags += kernel
        deps += [SYS + 'fs/fd.c', SYS + 'lib/kstring.c', SYS + 'lib/kctypes.c']
    elif name == 'krffs_root_lock':
        origins += extract(work / 'kfs_data.inc', [('fs/vfs.c',
            'struct ns_turn|mount_base|ns_my_turn|vfs_ns_lock|vfs_ns_unlock|kfs_read_locked|'
            'kfs_write_locked|kfs_truncate_locked|kfs_fsync_locked|kfs_readdir_locked|'
            'kfs_read|kfs_write|kfs_truncate|kfs_fsync|kfs_readdir')])
        flags += kernel
        deps += [SYS + 'lib/kstring.c', SYS + 'lib/kctypes.c']
    elif name == 'krffs_stat':
        origins += extract(work / 'krffs_stat_types.inc', [
            ('arch/i686/uabi.h', 'struct linux_stat|struct user_stat'),
            ('fs/vfs.h', 'struct vfs_stat'), ('arch/i686/uabi.h', '#define STAT_SET_INO'),
            ('syscall_fs.c', 'struct kstat')])
        origins += extract(work / 'krffs_stat_funcs.inc', [('syscall_fs.c',
            'path_names_dir|stat64_put|stat64_node_times|stat64_root_times|krffs_fill_stat64|'
            'stat_times_from_stat64|stat_node_times|stat_root_times|do_stat64|old_stat_put|do_old_stat'),
            ('fs/vfs.c', 'kfs_getattr')])
        flags += kernel
        # The original stat fixture deliberately did not enable extra warnings.
        flags = [word for word in flags if word not in ('-Wall', '-Wextra', '-Werror')]
        deps += [SYS + 'lib/kstring.c', SYS + 'lib/kctypes.c']
    elif name == 'pagecache':
        origins += extract(work / 'syscall_sync.inc', [('syscall_fs.c', 'sys_sync')])
        source = 'tests/host/pagecache_hosttest.c'
        flags += [*kernel, '-DPAGECACHE_SYNC_HOSTTEST', '-Wno-empty-body',
                  '-I', SYS + 'arch/x86', '-I', SYS + 'arch/i686',
                  '-I', 'utils', '-I', 'krffs', '-I', 'tests/host/compat']
    else:
        raise ValueError('unknown extracted kernel fixture: ' + name)
    if name in ('krffs_open', 'krffs_root_lock'):
        deps += ['tests/host/fixtures/krffs_open_threads.c']
        if not routing.windows_host():
            flags += ['-pthread']
    program = str(base / name)
    sources = [source, *deps]
    return [*flags, '-o', program, *sources], sources, origins, program


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: fixtures.py NAME OUTPUT')
    output = Path(sys.argv[2])
    command, _, _, program = recipe(sys.argv[1], output.parent, [
        '-std=c99', '-D_DEFAULT_SOURCE', '-D_FORTIFY_SOURCE=0',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g', '-O1'])
    if program != str(output):
        raise SystemExit('output basename must equal the fixture name')
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), *command], check=True)
    if not output.exists() and Path(str(output) + '.exe').is_file():
        Path(str(output) + '.exe').replace(output)
