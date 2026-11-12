# Samples of one entry by innermost function and by caller chain.  Usage: prof2.py NM PROFILE ENTRY [N]
import bisect,collections,re,sys
syms=[]
for l in open(sys.argv[1]):
    p=l.split()
    if len(p)==3 and p[1] in 'tTwW': syms.append((int(p[0],16),p[2]))
syms.sort(); addrs=[a for a,_ in syms]
def fn(a):
    a=int(a,16)
    if a==0: return '-'
    i=bisect.bisect_right(addrs,a)-1
    return syms[i][1] if i>=0 else hex(a)
names={}
for l in open('build/x86_64/vendor-work/musl/install/include/bits/syscall.h'):
    m=re.match(r'#define __NR_(\w+)\s+(\d+)',l)
    if m: names[int(m.group(2))]=m.group(1)
want=sys.argv[3]; N=int(sys.argv[4]) if len(sys.argv)>4 else 25
anyf=collections.Counter(); chain=collections.Counter(); tot=0
for l in open(sys.argv[2]):
    p=l.split()
    if not p or p[0] in ('samples','lost'): continue
    e=names.get(int(p[1]),p[1]) if p[0]=='syscall' else p[0]+' '+p[1]
    if e!=want: continue
    pcs=p[2:6]; n=int(p[-1]); tot+=n
    fs=[fn(x) for x in pcs]
    chain[' < '.join(fs)]+=n
    for f in set(fs): anyf[f]+=n
print('total',tot)
print('-- functions anywhere in the chain')
for f,n in anyf.most_common(N): print('%6d %s'%(n,f))
print('-- chains')
for f,n in chain.most_common(N): print('%6d %s'%(n,f))
