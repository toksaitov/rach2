#!/usr/bin/env python3
"""Small, bounded compatibility smoke test; uses only disposable local data."""
import json
import pathlib
import subprocess
import sys
import tempfile
import time

PROBES = {
    "filesystem": r'''
import os, pathlib
p = pathlib.Path('original'); p.write_bytes(b'hello Rach2')
with p.open('r+b') as f: os.fsync(f.fileno())
os.link(p, 'hard'); os.symlink('original', 'sym')
assert pathlib.Path('sym').read_bytes() == b'hello Rach2'
assert os.stat(p).st_ino == os.stat('hard').st_ino
os.replace(p, 'renamed')
assert pathlib.Path('hard').read_bytes() == pathlib.Path('renamed').read_bytes()
fd = os.open('.', os.O_RDONLY | os.O_DIRECTORY)
try: os.fsync(fd)
finally: os.close(fd)
''',
    "mmap": r'''
import mmap
with open('mapped', 'w+b') as f:
    f.truncate(4096)
    with mmap.mmap(f.fileno(), 4096) as m:
        m[:4] = b'Rach'; m.flush()
    f.seek(0); assert f.read(4) == b'Rach'
''',
    "threads": r'''
from concurrent.futures import ThreadPoolExecutor
import threading
barrier = threading.Barrier(4, timeout=3)
def task(i):
    barrier.wait(); return i * i
with ThreadPoolExecutor(max_workers=4) as pool:
    assert list(pool.map(task, range(4))) == [0, 1, 4, 9]
''',
    "processes_and_signals": r'''
import signal, subprocess, sys
p = subprocess.run(['sh', '-c', 'printf rach2 | tr a-z A-Z'], capture_output=True, timeout=3)
assert p.returncode == 0 and p.stdout == b'RACH2'
p = subprocess.Popen([sys.executable, '-u', '-c', 'import time; print("ready"); time.sleep(30)'], stdout=subprocess.PIPE)
try:
    assert p.stdout.readline().strip() == b'ready'
    p.terminate(); assert p.wait(timeout=3) == -signal.SIGTERM
finally:
    if p.poll() is None: p.kill(); p.wait()
''',
    "epoll_pipe": r'''
import os, select
r, w = os.pipe()
try:
    with select.epoll() as ep:
        ep.register(r, select.EPOLLIN); os.write(w, b'x')
        assert ep.poll(1) == [(r, select.EPOLLIN)]
        assert os.read(r, 1) == b'x'
finally: os.close(r); os.close(w)
''',
    "unix_sockets": r'''
import socket
with socket.socket(socket.AF_UNIX) as server, socket.socket(socket.AF_UNIX) as client:
    server.settimeout(2); client.settimeout(2)
    server.bind('test.sock'); server.listen(1); client.connect('test.sock')
    conn, _ = server.accept()
    with conn:
        conn.settimeout(2); client.sendall(b'hello'); assert conn.recv(5) == b'hello'
''',
    "tcp_loopback": r'''
import socket
with socket.socket() as server, socket.socket() as client:
    server.settimeout(2); client.settimeout(2)
    server.bind(('127.0.0.1', 0)); server.listen(1); client.connect(server.getsockname())
    conn, _ = server.accept()
    with conn:
        conn.settimeout(2); client.sendall(b'hello'); assert conn.recv(5) == b'hello'
''',
    "file_locking": r'''
import fcntl, subprocess, sys
with open('locked', 'w') as f:
    fcntl.flock(f, fcntl.LOCK_EX)
    code = "import fcntl,sys\nf=open('locked','r+')\ntry: fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)\nexcept BlockingIOError: sys.exit(0)\nsys.exit(1)"
    assert subprocess.run([sys.executable, '-c', code], timeout=3).returncode == 0
''',
    "sqlite_wal": r'''
import sqlite3
with sqlite3.connect('test.db') as db:
    assert db.execute('PRAGMA journal_mode=WAL').fetchone()[0] == 'wal'
    db.execute('CREATE TABLE t (x INTEGER)'); db.execute('INSERT INTO t VALUES (42)'); db.commit()
    with sqlite3.connect('test.db') as reader:
        assert reader.execute('SELECT x FROM t').fetchone() == (42,)
    db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
''',
    "random_and_clocks": r'''
import os, time
assert len(os.getrandom(32, os.GRND_NONBLOCK)) == 32
start = time.monotonic(); time.sleep(0.03)
assert time.monotonic() >= start + 0.02
''',
    "memfd_and_eventfd": r'''
import os
fd = os.memfd_create('discovery')
try:
    os.write(fd, b'hello'); os.lseek(fd, 0, 0); assert os.read(fd, 5) == b'hello'
finally: os.close(fd)
fd = os.eventfd(0, os.EFD_NONBLOCK)
try:
    os.eventfd_write(fd, 3); assert os.eventfd_read(fd) == 3
finally: os.close(fd)
''',
    "pty": r'''
import os, pty, termios
master, slave = pty.openpty()
try:
    assert os.isatty(slave); termios.tcgetattr(slave)
    os.write(slave, b'hello\n'); assert os.read(master, 64) == b'hello\r\n'
finally: os.close(master); os.close(slave)
''',
    "gcc_pthreads": r'''
import pathlib, subprocess
pathlib.Path('hello.c').write_text('#include <pthread.h>\n#include <stdio.h>\nstatic void *work(void *p) { return p; }\nint main(void) { pthread_t t; int x=42; void *r=0; if(pthread_create(&t,0,work,&x)) return 2; if(pthread_join(t,&r)) return 3; if(r != &x) return 4; puts("C pthread OK"); return 0; }\n')
subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', '-pthread', 'hello.c', '-o', 'hello'], check=True, timeout=10)
p = subprocess.run(['./hello'], check=True, capture_output=True, timeout=3)
assert p.stdout == b'C pthread OK\n'
''',
    "git": r'''
import pathlib, subprocess
def git(*args): return subprocess.run(['git', *args], check=True, capture_output=True, timeout=5).stdout
git('init', '-q'); pathlib.Path('hello').write_text('Rach2\n'); git('add', 'hello')
git('-c', 'user.name=Discovery', '-c', 'user.email=discovery@localhost', '-c', 'commit.gpgsign=false', 'commit', '-qm', 'local smoke test')
assert git('show', 'HEAD:hello') == b'Rach2\n'
git('fsck', '--full'); assert git('status', '--porcelain') == b''
''',
}

def main():
    selected = sys.argv[1:] or list(PROBES)
    failures = 0
    for name in selected:
        start = time.monotonic()
        with tempfile.TemporaryDirectory(prefix='rach2-probe-') as scratch:
            try:
                p = subprocess.run([sys.executable, '-c', PROBES[name]], cwd=scratch,
                                   capture_output=True, text=True, timeout=15)
                result = dict(probe=name, status='PASS' if p.returncode == 0 else 'FAIL',
                              exit_code=p.returncode, stdout=p.stdout, stderr=p.stderr)
            except subprocess.TimeoutExpired as e:
                result = dict(probe=name, status='TIMEOUT', detail=str(e))
            failures += result['status'] != 'PASS'
            result['seconds'] = round(time.monotonic() - start, 3)
            print(json.dumps(result), flush=True)
    return bool(failures)

if __name__ == '__main__':
    sys.exit(main())
