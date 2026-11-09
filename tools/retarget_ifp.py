# Retargets VC (skinned "Bip01" skeleton) ped animations onto GTA III's segmented ped skeleton.
#
# Calibration: an animation that exists in both games (idle_stance) gives, at frame 0, the same
# physical pose on both skeletons. For each III segment we take the VC bone that drives it and
# compute a constant offset O = W_vc_ref^-1 * W_iii_ref (world orientations, column convention).
# Each VC frame is then mapped with W_iii(t) = W_vc(t) * O and turned back into III local rotations.
# The pelvis (Swaist) also gets a translation: VC pelvis world position + calibrated offset.
#
# Usage: python -I retarget_ifp.py <vc player.dff> <iii player.dff> <vc ped.ifp> <iii ped.ifp>
#                                  <vc anims.ifp> <out.ifp> [--check walk_civi]
import math, struct, sys

# ---------- quaternion / matrix helpers (column-vector convention) ----------
def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw*bx + ax*bw + ay*bz - az*by, aw*by - ax*bz + ay*bw + az*bx,
            aw*bz + ax*by - ay*bx + az*bw, aw*bw - ax*bx - ay*by - az*bz)
def qconj(q): return (-q[0], -q[1], -q[2], q[3])
def qnorm(q):
    l = math.sqrt(sum(c*c for c in q)) or 1.0
    return tuple(c/l for c in q)
def qrot(q, v):
    p = qmul(qmul(q, (v[0], v[1], v[2], 0.0)), qconj(q)); return p[:3]
def qslerp(a, b, t):
    d = sum(x*y for x, y in zip(a, b))
    if d < 0: b = tuple(-c for c in b); d = -d
    if d > 0.9995: return qnorm(tuple(x + (y-x)*t for x, y in zip(a, b)))
    th = math.acos(d); s = math.sin(th)
    return tuple((math.sin((1-t)*th)*x + math.sin(t*th)*y)/s for x, y in zip(a, b))
def mat_to_q(m):  # m = rows (3x3) of a column-convention rotation matrix
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        return qnorm(((m[2][1]-m[1][2])/s, (m[0][2]-m[2][0])/s, (m[1][0]-m[0][1])/s, 0.25*s))
    if m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        return qnorm((0.25*s, (m[0][1]+m[1][0])/s, (m[0][2]+m[2][0])/s, (m[2][1]-m[1][2])/s))
    if m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        return qnorm(((m[0][1]+m[1][0])/s, 0.25*s, (m[1][2]+m[2][1])/s, (m[0][2]-m[2][0])/s))
    s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
    return qnorm(((m[0][2]+m[2][0])/s, (m[1][2]+m[2][1])/s, 0.25*s, (m[1][0]-m[0][1])/s))

# ---------- DFF frame list ----------
def _data(src):
    return src if isinstance(src, (bytes, bytearray)) else open(src, 'rb').read()

def dff_frames(path):
    d = _data(path)
    o = 12 + 12 + struct.unpack_from('<I', d, 16)[0]       # skip clump struct
    t, s, v = struct.unpack_from('<III', d, o); assert t == 0xE
    p = o + 12; t2, s2, v2 = struct.unpack_from('<III', d, p)
    n = struct.unpack_from('<I', d, p + 12)[0]; q = p + 16
    frames = []
    for i in range(n):
        f = struct.unpack_from('<12fiI', d, q); q += 56
        right, up, at, pos = f[0:3], f[3:6], f[6:9], f[9:12]
        # RW rows are the child basis vectors in parent space -> they are the columns of R
        R = [[right[0], up[0], at[0]], [right[1], up[1], at[1]], [right[2], up[2], at[2]]]
        frames.append({'q': mat_to_q(R), 'pos': pos, 'parent': f[12], 'name': None})
    q = p + 12 + s2; i = 0
    end = o + 12 + s
    while q < end and i < n:          # one Extension per frame, holding the name chunk
        t3, s3, v3 = struct.unpack_from('<III', d, q); e = q + 12
        while e < q + 12 + s3:
            t4, s4, v4 = struct.unpack_from('<III', d, e)
            if t4 == 0x253F2FE: frames[i]['name'] = d[e+12:e+12+s4].decode('latin1').strip('\0')
            e += 12 + s4
        q += 12 + s3; i += 1
    return frames

