# LibertyBikes installer: takes the PCJ-600 from the player's own GTA Vice City install and
# adds it to GTA III. Nothing from Vice City is shipped with the mod.
#
#   LibertyBikesInstaller.exe [--gta3 DIR] [--vc DIR] [--uninstall]
#
# It must be run from (or pointed at) the GTA III folder. Files it changes are backed up to
# _backups\LibertyBikes first, and --uninstall puts them back.
import os, shutil, struct, sys

HERE = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, 'frozen', False) else __file__))
sys.path.insert(0, HERE)
import retarget_ifp as rt

RW_III = 0x0C02FFFF   # RenderWare 3.3.0.2, what gta3.exe reads
IDE_LINE = '151,\tpcj600,\tpcj600,\tbike,\tKURUMA,\tPCJ600,\tspecial,\t0,\t7,\t0,\t16,\t0.67\n'
CONTAINERS = {0x3, 0x6, 0x7, 0x8, 0xE, 0xF, 0x10, 0x14, 0x15, 0x16, 0x1A}
BACKED_UP = [('models', 'gta3.dir'), ('models\\coll', 'vehicles.col'), ('data', 'default.ide')]


def say(msg): print(msg, flush=True)


# ---------------------------------------------------------------- finding the games
def is_gta3(d): return d and os.path.isfile(os.path.join(d, 'gta3.exe')) and os.path.isfile(os.path.join(d, 'models', 'gta3.img'))
def is_vc(d): return d and os.path.isfile(os.path.join(d, 'gta-vc.exe')) and os.path.isfile(os.path.join(d, 'models', 'gta3.img'))

def find_vc(gta3):
    parent = os.path.dirname(gta3)
    candidates = [os.path.join(parent, n) for n in sorted(os.listdir(parent))] if os.path.isdir(parent) else []
    for root in (os.environ.get('ProgramFiles(x86)', ''), os.environ.get('ProgramFiles', '')):
        candidates += [os.path.join(root, 'Steam', 'steamapps', 'common', 'Grand Theft Auto Vice City'),
                       os.path.join(root, 'Rockstar Games', 'Grand Theft Auto Vice City'),
                       os.path.join(root, 'Rockstar Games', 'GTA Vice City')]
    for c in candidates:
        if is_vc(c): return c
    return None

def ask_dir(prompt, check):
    while True:
        d = input(prompt).strip().strip('"')
        if not d: return None
        if check(d): return d
        say('  That folder does not look right, try again (or press Enter to cancel).')


