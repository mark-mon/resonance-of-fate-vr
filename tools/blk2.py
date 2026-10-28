import struct, sys, numpy as np, collections
d=open(sys.argv[1],'rb').read(); o=28
nu=struct.unpack_from('<I',d,4)[0]; nr=struct.unpack_from('<I',d,8)[0]; o+=nr*472
ups=[]
for i in range(nu):
    buf,by,bb,how,off=struct.unpack_from('<QIIII',d,o); rva=struct.unpack_from('<4I',d,o+24); o+=40; ups.append((buf,by,bb,how,off,rva))
nd=struct.unpack_from('<I',d,o)[0]; o+=4; data=d[o:o+nd]
size=int(sys.argv[2]) if len(sys.argv)>2 else 2208
for ui,(buf,by,bb,how,off,rva) in enumerate(ups):
    if by!=size: continue
    a=np.frombuffer(data,dtype='<f4',count=by//4,offset=off)
    hits=[]
    for i in range(0,len(a)-12,4):
        R=np.array([a[i:i+3],a[i+4:i+7],a[i+8:i+11]])
        if np.allclose(R@R.T,np.eye(3),atol=2e-3) and abs(np.linalg.det(R)-1)<1e-2 and not np.allclose(R,np.eye(3)):
            hits.append('+0x%X t=(%.1f %.1f %.1f)'%(i*4,a[i+3],a[i+7],a[i+11]))
    print(ui,'rva',hex(rva[1]),hits[:10])
