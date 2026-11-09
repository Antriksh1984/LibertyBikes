# Helpers to read gta3.exe (v1.0) by virtual address, and disassemble.
import struct, sys, os
EXE = os.path.join(os.path.dirname(__file__), '..', '..', '..', 'gta3.exe')
D = open(EXE, 'rb').read()
pe = struct.unpack_from('<I', D, 0x3c)[0]
nsec = struct.unpack_from('<H', D, pe+6)[0]; opt = struct.unpack_from('<H', D, pe+20)[0]
base = struct.unpack_from('<I', D, pe+24+28)[0]
secs = []
for i in range(nsec):
    o = pe+24+opt+i*40
    name = D[o:o+8].rstrip(b'\0'); vs, va, rs, ro = struct.unpack_from('<IIII', D, o+8)
    secs.append((name, base+va, vs, rs, ro))
def off(v):
    for n, va, vs, rs, ro in secs:
        if va <= v < va+max(vs, rs): return ro + v - va
    raise ValueError(hex(v))
def u32(v): return struct.unpack_from('<I', D, off(v))[0]
def f32(v): return struct.unpack_from('<f', D, off(v))[0]
def cstr(v):
    o = off(v); return D[o:D.index(b'\0', o)].decode('latin1')
def dis(v, n=60):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    o = off(v); out = []
    for ins in md.disasm(D[o:o+n*8], v):
        out.append('%08x  %-6s %s' % (ins.address, ins.mnemonic, ins.op_str)); n -= 1
        if n == 0: break
    return '\n'.join(out)
def strings_find(s):
    b = s.encode() + b'\0'; i = D.find(b); res = []
    while i >= 0:
        for n, va, vs, rs, ro in secs:
            if ro <= i < ro+rs: res.append(va + i - ro)
        i = D.find(b, i+1)
    return res
