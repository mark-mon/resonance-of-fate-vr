# Map an SO4 vtable to ROF: match the job function (slot 0x250/8 in SO4) and other slots, then find the ROF vtable(s) holding it
import struct, sys, collections
from findmatch import best, S
from match import Img2
R=Img2('/home/claude/rofvr/work/rof_dump.exe',0x7ff723fe0000)
q=lambda I,o: struct.unpack_from('<Q',I.d,o)[0]
def rof_vtables_with(fn_rva, slot):
    target=R.base+fn_rva; res=[]
    rd=[(v,z) for n,v,z in R.secs if n=='.rdata'][0]
    for o in range(rd[0],rd[0]+rd[1]-8,8):
        if q(R,o)==target:
            vt=o-slot*8
            res.append(vt)
    return res
def map_vt(so4vt, slots=(0x250//8,0x258//8,0x248//8,0)):
    print('SO4 vt %x'%so4vt)
    votes=collections.Counter()
    for sl in slots:
        fn=q(S,so4vt+sl*8)-0x140000000
        r=best(fn,3,show=True)
        for j,b,f in r[:2]:
            if j<0.2: continue
            for v in rof_vtables_with(b, sl-1 if sl>0 else 0): votes[v]+=j
            for v in rof_vtables_with(b, sl): votes[v]+=j*0.5
    print('  ->',[(hex(v),round(c,2)) for v,c in votes.most_common(4)])
if __name__=='__main__':
    for a in sys.argv[1:]: map_vt(int(a,16))
