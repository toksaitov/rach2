# deqp-run.py OUT [STEP] [JOBS]: every STEP-th case (500 by default) of
# VK-GL-CTS's vk-default mustpass list on Venus, in JOBS deqp-vk processes (4),
# each given every JOBS-th case of the sample. A process that dies or prints
# nothing for 300 s is restarted after its case, which is counted as Crash or
# Timeout. Writes OUT/results.csv (case,status), OUT/summary.txt and each
# process's log; the .qpa files go to OUT/qpa/.
import collections, os, subprocess, sys, threading, time

CTS = os.path.expanduser('~/cts')
BIN = CTS + '/build/external/vulkancts/modules/vulkan'
MUST = CTS + '/external/vulkancts/mustpass/main'
out = sys.argv[1]
step = int(sys.argv[2]) if len(sys.argv) > 2 else 500
jobs = int(sys.argv[3]) if len(sys.argv) > 3 else 4
os.makedirs(out + '/qpa', exist_ok=True)


def cases(path):
    """Expand a mustpass file, following its include lines."""
    for line in open(path):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if line.endswith('.txt'):
            yield from cases(os.path.join(os.path.dirname(path), line))
        else:
            yield line


every = list(cases(MUST + '/vk-default.txt'))
sample = every[::step]
results = {}
lock = threading.Lock()


def worker(j, todo):
    """Run the cases in todo, restarting deqp-vk after a crash or a hang."""
    log = open('%s/job%d.log' % (out, j), 'w')
    n = 0
    while todo:
        n += 1
        lst = '%s/job%d-%d.txt' % (out, j, n)
        open(lst, 'w').write('\n'.join(todo) + '\n')
        p = subprocess.Popen(
            [BIN + '/deqp-vk', '--deqp-caselist-file=' + lst,
             '--deqp-log-filename=%s/qpa/job%d-%d.qpa' % (out, j, n),
             '--deqp-log-images=disable', '--deqp-log-shader-sources=disable',
             '--deqp-surface-type=window', '--deqp-wsi-type=wayland'],
            cwd=BIN, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        last = [time.time()]
        cur = [None]

        def watch():
            while p.poll() is None:
                if time.time() - last[0] > 300:
                    p.kill()
                    return
                time.sleep(5)
        threading.Thread(target=watch, daemon=True).start()
        for line in p.stdout:
            last[0] = time.time()
            log.write(line)
            if line.startswith("Test case '"):
                cur[0] = line[11:line.rindex("'")]
            elif cur[0] and line.startswith('  ') and line.strip():
                st = line.split()[0]
                with lock:
                    results[cur[0]] = st
                todo.remove(cur[0])
                cur[0] = None
        p.wait()
        if cur[0]:
            with lock:
                results[cur[0]] = 'Timeout' if p.returncode == -9 else 'Crash'
            todo.remove(cur[0])
        elif todo and p.returncode is not None:
            # Ended between cases without reaching the rest: count the next.
            with lock:
                results[todo[0]] = 'Crash'
            todo.pop(0)
    log.close()


t0 = time.time()
ts = [threading.Thread(target=worker, args=(j, sample[j::jobs])) for j in range(jobs)]
for t in ts:
    t.start()
for t in ts:
    t.join()
wall = time.time() - t0
with open(out + '/results.csv', 'w') as f:
    for c in sample:
        f.write('%s,%s\n' % (c, results.get(c, 'Missing')))
count = collections.Counter(results.get(c, 'Missing') for c in sample)
with open(out + '/summary.txt', 'w') as f:
    f.write('cases %d of %d (every %dth), wall %.1f s, jobs %d\n' %
            (len(sample), len(every), step, wall, jobs))
    for k in sorted(count):
        f.write('%s %d\n' % (k, count[k]))
print(open(out + '/summary.txt').read(), end='')
