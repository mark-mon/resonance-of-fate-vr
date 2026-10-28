import sys, numpy as np
from lib import *
def callers(I, target_rva):
    va,sz=I.sec('.data1'); d=np.frombuffer(I.d[va:va+sz],dtype=np.uint8)
    idx=np.nonzero(d[:-5]==0xE8)[0]
    disp=np.frombuffer(I.d[va:va+sz],dtype=np.uint8)
    b=np.stack([d[idx+1],d[idx+2],d[idx+3],d[idx+4]]).astype(np.int64)
    rel=b[0]|(b[1]<<8)|(b[2]<<16)|(b[3]<<24); rel=np.where(rel>=2**31,rel-2**32,rel)
    tgt=idx+va+5+rel
    res=[int(x)+va for x in idx[tgt==target_rva]]
    # also jmp E9
    idx2=np.nonzero(d[:-5]==0xE9)[0]
    b=np.stack([d[idx2+1],d[idx2+2],d[idx2+3],d[idx2+4]]).astype(np.int64)
    rel=b[0]|(b[1]<<8)|(b[2]<<16)|(b[3]<<24); rel=np.where(rel>=2**31,rel-2**32,rel)
    res+= [int(x)+va for x in idx2[idx2+va+5+rel==target_rva]]
    return res
if __name__=='__main__':
    I=Img()
    for t in sys.argv[1:]:
        t=int(t,16); t=t-BASE if t>=BASE else t
        print('callers of',hex(t+BASE))
        for c in callers(I,t):
            f=I.func_of(c); print('  ',hex(c+BASE),'in',hex(f[0]+BASE) if f else None)
