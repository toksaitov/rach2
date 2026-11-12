# phases.py: the phases of a vkcube round's start, each run alone, inside a
# graphics guest, Rach2's or Debian's, as root, in guest-env.sh's
# environment: the runner's GCC build of vkhold.c, Weston to its socket,
# vkcube to its first present and its end (`--c 1`), weston-screenshooter
# and frame.py, the way tests/graphics/runners/vkcube.sh runs each.
#
#   python3 -I phases.py RUNNERS OUT [N]
#
# RUNNERS is the directory of vkcube.sh, vkhold.c and frame.py. Each phase
# runs N times (5), the first of each kept apart as cold, and prints a line
#
#   phase NAME I wall S user S system S minflt N kernel S
#
# with its wall time, the user and system time and minor faults of the
# processes it started (getrusage of the children), and the system, irq and
# softirq time /proc/stat gives over the four processors meanwhile. OUT gets
# the lines in `phases`.
import os
import resource
import signal
import subprocess
import sys
import time

runners, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
count = int(sys.argv[3]) if len(sys.argv) > 3 else 5
work = os.path.join(out, 'phases-work')
os.makedirs(work, exist_ok=True)
lines = []


def kernel():
    """The system, irq and softirq ticks of /proc/stat's cpu line."""
    with open('/proc/stat') as f:
        v = f.readline().split()[1:]
    return int(v[2]) + int(v[5]) + int(v[6])


def measure(name, i, start):
    """Time one run of `start`, which runs the phase and reaps its children."""
    r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    k0, t0 = kernel(), time.monotonic()
    start()
    t1, k1 = time.monotonic(), kernel()
    r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    line = 'phase %s %d wall %.3f user %.3f system %.3f minflt %d kernel %.2f' % (
        name, i, t1 - t0, r1.ru_utime - r0.ru_utime, r1.ru_stime - r0.ru_stime,
        r1.ru_minflt - r0.ru_minflt, (k1 - k0) / 100.0)
    print(line, flush=True)
    lines.append(line)


def quiet(argv, env=None):
    subprocess.run(argv, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   env=env, cwd=work)


def westons():
    """End every running Weston, the boot session's among them."""
    for pid in os.listdir('/proc'):
        if pid.isdigit():
            try:
                with open('/proc/%s/comm' % pid) as f:
                    if f.read().strip() == 'weston':
                        os.kill(int(pid), signal.SIGTERM)
            except OSError:
                pass
    time.sleep(2)


env = dict(os.environ)
env['XDG_RUNTIME_DIR'] = os.path.join(work, 'run')
env['WAYLAND_DISPLAY'] = 'wayland-phases'
env['LIBSEAT_BACKEND'] = 'noop'
os.makedirs(env['XDG_RUNTIME_DIR'], mode=0o700, exist_ok=True)
sock = os.path.join(env['XDG_RUNTIME_DIR'], env['WAYLAND_DISPLAY'])
weston = ['weston', '--backend=drm', '--renderer=gl', '--shell=kiosk', '--idle-time=0',
          '--no-config', '--debug', '--socket=' + env['WAYLAND_DISPLAY'],
          '--log=' + os.path.join(work, 'weston.log')]
westons()

for i in range(count):
    measure('gcc', i, lambda: quiet(['gcc', '-std=c99', '-O2', '-shared', '-fPIC', '-o',
                                     os.path.join(work, 'libvkhold.so'),
                                     os.path.join(runners, 'vkhold.c')]))


def weston_up():
    """Start Weston and wait for its socket; the process, still running."""
    p = subprocess.Popen(weston, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, env=env, start_new_session=True)
    while not os.path.exists(sock):
        if p.poll() is not None:
            raise SystemExit('phases: weston ended')
        time.sleep(0.01)
    return p


def weston_down(p):
    p.terminate()
    p.wait()
    try:
        os.unlink(sock)
    except OSError:
        pass


for i in range(count):
    held = []
    measure('weston', i, lambda: held.append(weston_up()))
    weston_down(held[0])

p = weston_up()
try:
    for i in range(count):
        measure('vkcube', i, lambda: quiet(['vkcube', '--wsi', 'wayland', '--c', '1'], env))
    for i in range(count):
        shot = os.path.join(work, 'shot')
        os.makedirs(shot, exist_ok=True)
        for f in os.listdir(shot):
            os.unlink(os.path.join(shot, f))
        measure('screenshooter', i, lambda: subprocess.run(
            ['weston-screenshooter'], check=True, cwd=shot, env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    png = [os.path.join(shot, f) for f in os.listdir(shot) if f.endswith('.png')][0]
    for i in range(count):
        measure('frame', i, lambda: quiet(['python3', '-I', os.path.join(runners, 'frame.py'),
                                           png]))
finally:
    weston_down(p)

with open(os.path.join(out, 'phases'), 'w') as f:
    f.write('\n'.join(lines) + '\n')
