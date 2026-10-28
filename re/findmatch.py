import pickle, collections, sys
from match import *
S=Img2('/home/claude/rofvr/work/so4_dump.exe',0x140000000)
db,inv=pickle.load(open('/home/claude/rofvr/work/rof_ng.pkl','rb'))
def best(so4rva, k=3, show=True):
    f=S.func_of(so4rva)
    if not f: print(hex(so4rva),'no so4 func'); return []
    t=S.toks(*f,3000); g=ngrams(t)
    cnt=collections.Counter()
    for h in g:
        l=inv.get(h)
        if l and len(l)<200:
            for b in l: cnt[b]+=1
    res=[]
    for b,c in cnt.most_common(40):
        n,gg=db[b]; j=c/len(g|gg) if g else 0
        res.append((j,b))
    res.sort(reverse=True)
    if show:
        print('SO4 %x (func %x..%x, off +%x, %d ins) ->'%(so4rva,f[0],f[1],so4rva-f[0],len(t)), ' '.join('%x(%.2f)'%(b,j) for j,b in res[:k]))
    return [(j,b,f) for j,b in res[:k]]
if __name__=='__main__':
    for a in sys.argv[1:]: best(int(a,16))