# ---------- IFP ----------
def r4(d, o): return d[o:o+4], struct.unpack_from('<I', d, o+4)[0]
def pad(s): return (s + 3) & ~3
def read_ifp(path):
    d = _data(path); anims = {}; blockname = None
    o = 0
    while o + 8 <= len(d):
        tag, size = r4(d, o)
        if tag != b'ANPK': break
        end = o + 8 + size
        t, s = r4(d, o + 8); blockname = d[o+20:o+16+pad(s)].split(b'\0')[0].decode('latin1')
        p = o + 16 + pad(s); name = None
        while p < end:
            t, s = r4(d, p)
            if t == b'NAME':
                name = d[p+8:p+8+s].split(b'\0')[0].decode('latin1'); p += 8 + pad(s); continue
            if t == b'DGAN':
                q = p + 8; dend = p + 8 + s
                t2, s2 = r4(d, q); q += 8 + pad(s2)
                seqs = []
                while q < dend:
                    t3, s3 = r4(d, q); qq = q + 8           # CPAN
                    t4, s4 = r4(d, qq); buf = d[qq+8:qq+8+s4]
                    bname = buf[:28].split(b'\0')[0].decode('latin1'); nf = struct.unpack_from('<i', buf, 28)[0]
                    extra = buf[28:]
                    qq += 8 + s4; kt, ks = r4(d, qq); kd = d[qq+8:qq+8+ks]
                    keys = []
                    stride = {b'KR00': 5, b'KRT0': 8, b'KRTS': 11}.get(kt)
                    for k in range(nf if stride else 0):
                        f = struct.unpack_from('<%df' % stride, kd, k*stride*4)
                        rot = qnorm((-f[0], -f[1], -f[2], f[3]))      # game quaternion
                        trans = f[4:7] if stride > 5 else None
                        keys.append((f[stride-1], rot, trans))
                    seqs.append({'name': bname, 'keys': keys, 'extra': extra})
                    q = qq + 8 + ks
                anims[name.lower()] = {'name': name, 'seqs': seqs}
                p = dend; continue
            p += 8 + pad(s)
        o = end
    return blockname, anims

def sample(seq, t):
    keys = seq['keys']
    if t <= keys[0][0]: return keys[0][1], keys[0][2]
    for a, b in zip(keys, keys[1:]):
        if t <= b[0]:
            u = (t - a[0]) / max(b[0] - a[0], 1e-6)
            tr = None
            if a[2] and b[2]: tr = tuple(x + (y-x)*u for x, y in zip(a[2], b[2]))
            return qslerp(a[1], b[1], u), tr
    return keys[-1][1], keys[-1][2]

# ---------- skeleton evaluation ----------
def norm_name(n): return n.lower().replace('bip01 ', '').strip()
def world_pose(frames, anim, t):
    """Return {frame index: (world quat, world pos)} for a dff skeleton posed by anim at time t."""
    seqmap = {norm_name(s['name']): s for s in anim['seqs']} if anim else {}
    out = {}
    def get(i):
        if i in out: return out[i]
        f = frames[i]; q, pos = f['q'], f['pos']
        s = seqmap.get(norm_name(f['name'] or '')) if f['parent'] >= 0 else None   # clump frame never animates
        if s and s['keys']:
            q, tr = sample(s, t)
            if tr: pos = tr
        if f['parent'] < 0: out[i] = (q, pos)
        else:
            pq, pp = get(f['parent'])
            wp = qrot(pq, pos); out[i] = (qmul(pq, q), (wp[0]+pp[0], wp[1]+pp[1], wp[2]+pp[2]))
        return out[i]
    for i in range(len(frames)): get(i)
    return out

# III segment <- VC bone
MAP = {'Swaist': 'pelvis', 'Smid': 'spine', 'Storso': 'spine1', 'Shead': 'head',
       'Supperarml': 'l upperarm', 'Slowerarml': 'l forearm', 'SLhand': 'l hand',
       'Supperarmr': 'r upperarm', 'Slowerarmr': 'r forearm', 'SRhand': 'r hand',
       'Supperlegl': 'l thigh', 'Slowerlegl': 'l calf', 'Sfootl': 'l foot',
       'Supperlegr': 'r thigh', 'Slowerlegr': 'r calf', 'Sfootr': 'r foot'}

def index_by_name(frames):
    return {norm_name(f['name']): i for i, f in enumerate(frames) if f['name']}

def retarget(vc_frames, iii_frames, calib, vanim, extra_by_bone):
    vidx, iidx = index_by_name(vc_frames), index_by_name(iii_frames)
    times = sorted({round(k[0], 4) for s in vanim['seqs'] for k in s['keys']})
    out_keys = {seg: [] for seg in MAP}
    for t in times:
        vw = world_pose(vc_frames, vanim, t)
        iw = {}
        # world orientations for the III segments, then locals by walking III parents
        for seg, bone in MAP.items():
            q = qmul(vw[vidx[bone]][0], calib['O'][seg]); iw[seg] = q
        for seg in MAP:
            fi = iidx[seg.lower()]; par = iii_frames[fi]['parent']
            pname = iii_frames[par]['name'] if par >= 0 else None
            if pname in MAP: lq = qmul(qconj(iw[pname]), iw[seg])
            else:            lq = qmul(qconj(calib['iii_parent_world'][seg]), iw[seg])
            tr = None
            if seg == 'Swaist':
                vp = vw[vidx['pelvis']][1]; dlt = calib['pelvis_offset']
                world = (vp[0]+dlt[0], vp[1]+dlt[1], vp[2]+dlt[2])
                pq, pp = calib['iii_parent_world_pos'][seg]
                rel = qrot(qconj(pq), (world[0]-pp[0], world[1]-pp[1], world[2]-pp[2]))
                tr = rel
            out_keys[seg].append((t, lq, tr))
    seqs = []
    for seg in MAP:      # keep III bone order as in III's ped.ifp
        seqs.append({'name': seg, 'keys': out_keys[seg], 'extra': extra_by_bone[seg]})
    return {'name': vanim['name'], 'seqs': seqs}

