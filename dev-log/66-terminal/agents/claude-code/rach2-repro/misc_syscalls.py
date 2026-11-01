# Assorted Linux-compat checks. Each line prints OK or the errno Linux would NOT return.
import os, socket, tempfile, ctypes, subprocess
libc = ctypes.CDLL(None, use_errno=True)
D = tempfile.mkdtemp(dir=os.getcwd()); src = os.path.join(D, 'src'); open(src, 'wb').write(os.urandom(1 << 16))
def t(name, f):
    try: print(f"OK    {name:40} {f()}")
    except Exception as e: print(f"FAIL  {name:40} {type(e).__name__}: {e}")
fd = os.open(src, os.O_RDONLY); pair = socket.socketpair()  # keep both ends referenced
t('sendfile file->socket (R2-017)', lambda: os.sendfile(pair[0].fileno(), fd, 0, 4096))
t('sendfile file->file (R2-017)', lambda: os.sendfile(os.open(os.path.join(D, 'o1'), os.O_WRONLY | os.O_CREAT), fd, 0, 4096))
t('splice file->pipe (R2-017)', lambda: os.splice(fd, os.pipe()[1], 4096))
t('copy_file_range (R2-017)', lambda: os.copy_file_range(fd, os.open(os.path.join(D, 'o2'), os.O_WRONLY | os.O_CREAT), 4096, 0))
t('socket(AF_INET6) (R2-018)', lambda: socket.socket(socket.AF_INET6).close())
t('lseek SEEK_DATA/SEEK_HOLE (R2-023)', lambda: (os.lseek(fd, 0, os.SEEK_DATA), os.lseek(fd, 0, os.SEEK_HOLE)))
t('O_TMPFILE (R2-023; Linux: OK or EOPNOTSUPP)', lambda: os.open(D, os.O_TMPFILE | os.O_RDWR))
t('setxattr user.* (R2-023)', lambda: os.setxattr(src, 'user.k', b'v'))
t('FICLONE via cp --reflink=always (R2-023; Linux errno EOPNOTSUPP)', lambda: subprocess.run(['cp', '--reflink=always', src, os.path.join(D, 'o3')], capture_output=True, text=True, check=True))
p = os.path.join(D, 'late'); open(p, 'w').close()
t('utime year 2040 (R2-023)', lambda: (os.utime(p, (2222222222, 2222222222)), os.stat(p).st_mtime)[1])
for nr, name in ((298, 'perf_event_open (R2-020)'), (425, 'io_uring_setup (R2-025)'), (434, 'pidfd_open (R2-025)'), (324, 'membarrier (R2-025)'), (444, 'landlock_create_ruleset (R2-025)')):
    def call(nr=nr):
        buf = ctypes.create_string_buffer(256); buf[4] = 128
        args = {298: (ctypes.byref(buf), 0, -1, -1, 0), 425: (4, buf), 434: (os.getpid(), 0), 324: (0, 0), 444: (None, 0, 1)}[nr]
        r = libc.syscall(nr, *args)
        if r < 0: raise OSError(ctypes.get_errno(), os.strerror(ctypes.get_errno()))
        return r
    t(name, call)
subprocess.run(['rm', '-r', D])
