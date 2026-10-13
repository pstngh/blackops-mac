#!/usr/bin/env python3
"""Convert the game's images (IWI files, images/<name>.iwi in main/*.iwd) to PNGs.

usage: tools/iwi_to_png.py <game dir> <out dir> <image name>...
       tools/iwi_to_png.py <file.iwi> <out.png>

Finds each image in the game folder's main/*.iwd archives (the last archive that has it wins, as in the
game) and writes <out dir>/<name>.png: the largest mip level as RGBA. The IWI layout (r_image_load_obj.cpp):
a 48-byte header (GfxImageFileHeader: "IWi", version, format, flags, width, height, depth, ...), then the
mip levels from the smallest to the largest, so the full image is the file's last bytes. Formats: 1 ARGB32,
2 RGB24, 3 L8A8, 4 L8, 5 A8, 0x0B DXT1, 0x0C DXT3, 0x0D DXT5; 2D images only. Plain Python, no image library.
"""
import glob
import os
import struct
import sys
import zipfile
import zlib


def rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def decode_dxt(data, w, h, kind):
    out = bytearray(w * h * 4)
    bsize = 8 if kind == 1 else 16
    off = 0
    for by in range((h + 3) // 4):
        for bx in range((w + 3) // 4):
            block = data[off:off + bsize]
            off += bsize
            alpha = None
            cblock = block
            if kind == 3:
                bits = int.from_bytes(block[0:8], 'little')
                alpha = [((bits >> (4 * i)) & 15) * 17 for i in range(16)]
                cblock = block[8:]
            elif kind == 5:
                a0, a1 = block[0], block[1]
                if a0 > a1:
                    pal = [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
                else:
                    pal = [a0, a1] + [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255]
                bits = int.from_bytes(block[2:8], 'little')
                alpha = [pal[(bits >> (3 * i)) & 7] for i in range(16)]
                cblock = block[8:]
            c0, c1, idx = struct.unpack_from('<HHI', cblock, 0)
            p0, p1 = rgb565(c0), rgb565(c1)
            if c0 > c1 or kind != 1:
                colors = [p0 + (255,), p1 + (255,), tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
                          tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
            else:
                colors = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
            for i in range(16):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x < w and y < h:
                    r, g, b, a = colors[(idx >> (2 * i)) & 3]
                    o = (y * w + x) * 4
                    out[o:o + 4] = bytes((r, g, b, alpha[i] if alpha is not None else a))
    return out


def decode(iwi):
    if iwi[:3] != b'IWi':
        raise ValueError('not an IWI file')
    fmt = iwi[4]
    w, h, d = struct.unpack_from('<hhh', iwi, 6)
    if d > 1:
        raise ValueError('a 3D image')
    if fmt in (0x0B, 0x0C, 0x0D):
        kind = {0x0B: 1, 0x0C: 3, 0x0D: 5}[fmt]
        size = (8 if kind == 1 else 16) * ((w + 3) // 4) * ((h + 3) // 4)
        return w, h, decode_dxt(iwi[len(iwi) - size:], w, h, kind)
    bpp = {1: 4, 2: 3, 3: 2, 4: 1, 5: 1}.get(fmt)
    if not bpp:
        raise ValueError('format %d not supported' % fmt)
    data = iwi[len(iwi) - w * h * bpp:]
    out = bytearray(w * h * 4)
    for i in range(w * h):
        p = data[i * bpp:(i + 1) * bpp]
        if fmt == 1:
            px = (p[2], p[1], p[0], p[3])          # B G R A in memory
        elif fmt == 2:
            px = (p[2], p[1], p[0], 255)
        elif fmt == 3:
            px = (p[0], p[0], p[0], p[1])
        elif fmt == 4:
            px = (p[0], p[0], p[0], 255)
        else:
            px = (255, 255, 255, p[0])
        out[i * 4:i * 4 + 4] = bytes(px)
    return w, h, out


def write_png(path, w, h, rgba):
    raw = b''.join(b'\x00' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, data):
        c = struct.pack('>I', len(data)) + t + data
        return c + struct.pack('>I', zlib.crc32(t + data) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def find_in_archives(game, names):
    found = {}
    for archive in sorted(glob.glob(os.path.join(game, 'main', '*.iwd'))):
        with zipfile.ZipFile(archive) as z:
            members = {m.lower(): m for m in z.namelist()}
            for name in names:
                member = members.get('images/%s.iwi' % name.lower())
                if member:
                    found[name] = z.read(member)
    return found


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0].lower().endswith('.iwi'):
        w, h, rgba = decode(open(args[0], 'rb').read())
        write_png(args[1], w, h, rgba)
        print('%s: %dx%d -> %s' % (args[0], w, h, args[1]))
        return
    if len(args) < 3:
        raise SystemExit(__doc__)
    game, out, names = args[0], args[1], args[2:]
    os.makedirs(out, exist_ok=True)
    found = find_in_archives(game, names)
    for name in names:
        if name not in found:
            print('%s: not in %s/main/*.iwd' % (name, game))
            continue
        try:
            w, h, rgba = decode(found[name])
        except ValueError as e:
            print('%s: %s' % (name, e))
            continue
        path = os.path.join(out, name + '.png')
        write_png(path, w, h, rgba)
        print('%s: %dx%d -> %s' % (name, w, h, path))


if __name__ == '__main__':
    main()
