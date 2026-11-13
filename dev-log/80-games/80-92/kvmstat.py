# Run as root on the testing host beside crysis-load.py: for each of the
# harness's QEMU processes, keep KVM's counters for its VM
# (/sys/kernel/debug/kvm/PID-FD) in kvm-a.txt and kvm-b.txt of its out
# directory as soon as crysis-load.py writes host-a.txt and host-b.txt, which
# it does at the load's start and end. Debugfs names a VM by its PID outside
# WSL's PID namespace, which /proc here does not show, so a VM is taken for
# the harness's only while it is the one VM and the harness has one QEMU.
# Over the same window, histograms of the VM exits by reason and of the MSR
# accesses by number and direction go to kvm-hist.txt, from hist triggers
# on the kvm_exit and kvm_msr trace events, which count every VM's.
# Ends once the file STOP names holds STOPWORD, or on SIGINT, SIGTERM or
# SIGHUP, and removes the triggers it installed on every exit. A signal only
# records the cancellation, which the main loop reads, so that no second
# signal can interrupt the removal.
# Usage: kvmstat.py WORKDIR STOP STOPWORD
import os, pathlib, re, signal, sys, time

w, stop, word = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
kvm = pathlib.Path('/sys/kernel/debug/kvm')
done = set()
n = 0
tr = pathlib.Path('/sys/kernel/tracing/events/kvm')
hists = {'kvm_exit': 'hist:keys=exit_reason:sort=hitcount:size=256',
         'kvm_msr': 'hist:keys=ecx,write:sort=hitcount:size=256'}
installed = []  # the events whose trigger is this run's to remove
cancelled = 0  # the signal that ended the run, or 0


def trigger(ev, text):
    """Append TEXT to EV's trigger file; opening it to write over it would
    remove every trigger there."""
    with open(tr / ev / 'trigger', 'a') as f:
        f.write(text + '\n')


def hist_clear():
    for ev in installed:
        trigger(ev, hists[ev] + ':clear')


def hist_keep(path):
    path.write_text(''.join('== %s\n%s' % (ev, (tr / ev / 'hist').read_text()) for ev in hists))
    os.chown(path, os.stat(path.parent).st_uid, os.stat(path.parent).st_gid)


def vm_rows(d):
    """Give one line for each of KVM's counters of the VM D, or None once
    its directory has gone, as it does when QEMU ends."""
    rows = []
    try:
        for f in sorted(d.iterdir()):
            if f.is_file():
                try:
                    rows.append('%s %s\n' % (f.name, f.read_text().strip()))
                except OSError:
                    pass
    except OSError:
        return None
    return rows


def ended(sig, frame):
    """Record the first ending signal; raising here could abandon the
    triggers' removal."""
    global cancelled
    cancelled = cancelled or sig


end_signals = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
for s in end_signals:
    signal.signal(s, ended)

try:
    for ev, h in hists.items():
        if cancelled:
            break
        try:
            trigger(ev, h)
        except FileExistsError:
            pass  # left by a run that ended without removing it; ours to remove
        installed.append(ev)
    while not cancelled:
        vms = list(kvm.glob('*-*'))
        qemus = []
        for p in pathlib.Path('/proc').glob('[0-9]*'):
            try:
                cmd = (p / 'cmdline').read_bytes().decode(errors='replace')
            except OSError:
                continue
            m = re.search(r'qmp-([A-Za-z0-9-]+)\.sock', cmd)
            if m and cmd.startswith('qemu-system') and str(w) in cmd:
                qemus.append(m.group(1))
        for d in vms if len(vms) == 1 and len(qemus) == 1 else ():
            out = w / ('out-' + qemus[0])
            for side in 'ab':
                key = (d.name, side)
                if key in done or not (out / f'host-{side}.txt').exists():
                    continue
                rows = vm_rows(d)
                if rows is None:
                    break
                if side == 'a':
                    hist_clear()
                else:
                    hist_keep(out / 'kvm-hist.txt')
                (out / f'kvm-{side}.txt').write_text(''.join(rows))
                os.chown(out / f'kvm-{side}.txt', os.stat(out).st_uid, os.stat(out).st_gid)
                done.add(key)
        n += 1
        if n % 100 == 0 and stop.exists() and word in stop.read_text():
            break
        time.sleep(0.02)
finally:
    for ev in installed:
        try:
            trigger(ev, '!' + hists[ev])
        except OSError as e:
            sys.stderr.write('kvmstat: %s: trigger left installed: %s\n' % (ev, e))
if cancelled:
    sys.exit(128 + cancelled)
