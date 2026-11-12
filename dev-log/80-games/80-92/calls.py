# Rank bkl/calls by wall time.  Usage: calls.py FILE
import re,sys
names={}
for l in open('build/x86_64/vendor-work/musl/install/include/bits/syscall.h'):
    m=re.match(r'#define __NR_(\w+)\s+(\d+)',l)
    if m: names[int(m.group(2))]=m.group(1)
rows=[]
for l in open(sys.argv[1]):
    p=l.split()
    if len(p)<7 or p[0]=='kind': continue
    nm=names.get(int(p[1]),p[1]) if p[0]=='syscall' else p[0]+' '+p[1]
    calls,wall,holds,held,wait=map(int,p[2:7])
    rows.append((wall if wall else held+wait,nm,calls,holds,held,wait))
rows.sort(reverse=True)
print('%-22s %9s %11s %9s %9s %9s %8s'%('entry','calls','wall_ms','held_ms','wait_ms','us/call','held/c'))
for wall,nm,calls,holds,held,wait in rows[:int(sys.argv[2]) if len(sys.argv)>2 else 50]:
    print('%-22s %9d %11.1f %9.1f %9.1f %9.1f %8.1f'%(nm,calls,wall/1e3,held/1e3,wait/1e3,wall/max(calls,1),held/max(calls,1)))