def calibrate(vc_frames, iii_frames, vref, iref):
    vidx, iidx = index_by_name(vc_frames), index_by_name(iii_frames)
    vw, iw = world_pose(vc_frames, vref, 0.0), world_pose(iii_frames, iref, 0.0)
    O, ppw, ppwp = {}, {}, {}
    for seg, bone in MAP.items():
        fi = iidx[seg.lower()]
        O[seg] = qmul(qconj(vw[vidx[bone]][0]), iw[fi][0])
        par = iii_frames[fi]['parent']
        ppw[seg] = iw[par][0] if par >= 0 else (0, 0, 0, 1)
        ppwp[seg] = iw[par] if par >= 0 else ((0, 0, 0, 1), (0, 0, 0))
    vp, ip = vw[vidx['pelvis']][1], iw[iidx['swaist']][1]
    return {'O': O, 'iii_parent_world': ppw, 'iii_parent_world_pos': ppwp,
            'pelvis_offset': (ip[0]-vp[0], ip[1]-vp[1], ip[2]-vp[2])}

def build_ifp(block, anims):
    def chunk(tag, payload): return tag + struct.pack('<I', len(payload)) + payload
    def padb(b): return b + b'\0' * (pad(len(b)) - len(b))
    body = b''
    for a in anims:
        body += chunk(b'NAME', padb(a['name'].encode() + b'\0'))
        dg = chunk(b'INFO', struct.pack('<i', len(a['seqs'])) + padb(b'\0'))
        for s in a['seqs']:
            nf = len(s['keys']); has_t = any(k[2] for k in s['keys'])
            name = s['name'].encode().ljust(28, b'\0')
            extra = bytearray(s['extra'])
            struct.pack_into('<i', extra, 0, nf)          # numFrames
            if len(extra) >= 12: struct.pack_into('<i', extra, 8, nf - 1)   # last frame index
            anim = chunk(b'ANIM', name + bytes(extra))
            kd = b''
            for t, q, tr in s['keys']:
                f = (-q[0], -q[1], -q[2], q[3])       # back to file quaternion
                kd += struct.pack('<4f', *f) + (struct.pack('<3f', *(tr or (0, 0, 0))) if has_t else b'') + struct.pack('<f', t)
            dg += chunk(b'CPAN', anim + chunk(b'KRT0' if has_t else b'KR00', kd))
        body += chunk(b'DGAN', dg)
    info = chunk(b'INFO', struct.pack('<i', len(anims)) + padb(block.encode() + b'\0'))
    return chunk(b'ANPK', info + body)

def write_ifp(path, block, anims):
    open(path, 'wb').write(build_ifp(block, anims))

def compare(a, b):
    """Mean angular error (degrees) between two III-skeleton anims sampled over time."""
    sa = {s['name']: s for s in a['seqs']}; sb = {s['name']: s for s in b['seqs']}
    errs = []
    T = max(k[0] for s in b['seqs'] for k in s['keys'])
    for i in range(12):
        t = T * i / 11
        for n in MAP:
            if n in sa and n in sb and sa[n]['keys'] and sb[n]['keys']:
                qa, _ = sample(sa[n], t); qb, _ = sample(sb[n], t)
                d = abs(sum(x*y for x, y in zip(qa, qb)))
                errs.append(math.degrees(2*math.acos(min(1.0, d))))
    return sum(errs)/len(errs), max(errs)

if __name__ == '__main__':
    a = sys.argv[1:]
    vc_dff, iii_dff, vc_ped, iii_ped, src, dst = a[:6]
    vcf, iif = dff_frames(vc_dff), dff_frames(iii_dff)
    _, vped = read_ifp(vc_ped); _, iped = read_ifp(iii_ped)
    calib = calibrate(vcf, iif, vped['idle_stance'], iped['idle_stance'])
    extra = {s['name']: s['extra'] for s in iped['idle_stance']['seqs']}
    if '--check' in a:
        name = a[a.index('--check') + 1]
        r = retarget(vcf, iif, calib, vped[name], extra)
        print('check %s: mean %.1f deg, max %.1f deg' % ((name,) + compare(r, iped[name])))
    block, anims = read_ifp(src)
    out = [retarget(vcf, iif, calib, anims[k], extra) for k in anims]
    if '--extra' in a:     # --extra <ifp> name1,name2 : append anims from another VC file (e.g. ped.ifp)
        i = a.index('--extra')
        _, more = read_ifp(a[i + 1])
        for name in a[i + 2].split(','):
            out.append(retarget(vcf, iif, calib, more[name.lower()], extra))
    write_ifp(dst, block, out)
    print('wrote', dst, len(out), 'anims (block %s)' % block)
