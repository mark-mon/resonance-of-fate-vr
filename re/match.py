import struct, bisect, pickle, os, collections, sys
import pefile
from capstone import *
from capstone.x86 import *
class Img2:
    def __init__(s, path, base):
        s.d=open(path,'rb').read(); s.base=base
        pe=pefile.PE(data=s.d[:0x1000],fast_load=True)
        s.secs=[(x.Name.rstrip(b'\0').decode('latin1'),x.VirtualAddress,x.Misc_VirtualSize) for x in pe.sections]
        va,sz=[(v,z) for n,v,z in s.secs if n=='.pdata'][0]
        f=[]
        for o in range(va,va+sz,12):
            b,e,u=struct.unpack_from('<III',s.d,o)
            if b==0: break
            f.append((b,e))
        s.funcs=sorted(set(f)); s.fb=[x[0] for x in s.funcs]
        s.md=Cs(CS_ARCH_X86,CS_MODE_64); s.md.detail=True
    def func_of(s,rva):
        i=bisect.bisect_right(s.fb,rva)-1
        if i>=0 and s.funcs[i][0]<=rva<s.funcs[i][1]: return s.funcs[i]
    def toks(s,b,e,limit=4000):
        out=[]
        for ins in s.md.disasm(s.d[b:e],s.base+b):
            if ins.id==0: continue
            t=[ins.mnemonic]
            for op in ins.operands:
                if op.type==X86_OP_REG: t.append('r')
                elif op.type==X86_OP_IMM:
                    v=op.imm
                    t.append('i%x'%v if (0<=v<0x10000 and ins.id not in (X86_INS_CALL,X86_INS_JMP) and not ins.group(X86_GRP_JUMP)) else 'I')
                elif op.type==X86_OP_MEM:
                    if op.mem.base==X86_REG_RIP: t.append('mR')
                    else: t.append('m%x'%(op.mem.disp & 0xffffffff) if abs(op.mem.disp)<0x100000 else 'mD')
            out.append(' '.join(t))
            if len(out)>=limit: break
        return out
import zlib
def ngrams(t,n=4): return set(zlib.crc32('|'.join(t[i:i+n]).encode()) for i in range(max(1,len(t)-n+1)))
