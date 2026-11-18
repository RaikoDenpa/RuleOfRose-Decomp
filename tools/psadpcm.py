import struct

COEF = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))


def decode(data, state=None, stop_at_end_flag=False):
    h1, h2 = state if state else (0, 0)
    out = []
    append = out.append
    for p in range(0, len(data) - 15, 16):
        hdr, flags = data[p], data[p + 1]
        shift = hdr & 0x0f
        if shift > 12:
            shift = 9
        c0, c1 = COEF[(hdr >> 4) if (hdr >> 4) < 5 else 0]
        for b in data[p + 2:p + 16]:
            for n in (b & 0x0f, b >> 4):
                s = ((n - 16 if n & 8 else n) << 12) >> shift
                s += (h1 * c0 + h2 * c1 + 32) >> 6
                s = -32768 if s < -32768 else 32767 if s > 32767 else s
                append(s)
                h2, h1 = h1, s
        if stop_at_end_flag and flags & 1:
            break
    if state is not None:
        state[0], state[1] = h1, h2
    return out


def write_wav(path, rate, channels):
    n = min(len(c) for c in channels)
    nch = len(channels)
    if nch == 1:
        pcm = struct.pack('<%dh' % n, *channels[0][:n])
    else:
        inter = [0] * (n * nch)
        for k, c in enumerate(channels):
            inter[k::nch] = c[:n]
        pcm = struct.pack('<%dh' % len(inter), *inter)
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' +
                struct.pack('<IHHIIHH', 16, 1, nch, rate, rate * 2 * nch, 2 * nch, 16) +
                b'data' + struct.pack('<I', len(pcm)) + pcm)
