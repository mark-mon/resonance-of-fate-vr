# Build xref index: for every .pdata function, linear-disassemble; record rip-relative refs and direct calls/jmps.
import pickle, struct
from capstone import *
from capstone.x86 import *
from lib import *
I=Img(); d=I.d
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True; md.skipdata=True
refs=[]  # (insn_rva, target_rva, kind) kind: 'c' call,'j' jmp,'m' mem
funcs=I.funcs()
seen=set()
for b,e in funcs:
    if b in seen: continue
    seen.add(b)
    for ins in md.disasm(d[b:e], BASE+b):
        a=ins.address-BASE
        if ins.id==0: continue
        if ins.id in (X86_INS_CALL,X86_INS_JMP) or ins.group(X86_GRP_JUMP):
            op=ins.operands[0] if ins.operands else None
            if op is not None and op.type==X86_OP_IMM:
                refs.append((a,op.imm-BASE,'c' if ins.id==X86_INS_CALL else 'j')); continue
        for op in ins.operands:
            if op.type==X86_OP_MEM and op.mem.base==X86_REG_RIP:
                refs.append((a,ins.address+ins.size+op.mem.disp-BASE,'m'))
print(len(funcs),'funcs',len(refs),'refs')
pickle.dump(refs,open('/home/claude/rofvr/work/refs.pkl','wb'))
