# R2-011: IN_ATTRIB must be reported to BOTH a watch on the file and a watch on its parent directory.
import os, ctypes, struct, select, time, tempfile
libc = ctypes.CDLL(None, use_errno=True)
D = tempfile.mkdtemp(dir=os.getcwd()); f = os.path.join(D, 'x'); open(f, 'w').close()
fd = libc.inotify_init1(0)
wd = libc.inotify_add_watch(fd, D.encode(), 0xfff); wf = libc.inotify_add_watch(fd, f.encode(), 0xfff)
def events(action):
    action(); time.sleep(0.2); out = []
    while select.select([fd], [], [], 0.2)[0]:
        b = os.read(fd, 4096); i = 0
        while i < len(b):
            w, m, c, l = struct.unpack_from('iIII', b, i); out.append(('dir' if w == wd else 'file', m)); i += 16 + l
    return out
for name, act in [('chmod', lambda: os.chmod(f, 0o600)), ('touch', lambda: os.utime(f)), ('chown', lambda: os.chown(f, 0, 0))]:
    ev = events(act)
    fw = any(w == 'file' and m & 4 for w, m in ev); dw = any(w == 'dir' and m & 4 for w, m in ev)
    print(f"{name:6}: IN_ATTRIB on file-watch={'yes' if fw else 'NO'}  on dir-watch={'yes' if dw else 'NO'}   (Linux: yes / yes)")
for k in ('max_user_watches', 'max_user_instances', 'max_queued_events'):
    print(f"/proc/sys/fs/inotify/{k} = {open('/proc/sys/fs/inotify/'+k).read().strip()}")
