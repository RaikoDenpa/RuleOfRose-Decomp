"""TIM2 textures (Sony PS2 standard format) -> PNG."""
import argparse
import struct
import sys
import zlib

IMG_RGBA16, IMG_RGB24, IMG_RGBA32, IMG_4BIT, IMG_8BIT = 1, 2, 3, 4, 5


def csm1(i):
    return (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)


def ps2_alpha(a):
    return min(255, a * 2)


def colors(raw, ctype):
    out = []
    if ctype == 1:
        for (c,) in struct.iter_unpack('<H', raw[:len(raw) // 2 * 2]):
            out.append([(c & 0x1f) * 255 // 31, ((c >> 5) & 0x1f) * 255 // 31, ((c >> 10) & 0x1f) * 255 // 31, 255 if c & 0x8000 else 0])
    elif ctype == 2:
        for i in range(0, len(raw) - 2, 3):
            out.append([raw[i], raw[i + 1], raw[i + 2], 255])
    elif ctype == 3:
        for i in range(0, len(raw) - 3, 4):
            out.append([raw[i], raw[i + 1], raw[i + 2], ps2_alpha(raw[i + 3])])
    else:
        raise ValueError('unsupported colour type %d' % ctype)
    return [tuple(o) for o in out]


class Picture:
    def __init__(self, data, off):
        (self.total_size, self.clut_size, self.image_size, self.header_size, self.clut_colors, self.pict_format,
         self.mipmaps, self.clut_type, self.image_type, self.width, self.height, self.tex0, self.tex1,
         self.texa, self.texclut) = struct.unpack_from('<IIIHHBBBBHHQQII', data, off)
        self.offset = off
        self.image = data[off + self.header_size: off + self.header_size + self.image_size]
        c0 = off + self.header_size + self.image_size
        self.clut = data[c0: c0 + self.clut_size]

    def describe(self):
        return ('%dx%d image_type=%d clut_type=%#x clut_colors=%d mipmaps=%d header=%#x image=%#x clut=%#x TEX0=%#x'
                % (self.width, self.height, self.image_type, self.clut_type, self.clut_colors, self.mipmaps,
                   self.header_size, self.image_size, self.clut_size, self.tex0))

    @property
    def tcc(self):
        return (self.tex0 >> 34) & 1

    def rgba(self, clut_index=0, alpha='tex0'):
        rows = self._rgba(clut_index)
        if alpha == 'opaque' or (alpha == 'tex0' and self.tcc == 0):
            rows = [bytes(b if i % 4 != 3 else 255 for i, b in enumerate(r)) for r in rows]
        return rows

    def _rgba(self, clut_index):
        w, h, t = self.width, self.height, self.image_type
        if t in (IMG_4BIT, IMG_8BIT):
            raw = colors(self.clut, self.clut_type & 0x3f)
            n = 16 if t == IMG_4BIT else 256
            swz = not self.clut_type & 0x80
            idx = [csm1(clut_index * n + i) if swz else clut_index * n + i for i in range(n)]
            if max(idx) >= len(raw):
                raise ValueError('CLUT %d out of range (%d colours)' % (clut_index, len(raw)))
            pal = [bytes(raw[j]) for j in idx]
            rows = []
            if t == IMG_8BIT:
                for y in range(h):
                    rows.append(b''.join(pal[p] for p in self.image[y * w:(y + 1) * w]))
            else:
                stride = (w + 1) // 2
                for y in range(h):
                    row = self.image[y * stride:(y + 1) * stride]
                    px = []
                    for b in row:
                        px.append(pal[b & 0xf])
                        px.append(pal[b >> 4])
                    rows.append(b''.join(px[:w]))
            return rows
        bpp = {IMG_RGBA16: 2, IMG_RGB24: 3, IMG_RGBA32: 4}[t]
        cols = colors(self.image[:w * h * bpp], {IMG_RGBA16: 1, IMG_RGB24: 2, IMG_RGBA32: 3}[t])
        return [b''.join(bytes(c) for c in cols[y * w:(y + 1) * w]) for y in range(h)]


def parse(data):
    if data[:4] != b'TIM2':
        raise ValueError('not a TIM2 file')
    version, fmt, count = data[4], data[5], struct.unpack_from('<H', data, 6)[0]
    off = 0x80 if fmt == 1 else 0x10
    pics = []
    for _ in range(count):
        p = Picture(data, off)
        pics.append(p)
        off += p.total_size
    return version, fmt, pics


def write_png(path, w, h, rows):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(b''.join(b'\0' + r for r in rows), 6)))
        f.write(chunk(b'IEND', b''))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('file')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--png', metavar='OUT')
    ap.add_argument('--clut', type=int, default=0)
    ap.add_argument('--alpha', choices=('tex0', 'keep', 'opaque'), default='tex0')
    a = ap.parse_args()
    version, fmt, pics = parse(open(a.file, 'rb').read())
    if a.dump:
        print('TIM2 version %d format %d pictures %d' % (version, fmt, len(pics)))
        for i, p in enumerate(pics):
            print('  %d: %s' % (i, p.describe()))
    if a.png:
        for i, p in enumerate(pics):
            out = a.png if i == 0 else a.png.replace('.png', '_%d.png' % i)
            write_png(out, p.width, p.height, p.rgba(a.clut, a.alpha))
            print('wrote', out)
    if not (a.dump or a.png):
        ap.print_help()
        sys.exit(1)


if __name__ == '__main__':
    main()
