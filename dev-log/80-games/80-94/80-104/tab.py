# Print each load's exits, time, halts, interrupt windows, hypercalls and
# unnamed exits from vm.py's readings. Usage: tab.py DIR NAME...
import re,sys,os
d=sys.argv[1]
for f in sys.argv[2:]:
    t=open(os.path.join(d,f+'.out')).read()
    rows=[]
    for m in re.finditer(r'^== (.*?)  ([\d.]+) s\n(?:.*\n)*?   exits=(\d+) halt_exits=(\d+).*?irq_window_exits=(\d+).*?hypercalls=(\d+).*unnamed=(\d+)',t,re.M):
        cmd=m.group(1)
        if cmd.startswith('cat') or cmd.startswith('sleep'): continue
        k='unmap' if 'munmap' in cmd else 'tar'
        rows.append('%s %s/%s h%s w%s hc%s u%s'%(k,m.group(3),m.group(2),m.group(4),m.group(5),m.group(6),m.group(7)))
    print(f); [print('   ',r) for r in rows]
