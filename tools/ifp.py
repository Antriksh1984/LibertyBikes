# ANPK (GTA III/VC .ifp) reader/writer: list blocks + anims, extract blocks to a new ifp.
import struct, sys
def r4(d,o): return d[o:o+4], struct.unpack_from('<I',d,o+4)[0]
def blocks(d):
    o = 0; out = []
    while o + 8 <= len(d):
        tag, size = r4(d, o)
        if tag != b'ANPK': break
        end = o + 8 + size
        # INFO
        t, s = r4(d, o+8); n = struct.unpack_from('<i', d, o+16)[0]
        name = d[o+20:o+16+((s+3)&~3)+4].split(b'\0')[0].decode('latin1')
        p = o + 16 + ((s+3)&~3); anims = []
        while p < end:
            t, s = r4(d, p)
            if t == b'NAME':
                anims.append(d[p+8:p+8+s].split(b'\0')[0].decode('latin1'))
            if t in (b'NAME',): p += 8 + ((s+3)&~3)
            elif t == b'DGAN': p += 8 + s
            else: p += 8 + ((s+3)&~3)
        out.append((name, n, anims, o, end)); o = end
        o = (o + 3) & ~3 if o % 4 else o
    return out
if __name__ == '__main__':
    d = open(sys.argv[1],'rb').read()
    tot = 0
    for name, n, anims, o, e in blocks(d):
        tot += len(anims)
        print(name, n, len(anims), (' '.join(anims) if len(sys.argv)>2 and name in sys.argv[2:] else ''))
    print('total', tot)
