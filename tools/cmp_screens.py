#!/usr/bin/env python3
"""Compare two directories of TGA screenshots (same file names), e.g. two backends.

usage: cmp_screens.py <dirA> <dirB> [--threshold T] [--max-pct P] [--diff DIR]

For each pair: the mean absolute difference per channel (0-255) and the share of
pixels where any channel differs by more than T (default 24). Exits 1 when an image
differs in more than P percent of its pixels (default 1.0), or has no partner or another
size. --diff writes DIR/<name>.tga, the difference x4, to look at.

Two runs of the same build differ too (the weapon's idle sway, the fps counter,
animated effects), at T 24 on bench_views.py's views: mp_nuked a-f 0.2-0.4%; the
overviews 0.05-1% on most maps, up to ~5% with rain, smoke or moving lights (mp_hotel
5.0, mp_radiation 3.5, mp_kowloon 2.9, mp_cracked 2.3, mp_mountain 2.0). Compare two
runs of the reference build first to set P for a map.
"""
import os
import sys


def load(path):
    d = open(path, 'rb').read()
    idlen, cmap, kind = d[0], d[1], d[2]
    w, h, bpp, desc = d[12] | d[13] << 8, d[14] | d[15] << 8, d[16] // 8, d[17]
    if cmap or kind != 2 or bpp not in (3, 4):
        raise ValueError(f'{path}: not an uncompressed 24/32-bit TGA')
    px = d[18 + idlen:18 + idlen + w * h * bpp]
    rows = [px[y * w * bpp:(y + 1) * w * bpp] for y in range(h)]
    if not desc & 0x20:   # bottom-up
        rows.reverse()
    return w, h, bpp, rows


def compare(a, b, threshold, diff_path):
    wa, ha, ba, ra = load(a)
    wb, hb, bb, rb = load(b)
    if (wa, ha) != (wb, hb):
        return None
    total = big = 0
    out = [] if diff_path else None
    for y in range(ha):
        x, z = ra[y], rb[y]
        if x == z and ba == bb:
            if out is not None:
                out.append(bytes(wa * 3))
            continue
        line = bytearray(wa * 3) if out is not None else None
        for i in range(wa):
            p, q = i * ba, i * bb
            d0, d1, d2 = abs(x[p] - z[q]), abs(x[p + 1] - z[q + 1]), abs(x[p + 2] - z[q + 2])
            total += d0 + d1 + d2
            if d0 > threshold or d1 > threshold or d2 > threshold:
                big += 1
            if line is not None:
                line[i * 3:i * 3 + 3] = bytes((min(255, d0 * 4), min(255, d1 * 4), min(255, d2 * 4)))
        if out is not None:
            out.append(bytes(line))
    if out is not None:
        hdr = bytes([0, 0, 2] + [0] * 9 + [wa & 255, wa >> 8, ha & 255, ha >> 8, 24, 0x20])
        with open(diff_path, 'wb') as f:
            f.write(hdr + b''.join(out))
    n = wa * ha
    return total / (3 * n), 100.0 * big / n


def main():
    a = sys.argv[1:]
    def opt(name, default):
        if name in a:
            i = a.index(name)
            v = a[i + 1]
            del a[i:i + 2]
            return type(default)(v)
        return default
    threshold, max_pct, diff = opt('--threshold', 24), opt('--max-pct', 1.0), opt('--diff', '')
    if len(a) != 2:
        sys.exit(__doc__)
    da, db = a
    if diff:
        os.makedirs(diff, exist_ok=True)
    bad = 0
    names = sorted(set(f for f in os.listdir(da) if f.lower().endswith('.tga')) |
                   set(f for f in os.listdir(db) if f.lower().endswith('.tga')))
    for f in names:
        pa, pb = os.path.join(da, f), os.path.join(db, f)
        if not (os.path.exists(pa) and os.path.exists(pb)):
            print(f'{f}: only in {da if os.path.exists(pa) else db}')
            bad += 1
            continue
        r = compare(pa, pb, threshold, os.path.join(diff, f) if diff else None)
        if r is None:
            print(f'{f}: sizes differ')
            bad += 1
            continue
        mean, pct = r
        flag = '' if pct <= max_pct else '  DIFFERENT'
        bad += pct > max_pct
        print(f'{f}: mean |d| {mean:6.2f}, {pct:6.2f}% of pixels > {threshold}{flag}')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
