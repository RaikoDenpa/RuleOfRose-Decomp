"""Renderer regression check: three reference frames, in both GS modes (notes/renderer.md 6.7)."""
import argparse, os, shutil, struct, subprocess, sys, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REF = os.path.join(ROOT, 'extracted', 'gsref')
RUN = os.path.join(ROOT, 'extracted', 'ps2recomp', 'run')
GSREPLAY = os.path.join(ROOT, 'extracted', 'pcsx2', 'tree-run', 'ror-gsreplay.exe')
CASES = [('sellang', 90, 'language screen'), ('title', 380, 'Title menu'), ('evt1new20', 650, 'A164 opening event')]
PAD = '100+3:cross;200+3:start;300+3:start;400+3:cross;460+3:cross'
LIVE_LAG = 2
OUTPUTS = {'cpu': ('.gs', '_ours.png', '_pcsx2.png'), 'pcsx2': ('_vif1.gs', '_live.png', '_vif1.png')}


def read_png(path):
    d = open(path, 'rb').read()
    pos, idat, w = 8, b'', 0
    while pos < len(d):
        n, kind = struct.unpack_from('>I4s', d, pos)
        body = d[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', body)
            if depth != 8 or ctype not in (2, 6) or inter:
                raise ValueError('%s: unsupported PNG (depth %d, colour type %d)' % (path, depth, ctype))
            bpp = 3 if ctype == 2 else 4
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw, stride, out, prev = zlib.decompress(idat), w * bpp, bytearray(), bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 0xff
            elif f == 2:
                line[i] = (line[i] + b) & 0xff
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xff
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xff
        prev = line
        for x in range(w):
            out += line[x * bpp:x * bpp + 3]
    return w, h, bytes(out)


def downscale(w, h, px, nw, nh):
    xs = [(x * w // nw, max(x * w // nw + 1, (x + 1) * w // nw)) for x in range(nw)]
    out = bytearray()
    for y in range(nh):
        y0, y1 = y * h // nh, max(y * h // nh + 1, (y + 1) * h // nh)
        rows = [px[r * w * 3:(r + 1) * w * 3] for r in range(y0, y1)]
        for x0, x1 in xs:
            n = (x1 - x0) * len(rows)
            for c in range(3):
                out.append((sum(sum(row[x * 3 + c] for x in range(x0, x1)) for row in rows) + n // 2) // n)
    return bytes(out)


def compare(a, b, edges=False):
    if not os.path.exists(b):
        return False, 'no reference %s' % os.path.basename(b)
    wa, ha, pa = read_png(a)
    wb, hb, pb = read_png(b)
    scaled = ''
    if (wa, ha) != (wb, hb) and wa >= wb and ha >= hb:
        pa, scaled = downscale(wa, ha, pa, wb, hb), ' (%dx%d scaled)' % (wa, ha)
        wa, ha = wb, hb
    if (wa, ha) != (wb, hb):
        return False, 'size %dx%d, reference %dx%d' % (wa, ha, wb, hb)
    bad = total = 0
    for i in range(0, len(pa), 3):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        total += d
        if d > 24 and edges:
            x, y = (i // 3) % wa, (i // 3) // wa
            d = min(max(abs(pa[i] - pb[j]), abs(pa[i + 1] - pb[j + 1]), abs(pa[i + 2] - pb[j + 2]))
                    for yy in range(max(y - 1, 0), min(y + 2, ha)) for xx in range(max(x - 1, 0), min(x + 2, wa))
                    for j in ((yy * wa + xx) * 3,))
        bad += d > 24
    n = wa * ha
    return bad * 200 <= n, '%.2f %% of pixels differ, mean %.2f%s' % (100.0 * bad / n, total / 3.0 / n, scaled)




def replay(dump, png, vif1=False, gs_ini=None):
    if not os.path.exists(GSREPLAY):
        sys.exit('no %s: run build.py pcsx2' % GSREPLAY)
    r = subprocess.run([GSREPLAY, dump] + (['--vif1'] if vif1 else []) + ['--shot', 'last:' + png] +
                       (['--gs-ini', gs_ini] if gs_ini else []), capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('replay of %s failed:\n%s%s' % (dump, r.stdout, r.stderr))


def capture(mode, new, gs_ini=None):
    lag = LIVE_LAG if mode == 'pcsx2' else 0
    last = max(f for _, f, _ in CASES) + 4
    card = os.path.join(new, 'memcard')
    shutil.rmtree(card, ignore_errors=True)
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'port', 'build.py'), 'run', '--frames', str(last),
                    '--seconds', '900', '--pad-script', PAD, '--env', 'ROR_GS=' + mode,
                    '--env', 'ROR_GSDUMP=' + ','.join('%d:3' % f for _, f, _ in CASES),
                    '--env', 'ROR_SCREENSHOT=' + ','.join(str(f + lag) for _, f, _ in CASES),
                    '--env', 'ROR_GS_INI=' + (gs_ini or 'none'), '--env', 'ROR_MC_DIR=' + card, '--env', 'ROR_MOVIES=0', '--env', 'ROR_SOUND=0'], check=True)
    dump_suffix, shot_suffix = ('.gs', '_ours.png') if mode == 'cpu' else ('_vif1.gs', '_live.png')
    for name, frame, _ in CASES:
        shutil.copyfile(os.path.join(RUN, 'gsdump_%d%s' % (frame, '' if mode == 'cpu' else '_vif1') + '.gs'),
                        os.path.join(new, name + dump_suffix))
        shutil.copyfile(os.path.join(RUN, 'screen_%d.png' % (frame + lag)), os.path.join(new, name + shot_suffix))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--mode', choices=('cpu', 'pcsx2', 'both'), default='both')
    ap.add_argument('--update', action='store_true')
    ap.add_argument('--replay', action='store_true')
    ap.add_argument('--gs-ini', help='PCSX2 GS settings for the run and the replays (compared against the 1x references)')
    args = ap.parse_args()
    if args.gs_ini and args.update:
        sys.exit('--update makes 1x references: not with --gs-ini')
    gs_ini = os.path.abspath(args.gs_ini) if args.gs_ini else None
    edges = gs_ini is not None
    modes = ('cpu', 'pcsx2') if args.mode == 'both' else (args.mode,)
    os.makedirs(REF, exist_ok=True)
    new = os.path.join(REF, 'new-' + os.path.splitext(os.path.basename(gs_ini))[0] if gs_ini else 'new')
    os.makedirs(new, exist_ok=True)
    ref = lambda name, suffix: os.path.join(REF, name + suffix)
    out = lambda name, suffix: os.path.join(new, name + suffix)
    failed = 0
    for mode in modes:
        if not args.replay:
            capture(mode, new, gs_ini)
        for name, frame, what in CASES:
            results = []
            if mode == 'cpu':
                replay(ref(name, '.gs') if args.replay else out(name, '.gs'), out(name, '_pcsx2.png'), gs_ini=gs_ini)
                results.append(('pcsx2', compare(out(name, '_pcsx2.png'), ref(name, '_pcsx2.png'), edges)))
                if not args.replay:
                    results.append(('ours', compare(out(name, '_ours.png'), ref(name, '_ours.png'), edges)))
            else:
                replay(ref(name, '_vif1.gs') if args.replay else out(name, '_vif1.gs'), out(name, '_vif1.png'), vif1=True,
                       gs_ini=gs_ini)
                if args.replay:
                    results.append(('vif1', compare(out(name, '_vif1.png'), ref(name, '_vif1.png'), edges)))
                else:
                    results.append(('live', compare(out(name, '_live.png'), ref(name, '_live.png'), edges)))
                    results.append(('live/cpu', compare(out(name, '_live.png'), ref(name, '_pcsx2.png'), edges)))
                    results.append(('vif1', compare(out(name, '_vif1.png'), out(name, '_live.png'))))
            for kind, (ok, text) in results:
                failed += not ok
                print('%-10s %-20s %-5s %-8s %s  %s' % (name, what, mode, kind, 'ok' if ok else 'DIFFERS', text), flush=True)
            if args.update and not args.replay:
                for suffix in OUTPUTS[mode]:
                    shutil.copyfile(out(name, suffix), ref(name, suffix))
    print('%s; references in %s%s' % ('all match' if not failed else '%d comparisons differ' % failed, REF,
                                     ' (updated)' if args.update else ''))
    return 1 if failed and not args.update else 0


if __name__ == '__main__':
    sys.exit(main())
