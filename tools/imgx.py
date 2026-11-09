import sys, struct, os
d, img, out, names = sys.argv[1], sys.argv[2], sys.argv[3], [n.lower() for n in sys.argv[4:]]
dd = open(d,'rb').read(); f = open(img,'rb')
for i in range(len(dd)//32):
    off, size, name = struct.unpack_from('<II24s', dd, i*32)
    name = name.split(b'\0')[0].decode('latin1')
    if name.lower() in names:
        f.seek(off*2048); data = f.read(size*2048)
        open(os.path.join(out,name),'wb').write(data)
        t, s, v = struct.unpack_from('<III', data, 0)
        print(name, size*2048, 'chunk %#x ver %#x' % (t, v))