# ---------------------------------------------------------------- IMG / RW helpers
def img_read(game, names):
    d = open(os.path.join(game, 'models', 'gta3.dir'), 'rb').read()
    want = {n.lower(): None for n in names}
    with open(os.path.join(game, 'models', 'gta3.img'), 'rb') as img:
        for i in range(len(d) // 32):
            off, size, name = struct.unpack_from('<II24s', d, i * 32)
            name = name.split(b'\0')[0].decode('latin1').lower()
            if name in want and want[name] is None:
                img.seek(off * 2048)
                want[name] = img.read(size * 2048)
    missing = [n for n, v in want.items() if v is None]
    if missing: raise SystemExit('Could not find %s in %s\\models\\gta3.img' % (', '.join(missing), game))
    return want

def trim_rw(data):
    t, s, v = struct.unpack_from('<III', data, 0)
    return data[:12 + s]

def trim_ifp(data):
    return data[:8 + struct.unpack_from('<I', data, 4)[0]]

def restamp(data):
    """Set every RW chunk header to RW_III; the PCJ's TXD is 3.4, which gta3.exe refuses."""
    data = bytearray(trim_rw(data))
    def walk(o, end):
        while o + 12 <= end:
            t, s, v = struct.unpack_from('<III', data, o)
            if v & 0xFFFF0000: struct.pack_into('<I', data, o + 8, RW_III)
            if t in CONTAINERS: walk(o + 12, o + 12 + s)
            o += 12 + s
    walk(0, len(data))
    return bytes(data)

def col_entry(vc, model):
    d = open(os.path.join(vc, 'models', 'coll', 'vehicles.col'), 'rb').read(); o = 0
    while o + 32 <= len(d):
        fourcc, size = struct.unpack_from('<4sI', d, o)
        if d[o + 8:o + 30].split(b'\0')[0].decode('latin1').lower() == model: return d[o:o + 8 + size]
        o += 8 + size
    raise SystemExit('Could not find the %s collision in Vice City\'s vehicles.col' % model)


# ---------------------------------------------------------------- install steps
def backup(gta3):
    dst = os.path.join(gta3, '_backups', 'LibertyBikes')
    os.makedirs(dst, exist_ok=True)
    for folder, name in BACKED_UP:
        target = os.path.join(dst, name)
        if not os.path.exists(target): shutil.copy2(os.path.join(gta3, folder, name), target)

def img_add(gta3, files):
    d_path, i_path = os.path.join(gta3, 'models', 'gta3.dir'), os.path.join(gta3, 'models', 'gta3.img')
    dd = open(d_path, 'rb').read()
    entries = []
    for i in range(len(dd) // 32):
        off, size, name = struct.unpack_from('<II24s', dd, i * 32)
        entries.append([off, size, name.split(b'\0')[0].decode('latin1')])
    end_sector = (os.path.getsize(i_path) + 2047) // 2048
    with open(i_path, 'r+b') as img:
        for name, data in files:
            sectors = (len(data) + 2047) // 2048
            padded = data + b'\0' * (sectors * 2048 - len(data))
            hit = next((e for e in entries if e[2].lower() == name.lower()), None)
            if hit and hit[1] >= sectors:
                img.seek(hit[0] * 2048); img.write(padded); hit[1] = sectors
                continue
            img.seek(end_sector * 2048); img.write(padded)
            if hit: hit[0], hit[1] = end_sector, sectors
            else: entries.append([end_sector, sectors, name])
            end_sector += sectors
    with open(d_path, 'wb') as f:
        for off, size, name in entries: f.write(struct.pack('<II24s', off, size, name.encode('latin1')))

def col_add(gta3, entry):
    path = os.path.join(gta3, 'models', 'coll', 'vehicles.col')
    d = open(path, 'rb').read(); o = 0
    while o + 32 <= len(d):
        fourcc, size = struct.unpack_from('<4sI', d, o)
        if d[o + 8:o + 30].split(b'\0')[0].lower() == b'pcj600': return
        o += 8 + size
    with open(path, 'ab') as f: f.write(entry)

def ide_add(gta3):
    path = os.path.join(gta3, 'data', 'default.ide')
    lines = open(path, 'r', encoding='latin1').readlines()
    if any(l.split(',')[0].strip() == '151' for l in lines):
        if not any('pcj600' in l.lower() for l in lines):
            raise SystemExit('Model ID 151 in data\\default.ide is already used by another mod.')
        return
    out, in_cars, done = [], False, False
    for l in lines:
        s = l.strip().lower()
        if s == 'cars': in_cars = True
        elif in_cars and s == 'end' and not done:
            out.append(IDE_LINE); done = True; in_cars = False
        out.append(l)
    open(path, 'w', encoding='latin1', newline='').write(''.join(out))

def build_anims(gta3, vc, vc_files, iii_player):
    """Retarget VC's bike animations onto GTA III's ped skeleton (see retarget_ifp.py)."""
    vcf, iif = rt.dff_frames(trim_rw(vc_files['player.dff'])), rt.dff_frames(trim_rw(iii_player))
    _, vped = rt.read_ifp(os.path.join(vc, 'anim', 'ped.ifp'))
    _, iped = rt.read_ifp(os.path.join(gta3, 'anim', 'ped.ifp'))
    calib = rt.calibrate(vcf, iif, vped['idle_stance'], iped['idle_stance'])
    extra = {s['name']: s['extra'] for s in iped['idle_stance']['seqs']}
    block, anims = rt.read_ifp(trim_ifp(vc_files['bikes.ifp']))
    out = [rt.retarget(vcf, iif, calib, anims[k], extra) for k in anims]
    for name in ('bike_fall_off', 'bike_fallr'):
        out.append(rt.retarget(vcf, iif, calib, vped[name], extra))
    return rt.build_ifp(block, out)

def install(gta3, vc):
    say('GTA III:      ' + gta3)
    say('Vice City:    ' + vc)
    if not os.path.isfile(os.path.join(gta3, 'scripts', 'LibertyBikes.asi')):
        say('Note: scripts\\LibertyBikes.asi is not there yet - copy the scripts folder from the zip too.')
    say('Reading the PCJ-600 from Vice City...')
    vc_files = img_read(vc, ['pcj600.dff', 'pcj600.txd', 'bikes.ifp', 'player.dff'])
    iii_player = img_read(gta3, ['player.dff'])['player.dff']
    say('Converting the bike animations to the GTA III skeleton...')
    anims = build_anims(gta3, vc, vc_files, iii_player)
    say('Backing up files to _backups\\LibertyBikes...')
    backup(gta3)
    say('Adding the bike to GTA III...')
    img_add(gta3, [('pcj600.dff', restamp(vc_files['pcj600.dff'])), ('pcj600.txd', restamp(vc_files['pcj600.txd']))])
    col_add(gta3, col_entry(vc, 'pcj600'))
    ide_add(gta3)
    os.makedirs(os.path.join(gta3, 'scripts', 'LibertyBikes'), exist_ok=True)
    open(os.path.join(gta3, 'scripts', 'LibertyBikes', 'bikes.ifp'), 'wb').write(anims)
    say('\nDone! Start GTA III and press F4 to spawn a PCJ-600.')

def uninstall(gta3):
    src = os.path.join(gta3, '_backups', 'LibertyBikes')
    if not os.path.isdir(src): raise SystemExit('No backups found in _backups\\LibertyBikes - nothing to undo.')
    for folder, name in BACKED_UP:
        b = os.path.join(src, name)
        if os.path.exists(b): shutil.copy2(b, os.path.join(gta3, folder, name)); say('restored ' + os.path.join(folder, name))
    for f in ('LibertyBikes.asi', 'LibertyBikes.ini', 'LibertyBikes.log'):
        p = os.path.join(gta3, 'scripts', f)
        if os.path.exists(p): os.remove(p); say('removed scripts\\' + f)
    shutil.rmtree(os.path.join(gta3, 'scripts', 'LibertyBikes'), ignore_errors=True)
    say('\nLibertyBikes removed. (models\\gta3.img keeps some unused bytes at the end; that is harmless.)')

def main():
    args = sys.argv[1:]
    def opt(name):
        return args[args.index(name) + 1] if name in args and args.index(name) + 1 < len(args) else None
    say('LibertyBikes installer - Vice City motorbikes for GTA III\n')
    gta3 = os.path.abspath(opt("--gta3") or (HERE if is_gta3(HERE) else os.getcwd()))
    if not is_gta3(gta3):
        gta3 = ask_dir('Put this installer in your GTA III folder, or paste the GTA III folder here: ', is_gta3)
        if not gta3: return 1
    if '--uninstall' in args:
        uninstall(gta3); return 0
    vc = opt('--vc') or find_vc(gta3)
    if not is_vc(vc):
        say('Vice City was not found automatically. This mod takes the bike from your own copy of Vice City.')
        vc = ask_dir('Paste your GTA Vice City folder (the one with gta-vc.exe): ', is_vc)
        if not vc: return 1
    install(gta3, vc)
    return 0

if __name__ == '__main__':
    try:
        code = main()
    except SystemExit as e:
        say('\nError: %s' % e); code = 1
    except Exception as e:
        say('\nUnexpected error: %r' % e); code = 1
    if getattr(sys, 'frozen', False) and '--quiet' not in sys.argv:
        input('\nPress Enter to close.')
    sys.exit(code)
