import pickle, sys, collections
from lib import *
I=Img()
refs=pickle.load(open('/home/claude/rofvr/work/refs.pkl','rb'))
by_t=collections.defaultdict(list)
for a,t,k in refs: by_t[t].append((a,k))
def x(t):
    return by_t.get(t,[])
def fn(a):
    f=I.func_of(a); return f[0] if f else None
def dis(a,n=60):
    for ins in I.dis(a,n): print('  %x: %-8s %s'%(ins.address-BASE,ins.mnemonic,ins.op_str))
def disf(a,maxn=400):
    f=I.func_of(a); b,e=f
    k=0
    for ins in I.md.disasm(I.d[b:e],BASE+b):
        print('  %x: %-8s %s'%(ins.address-BASE,ins.mnemonic,ins.op_str)); k+=1
        if k>=maxn: print('  ...'); break
def calls_in(a):
    b,e=I.func_of(a); return [(s,t,k) for s,t,k in refs if b<=s<e and k=='c']
def r(absaddr): return absaddr-BASE   # absolute -> RVA
