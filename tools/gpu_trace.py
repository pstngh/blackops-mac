#!/usr/bin/env python3
"""GPU time and command buffers of a running game, from Instruments' Metal System Trace.

usage: gpu_trace.py record <pid> <seconds> <out.trace>
       gpu_trace.py report <out.trace> [--fps F] [--process blackops]

`report` prints how busy the GPU was (the union of the trace's GPU intervals, per
channel and overall) and the command buffers and encoders submitted per second, or per
frame with --fps (the game's KB_FPS rate during the recording). The GPU's busy share
times the frame time is the GPU's time per frame; it is the limit when it nears 100%.
Recording costs the game a few percent.
"""
import collections
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def record(pid, seconds, out):
    if os.path.exists(out):
        subprocess.run(['rm', '-rf', out], check=True)
    r = subprocess.run(['xcrun', 'xctrace', 'record', '--template', 'Metal System Trace', '--attach', str(pid),
                        '--time-limit', f'{int(seconds * 1000)}ms', '--output', out], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        sys.exit(f'xctrace record failed:\n{r.stdout}{r.stderr}')


def rows(trace, schema):
    """The rows of one table as dicts (column mnemonic -> text), xctrace's id/ref references resolved."""
    xml = os.path.join(tempfile.mkdtemp(), 'table.xml')
    try:
        r = subprocess.run(['xcrun', 'xctrace', 'export', '--input', trace, '--output', xml, '--xpath',
                            f'/trace-toc/run[@number="1"]/data/table[@schema="{schema}"]'],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f'xctrace export failed:\n{r.stdout}{r.stderr}')
        ids, cols = {}, []
        for ev, el in ET.iterparse(xml, events=('end',)):
            if el.tag == 'mnemonic':
                cols.append(el.text)
            if el.tag != 'row':
                continue
            out = {}
            for i, c in enumerate(list(el)):
                for sub in c.iter():
                    if sub.get('id') is not None:
                        ids[sub.get('id')] = (sub.text, sub.get('fmt'))
                v = ids.get(c.get('ref')) if c.get('ref') is not None else (c.text, c.get('fmt'))
                if i < len(cols):
                    out[cols[i]] = v
            yield out
            el.clear()
    finally:
        if os.path.exists(xml):
            os.unlink(xml)
        os.rmdir(os.path.dirname(xml))


def union(intervals):
    tot, cs, ce = 0, None, None
    for s, e in sorted(intervals):
        if cs is None or s > ce:
            if cs is not None:
                tot += ce - cs
            cs, ce = s, e
        else:
            ce = max(ce, e)
    return tot + (ce - cs if cs is not None else 0)


def fmt(v):
    return (v[1] or v[0] or '') if v else ''


def report(trace, fps, process):
    gpu = []
    for r in rows(trace, 'metal-gpu-intervals'):
        if process in fmt(r.get('process')):
            start, dur = int(r['start'][0]), int(r['duration'][0])
            gpu.append((fmt(r.get('channel-name')), start, start + dur))
    if not gpu:
        sys.exit(f'no GPU intervals for "{process}"')
    t0 = min(s for c, s, e in gpu)
    t1 = max(e for c, s, e in gpu)
    span = (t1 - t0) / 1e9
    per = 1.0 / fps if fps else 1.0
    unit = 'frame' if fps else 's'
    print(f'{span:.2f} s of GPU activity')
    by = collections.defaultdict(list)
    for c, s, e in gpu:
        by[c].append((s, e))
    for c, iv in sorted(by.items()):
        b = union(iv) / 1e9 / span
        print(f'  {c:12s} busy {100 * b:5.1f}%' + (f'  {1000 * b * per:6.2f} ms/frame' if fps else ''))
    b = union([(s, e) for c, s, e in gpu]) / 1e9 / span
    print(f'  {"any":12s} busy {100 * b:5.1f}%' + (f'  {1000 * b * per:6.2f} ms/frame' if fps else ''))
    cbs = [r for r in rows(trace, 'metal-application-command-buffer-submissions') if process in fmt(r.get('process'))]
    encs = sum(int(fmt(r.get('num-encoders')) or 0) for r in cbs)
    print(f'  {len(cbs) / span * per:.1f} command buffers, {encs / span * per:.1f} encoders per {unit}')


def main():
    a = sys.argv[1:]
    if len(a) >= 4 and a[0] == 'record':
        record(int(a[1]), float(a[2]), a[3])
    elif len(a) >= 2 and a[0] == 'report':
        fps = float(a[a.index('--fps') + 1]) if '--fps' in a else None
        proc = a[a.index('--process') + 1] if '--process' in a else 'blackops'
        report(a[1], fps, proc)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
