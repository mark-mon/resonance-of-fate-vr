# Rebuild an analysable PE from an in-memory image dump (raw = virtual layout).
# Usage: python3 rebuild_pe.py so4_dump0.bin so4_dump0.txt out.exe
import sys, struct, pefile
binp, txtp, outp = sys.argv[1:4]
data = bytearray(open(binp, 'rb').read())
info = {}
for l in open(txtp, encoding='latin1'):
    p = l.split()
    if p and p[0] in ('base', 'span', 'sizeofimage'): info[p[0]] = int(p[1], 16)
pe = pefile.PE(data=bytes(data[:0x1000]), fast_load=True)
base = info['base']
# Make raw offsets equal virtual addresses, image base = runtime base (pointers in the dump are absolute).
pe.OPTIONAL_HEADER.ImageBase = base
pe.OPTIONAL_HEADER.SizeOfImage = len(data)
pe.OPTIONAL_HEADER.FileAlignment = pe.OPTIONAL_HEADER.SectionAlignment
for s in pe.sections:
    s.PointerToRawData = s.VirtualAddress
    s.SizeOfRawData = max(s.Misc_VirtualSize, s.SizeOfRawData)
    s.Misc_VirtualSize = s.SizeOfRawData
hdr = pe.write()
data[:len(hdr)] = hdr
open(outp, 'wb').write(data)
print('wrote', outp, len(data), 'base', hex(base))
for s in pe.sections: print(s.Name.rstrip(b'\0'), hex(s.VirtualAddress), hex(s.Misc_VirtualSize))
