# Map old RoF functions to EE functions: vtable slot alignment (RTTI classes) + call-graph propagation.
import json, pickle, collections, struct, bisect
from match import Img2
O=Img2('/home/claude/rofvr/work/rof_dump.exe',0x7ff723fe0000)
E=Img2('/home/claude/rofvr/work_ee/ee_disk_mapped.exe',0x140000000)
vm=json.load(open('/home/claude/rofvr/work_ee/vt_map.json'))
def textrange(I):
    for n,va,sz in I.secs:
        if n=='.text': return va,va+sz
OT=textrange(O); ET=textrange(E)
def q(I,a): return struct.unpack_from('<Q',I.d,a)[0]
def ninsn(I,f):
    return sum(1 for _ in I.md.disasm(I.d[f[0]:f[1]],I.base+f[0]))
votes=collections.defaultdict(collections.Counter)
for ko,ke in vm.items():
    o=int(ko,16); e=int(ke,16)
    so=[];se=[]
    for i in range(400):
        a=q(O,o+8*i)-O.base
        if not (OT[0]<=a<OT[1]): break
        so.append(a)
    for i in range(400):
        a=q(E,e+8*i)-E.base
        if not (ET[0]<=a<ET[1]): break
        se.append(a)
    if len(so)==len(se):
        for a,b in zip(so,se): votes[a][b]+=1
    else:   # engine base classes lost a slot (job vt 0x248 -> 0x240): align from the end too
        d=len(so)-len(se)
        for a,b in zip(so[::-1],se[::-1]): votes[a][b]+=0.5
mp={}
for a,c in votes.items():
    b,n=c.most_common(1)[0]
    if n>=1: mp[a]=b
print('vtable-derived pairs',len(mp))
def calls(I,f):
    out=[]
    for ins in I.md.disasm(I.d[f[0]:f[1]],I.base+f[0]):
        if ins.mnemonic=='call' and ins.op_str.startswith('0x'):
            out.append(int(ins.op_str,16)-I.base)
    return out
for it in range(6):
    added=0
    for a,b in list(mp.items()):
        fa=O.func_of(a); fb=E.func_of(b)
        if not fa or not fb or fa[0]!=a or fb[0]!=b: continue
        ca=calls(O,fa); cb=calls(E,fb)
        if len(ca)==len(cb) and ca:
            for x,y in zip(ca,cb):
                if x not in mp: mp[x]=y; added+=1
    print('iteration',it,'added',added,'total',len(mp))
    if not added: break
pickle.dump(mp,open('/home/claude/rofvr/work_ee/fmap.pkl','wb'))
