# Minimal RenderWare chunk dumper (DFF/TXD): tree of chunk ids, versions, frame names, atomics.
import struct, sys
NAMES = {0x1:'Struct',0x2:'String',0x3:'Extension',0x6:'Texture',0x7:'Material',0x8:'MaterialList',0xE:'FrameList',
 0xF:'Geometry',0x10:'Clump',0x14:'Atomic',0x15:'TextureNative',0x16:'TexDictionary',0x1A:'GeometryList',
 0x1F:'RightToRender',0x116:'Skin',0x11E:'HAnim',0x120:'MatFX',0x50E:'BinMesh',0x253F2FE:'Frame(name)',
 0x253F2FA:'CollisionModel',0x253F2FC:'ReflectionMat',0x253F2F6:'SpecularMat',0x253F2F8:'2dfx',0x510:'NativeData'}
CONTAINERS = {0x3,0x6,0x7,0x8,0xE,0xF,0x10,0x14,0x16,0x1A,0x15}
def walk(d, o, end, depth, out):
    while o + 12 <= end:
        t, s, v = struct.unpack_from('<III', d, o)
        out.append((depth, t, s, v, o))
        if t in CONTAINERS: walk(d, o+12, o+12+s, depth+1, out)
        o += 12 + s
def main(p, full=False):
    d = open(p,'rb').read(); out = []; walk(d, 0, len(d), 0, out)
    vers = sorted(set(v for _,_,_,v,_ in out)); print('versions', [hex(v) for v in vers])
    for depth,t,s,v,o in out:
        if t == 0x253F2FE: print('  '*depth, 'FrameName', d[o+12:o+12+s].decode('latin1'))
        elif full or depth < 2: print('  '*depth, NAMES.get(t,hex(t)), s, hex(v))
    return d, out
if __name__ == '__main__': main(sys.argv[1], len(sys.argv) > 2)
