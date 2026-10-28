import struct, sys
from lib import *
def xrefs(I, target_rva, sec='.data1'):
    va,sz=I.sec(sec); d=I.d; res=[]
    import numpy as np
    arr=np.frombuffer(d[va:va+sz],dtype=np.uint8)
    # disp32 at position p: target = p+4+disp (+imm size for some forms; check via disasm)
    for extra in (0,1,4):
        pos=np.arange(0,sz-4)
        disp=np.frombuffer(d[va:va+sz],dtype='<i4',count=1) # placeholder
    out=set()
    dv=np.lib.stride_tricks.sliding_window_view(arr,4)[:,::-1]
    disp=(dv[:,0].astype(np.int64)<<24|dv[:,1].astype(np.int64)<<16|dv[:,2].astype(np.int64)<<8|dv[:,3].astype(np.int64))
    disp=np.where(disp>=2**31,disp-2**32,disp)
    p=np.arange(len(disp))+va
    for extra in (0,1,2,4):
        cand=np.nonzero(p+4+extra+disp==target_rva)[0]
        for c in cand:
            prva=int(c)+va
            for back in range(1,8):
                st=prva-back
                ins=I.dis(st,1)
                if ins and ins[0].address-BASE==st and st+ins[0].size==prva+4+extra:
                    out.add((st,ins[0].mnemonic+' '+ins[0].op_str)); break
    return sorted(out)
if __name__=='__main__':
    I=Img()
    for t in sys.argv[1:]:
        t=int(t,16); t = t-BASE if t>=BASE else t
        print('xrefs to',hex(t+BASE))
        for st,txt in xrefs(I,t):
            f=I.func_of(st); print('  ',hex(st+BASE),txt,' func',hex(f[0]+BASE) if f else None)
