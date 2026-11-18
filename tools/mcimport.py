"""PS2 memory cards (PCSX2) -> the port's host card folder (src/ror_mc.cpp): imports real-game saves."""
import argparse, datetime, os, shutil, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_TO = os.path.join(ROOT, 'extracted', 'ps2recomp', 'run', 'memcard', 'slot1')
MAGIC = b'Sony PS2 Memory Card Format '
JST = datetime.timezone(datetime.timedelta(hours=9))


class Image:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        if not self.d.startswith(MAGIC):
            sys.exit('%s: no PS2 memory card superblock' % path)
        self.page, self.ppc = struct.unpack_from('<HH', self.d, 0x28)
        self.alloc, = struct.unpack_from('<I', self.d, 0x34)
        self.root, = struct.unpack_from('<I', self.d, 0x3c)
        self.ifc = struct.unpack_from('<32I', self.d, 0x50)
        clusters, = struct.unpack_from('<I', self.d, 0x30)
        pages = clusters * self.ppc
        self.raw_page = self.page + 16 if len(self.d) == pages * (self.page + 16) else self.page
        if len(self.d) < pages * self.raw_page:
            sys.exit('%s: %d bytes, too short for %d pages' % (path, len(self.d), pages))
        self.csize = self.page * self.ppc

    def cluster(self, n):
        out = b''
        for p in range(n * self.ppc, (n + 1) * self.ppc):
            out += self.d[p * self.raw_page:p * self.raw_page + self.page]
        return out

    def fat(self, n):
        per = self.csize // 4
        ind = self.ifc[n // per // per]
        fat_cluster, = struct.unpack_from('<I', self.cluster(ind), (n // per % per) * 4)
        entry, = struct.unpack_from('<I', self.cluster(fat_cluster), (n % per) * 4)
        return entry

    def chain(self, first, nbytes):
        out, n, seen = b'', first, set()
        while len(out) < nbytes:
            if n in seen:
                sys.exit('FAT loop at cluster %d' % n)
            seen.add(n)
            out += self.cluster(self.alloc + n)
            entry = self.fat(n)
            if entry == 0xffffffff or not entry & 0x80000000:
                break
            n = entry & 0x7fffffff
        return out[:nbytes]

    @staticmethod
    def entry(b):
        mode, length = struct.unpack_from('<HxxI', b, 0)
        cluster, = struct.unpack_from('<I', b, 0x10)
        sec, mn, hr, day, mon, year = struct.unpack_from('<xBBBBBH', b, 0x18)
        name = b[0x40:0x60].split(b'\0')[0].decode('ascii', 'replace')
        try:
            mtime = datetime.datetime(year, mon, day, hr, mn, sec, tzinfo=JST).timestamp()
        except ValueError:
            mtime = None
        return {'mode': mode, 'length': length, 'cluster': cluster, 'name': name, 'mtime': mtime}

    def listdir(self, first, count=None):
        if count is None:
            count = self.entry(self.cluster(self.alloc + first)[:512])['length']
        raw = self.chain(first, count * 512)
        out = []
        for i in range(count):
            e = self.entry(raw[i * 512:(i + 1) * 512])
            if e['mode'] & 0x8000 and e['name'] not in ('.', '..'):
                out.append(e)
        return out

    def walk(self):
        for d in self.listdir(self.root):
            yield d['name'], d
            if d['mode'] & 0x20:
                for f in self.listdir(d['cluster'], d['length']):
                    yield d['name'] + '/' + f['name'], f

    def files(self, dirname):
        for d in self.listdir(self.root):
            if d['mode'] & 0x20 and d['name'] == dirname:
                return [(f['name'], self.chain(f['cluster'], f['length']), f['mtime'])
                        for f in self.listdir(d['cluster'], d['length']) if f['mode'] & 0x10]
        return None


class Folder:
    def __init__(self, path):
        self.path = path

    def walk(self):
        for d in sorted(os.listdir(self.path)):
            p = os.path.join(self.path, d)
            if os.path.isdir(p):
                yield d, {'mode': 0x8027, 'length': 0, 'name': d, 'mtime': os.path.getmtime(p)}
                for f in sorted(os.listdir(p)):
                    if not f.startswith('_pcsx2_'):
                        q = os.path.join(p, f)
                        yield d + '/' + f, {'mode': 0x8017, 'length': os.path.getsize(q), 'name': f,
                                            'mtime': os.path.getmtime(q)}

    def files(self, dirname):
        p = os.path.join(self.path, dirname)
        if not os.path.isdir(p):
            return None
        return [(f, open(os.path.join(p, f), 'rb').read(), os.path.getmtime(os.path.join(p, f)))
                for f in sorted(os.listdir(p)) if not f.startswith('_pcsx2_') and os.path.isfile(os.path.join(p, f))]


def open_card(path):
    if os.path.isdir(path):
        if not os.path.exists(os.path.join(path, '_pcsx2_superblock')):
            sys.exit('%s: not a PCSX2 folder memory card (no _pcsx2_superblock)' % path)
        return Folder(path)
    return Image(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('card')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--dir', default='BESLES-54218')
    ap.add_argument('--to', default=DEFAULT_TO)
    ap.add_argument('--force', action='store_true')
    args = ap.parse_args()
    card = open_card(args.card)
    if args.dump:
        for path, e in card.walk():
            kind = 'dir ' if e['mode'] & 0x20 else 'file'
            when = datetime.datetime.fromtimestamp(e['mtime']).strftime('%Y-%m-%d %H:%M') if e['mtime'] else '?'
            print('%s %-40s %8d  %s' % (kind, path, e['length'], when))
        return
    files = card.files(args.dir)
    if files is None:
        sys.exit('%s: no directory %s on the card (--dump lists them)' % (args.card, args.dir))
    dest = os.path.join(args.to, args.dir)
    if os.path.exists(dest):
        if not args.force:
            sys.exit('%s exists: --force replaces it' % dest)
        shutil.rmtree(dest)
    os.makedirs(dest)
    for name, data, mtime in files:
        p = os.path.join(dest, name)
        open(p, 'wb').write(data)
        if mtime:
            os.utime(p, (mtime, mtime))
        print('%-20s %8d bytes' % (name, len(data)))
    print('%d files -> %s' % (len(files), dest))


if __name__ == '__main__':
    main()
