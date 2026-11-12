# Per-thread CPU over the load: ticks of user and system time between stat-a and stat-b.  Usage: tasks.py DIR
import re,sys
def rd(p):
    d={}
    for l in open(p):
        if not l.startswith('task '): continue
        path=l.split()[1]
        rest=l[len('task ')+len(path)+1:]
        m=re.match(r'(\d+) \((.*)\) (\S) (.*)',rest)
        if not m: continue
        f=m.group(4).split()
        d[path]=(m.group(2),int(f[10]),int(f[11]))
    return d
a=rd(sys.argv[1]+'/stat-a.txt'); b=rd(sys.argv[1]+'/stat-b.txt')
rows=[]
for k,(c,u,s) in b.items():
    u0,s0=(a[k][1],a[k][2]) if k in a else (0,0)
    rows.append((u-u0+s-s0,u-u0,s-s0,c,k))
rows.sort(reverse=True)
tu=sum(r[1] for r in rows); ts=sum(r[2] for r in rows)
print('total user %d sys %d'%(tu,ts))
for r in rows[:25]: print('%6d %6d %6d %-20s %s'%r)
