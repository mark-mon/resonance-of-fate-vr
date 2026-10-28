import struct, bisect, re, pefile
from capstone import *
BASE=0x7ff723fe0000
class Img:
    def __init__(s, path='/home/claude/rofvr/work/rof_dump.exe'):
        s.d=open(path,'rb').read()
        pe=pefile.PE(data=s.d[:0x1000],fast_load=True)
        s.secs=[(x.Name.rstrip(b'\0').decode('latin1'),x.VirtualAddress,x.Misc_VirtualSize) for x in pe.sections]
        s.md=Cs(CS_ARCH_X86,CS_MODE_64); s.md.detail=True
        s._funcs=None
    def sec(s,name):
        for n,va,sz in s.secs:
            if n.startswith(name): return va,sz
    def q(s,rva): return struct.unpack_from('<Q',s.d,rva)[0]
    def u32(s,rva): return struct.unpack_from('<I',s.d,rva)[0]
    def i32(s,rva): return struct.unpack_from('<i',s.d,rva)[0]
    def cstr(s,rva):
        e=s.d.index(b'\0',rva); return s.d[rva:e].decode('latin1')
    def funcs(s):
        if s._funcs is None:
            va,sz=s.sec('.pdata'); f=[]
            for o in range(va,va+sz,12):
                b,e,u=struct.unpack_from('<III',s.d,o)
                if b==0: break
                f.append((b,e))
            f.sort(); s._funcs=f; s._fb=[x[0] for x in f]
        return s._funcs
    def func_of(s,rva):
        s.funcs(); i=bisect.bisect_right(s._fb,rva)-1
        if i>=0 and s._funcs[i][0]<=rva<s._funcs[i][1]: return s._funcs[i]
    def dis(s,rva,n=40,size=None):
        out=[]
        for i,ins in enumerate(s.md.disasm(s.d[rva:rva+(size or n*15)],BASE+rva)):
            out.append(ins)
            if size is None and i+1>=n: break
        return out
    def ptrs_to(s,va,secs=None):
        pat=struct.pack('<Q',va); res=[]; i=0
        while True:
            i=s.d.find(pat,i)
            if i<0: break
            if i%8==0: res.append(i)
            i+=1
        return res
