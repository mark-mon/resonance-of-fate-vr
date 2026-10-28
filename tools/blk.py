import struct, sys, numpy as np
d=open(sys.argv[1],'rb').read(); o=0
magic,nu,nr=struct.unpack_from('<III',d,o); o+=28
o+=nr*472
ups=[]
for i in range(nu):
    buf,by,bb,how,off=struct.unpack_from('<QIIII',d,o); rva=struct.unpack_from('<4I',d,o+24); o+=40; ups.append((buf,by,bb,how,off,rva))
nd=struct.unpack_from('<I',d,o)[0]; o+=4; data=d[o:o+nd]
for ui,(buf,by,bb,how,off,rva) in enumerate(ups):
    if by!=2208: continue
    a=np.frombuffer(data,dtype='<f4',count=by//4,offset=off)
    hits=[]
    for i in range(0,len(a)-16,4):
        M=a[i:i+16].reshape(4,4)
        R=M[:3,:3]
        if np.allclose(R@R.T,np.eye(3),atol=1e-3) and np.allclose(M[3],[0,0,0,1],atol=1e-4): hits.append(('rowmajor-rigid',hex(i*4),M[:3,3].round(2).tolist()))
        Rt=M[:3,:3].T
        if np.allclose(M[:3,3],0,atol=1e-4) and np.allclose(Rt@Rt.T,np.eye(3),atol=1e-3) and abs(M[3,3]-1)<1e-4 and np.abs(M[3,:3]).sum()>1e-3: hits.append(('colmajor-rigid',hex(i*4),M[3,:3].round(2).tolist()))
    print('upload',ui,'rva',[hex(x) for x in rva], hits[:12])
    if ui==0:
        for r in range(0,by//4,16): print('  +0x%03X'%(r*4), ' '.join('%9.4f'%v for v in a[r:r+16]))
