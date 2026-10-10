#!/usr/bin/env python3
"""Summaries of a macOS `sample` call graph (sample <pid> <seconds> -file <file>).

usage: sample_report.py <command> <file.sample> [thread] [args]

  threads <file>                      each thread's samples and busy share: time not blocked in the
                                      kernel (mach messages, e.g. the swap's WindowServer calls, count as
                                      blocked)
  self    <file> <thread> [--top N]   self time per function [library]
  libs    <file> <thread>             self time per library (the GL layer split from the engine)
  incl    <file> <thread> <regex>...  inclusive time of the outermost frames matching each regex
  callers <file> <thread> <regex> [--levels N]
                                      call chains leading to self time in matching frames
  tree    <file> <thread> <regex> [--depth N] [--min-pct P]
                                      the call tree under the frames matching regex, merged

<thread> is a thread number from `threads`, `main` (the main thread) or `render`
(the thread running RB_RenderThread). Function names are matched without arguments
for incl/callers/tree, e.g. '^glDrawElementsBaseVertex' or 'GLDevice::Present'.
"""
import collections
import re
import sys

WAIT_FUNCS = re.compile(
    r'^(__psynch_cvwait|__psynch_mutexwait|__workq_kernreturn|semaphore_wait_trap|semaphore_wait_signal_trap|semaphore_timedwait_trap|'
    r'kevent_id|kevent|kevent64|mach_msg2_trap|mach_msg_trap|__ulock_wait2?|swtch_pri|__semwait_signal|'
    r'__select|__recvfrom|nanosleep|__wait4|__sigsuspend)$')


_loaded = {}


def load(path):
    """The call graph's lines as (thread, depth, count, name, library)."""
    if path not in _loaded:
        _loaded[path] = list(_parse(path))
    return _loaded[path]


def _parse(path):
    lines = open(path, errors='replace').read().split('\n')
    start = next(i for i, l in enumerate(lines) if l.startswith('Call graph:'))
    end = next((i for i, l in enumerate(lines) if i > start and l.startswith('Total number in stack')), len(lines))
    thread = None
    for l in lines[start + 1:end]:
        m = re.match(r'^(\s*)([+!:| ]*)(\d+)\s+(.*)$', l)
        if not m:
            continue
        depth = len(m.group(1)) + len(m.group(2))
        count = int(m.group(3))
        rest = m.group(4)
        if rest.startswith('Thread_'):
            thread = rest
            yield thread, -1, count, rest, ''
            continue
        rest = re.sub(r'\s+[\w.+-]+:\d+$', '', rest)      # rb_shade.cpp:346
        rest = re.sub(r'\s+\[0x[^\]]*\]$', '', rest)   # [0x1a2b,0x3c4d,...]
        rest = re.sub(r'\s+\+ [\d,.]+$', '', rest)     # + 180,24,...
        lm = re.match(r'(.*?)\s+\(in ([^)]+)\)$', rest)
        name, lib = (lm.group(1), lm.group(2)) if lm else (rest, '?')
        yield thread, depth, count, name, lib


def short(name):
    return name.split('(')[0] if not name.startswith('(') else name


def thread_ids(path):
    ids = collections.OrderedDict()
    for thread, depth, count, name, lib in load(path):
        if depth < 0:
            m = re.match(r'Thread_(\d+|<multiple>)', thread)
            if m and m.group(1) != '<multiple>':
                ids.setdefault(m.group(1), thread)
    return ids


def resolve_thread(path, sel):
    ids = thread_ids(path)
    if sel in ('main', 'render'):
        want = 'com.apple.main-thread' if sel == 'main' else None
        hits = collections.Counter()
        for thread, depth, count, name, lib in load(path):
            m = re.match(r'Thread_(\d+)', thread or '')
            if not m:
                continue
            if want and depth < 0 and want in thread:
                return m.group(1)
            if not want and depth >= 0 and name.startswith('RB_RenderThread'):
                hits[m.group(1)] += count
        if hits:
            return hits.most_common(1)[0][0]
        sys.exit(f'no {sel} thread found')
    if sel not in ids:
        sys.exit(f'thread {sel} not in the sample (threads: {", ".join(ids)})')
    return sel


def walk(path, tid):
    """Yields (stack, count, self) per node of one thread: stack is the list of (name, lib)."""
    stack = []  # [depth, count, name, lib, children]
    def pop_to(depth):
        while stack and stack[-1][0] >= depth:
            d, c, n, lb, kids = stack.pop()
            yield [(s[2], s[3]) for s in stack] + [(n, lb)], c, c - kids
    for thread, depth, count, name, lib in load(path):
        mine = thread and re.match(rf'Thread_{tid}\b', thread)
        if depth < 0 or not mine:
            yield from pop_to(-1)
            continue
        yield from pop_to(depth)
        if stack:
            stack[-1][4] += count
        stack.append([depth, count, name, lib, 0])
    yield from pop_to(-1)


def total(path, tid):
    return sum(c for st, c, s in walk(path, tid) if len(st) == 1)


def cmd_threads(path):
    rows = []
    ids = thread_ids(path)
    for tid, label in ids.items():
        tot = busy = 0
        render = False
        for st, c, s in walk(path, tid):
            if len(st) == 1:
                tot += c
            if s > 0 and not WAIT_FUNCS.match(st[-1][0]):
                busy += s
            render = render or st[-1][0].startswith('RB_RenderThread')
        if tot:
            label = label[len('Thread_') + len(tid):].strip() + (' (render thread)' if render else '')
            rows.append((busy / tot, tid, tot, label))
    for share, tid, tot, label in sorted(rows, reverse=True):
        print(f'{tid:>10} {tot:7d} samples, busy {100 * share:5.1f}%  {label}')


def cmd_self(path, tid, top):
    selfc = collections.Counter()
    for st, c, s in walk(path, tid):
        if s > 0:
            selfc[f'{st[-1][0]} [{st[-1][1]}]'] += s
    tot = sum(selfc.values())
    print(f'total samples {tot}')
    for n, c in selfc.most_common(top):
        print(f'{c:7d} {100 * c / tot:5.1f}%  {n[:160]}')


GL_LAYER = re.compile(r'^(GLDevice|GLVertex|GLIndex|GLTexture|GLVolume|GLCube|GLSurface|GLSwapChain|GLQuery|KB_|'
                      r'\(anonymous namespace\)::SDLGLContext|\(anonymous namespace\)::SwapWithStats)')


def cmd_libs(path, tid):
    libs = collections.Counter()
    for st, c, s in walk(path, tid):
        if s > 0:
            name, lib = st[-1]
            if lib == 'blackops' and GL_LAYER.match(name):
                lib = 'blackops: D3D9 translation layer'
            elif lib == 'blackops':
                lib = 'blackops: engine'
            libs[lib] += s
    tot = sum(libs.values())
    for k, v in libs.most_common(25):
        print(f'{v:7d} {100 * v / tot:5.1f}%  {k}')


def cmd_incl(path, tid, pats):
    rx = [re.compile(p) for p in pats]
    incl = collections.Counter()
    tot = 0
    for st, c, s in walk(path, tid):
        if len(st) == 1:
            tot += c
        for r in rx:
            if r.search(short(st[-1][0])) and not any(r.search(short(n)) for n, lb in st[:-1]):
                incl[r.pattern] += c
    for r in rx:
        print(f'{incl[r.pattern]:7d} {100 * incl[r.pattern] / max(tot, 1):5.1f}%  {r.pattern}')
    print(f'{tot:7d} total')


def cmd_callers(path, tid, pat, levels):
    rx = re.compile(pat)
    agg = collections.Counter()
    for st, c, s in walk(path, tid):
        if s > 0 and rx.search(short(st[-1][0])):
            agg[' < '.join(short(n)[:70] for n, lb in reversed(st[-levels - 1:]))] += s
    print('total', sum(agg.values()))
    for k, v in agg.most_common(25):
        print(f'{v:6d}  {k}')


def cmd_tree(path, tid, pat, maxd, minpct):
    rx = re.compile(pat)
    tree = {}
    tot = 0
    for st, c, s in walk(path, tid):
        idx = next((i for i, (n, lb) in enumerate(st) if rx.search(short(n))), None)
        if idx is None:
            continue
        sub = st[idx:]
        if len(sub) > maxd + 1:
            continue
        node = tree
        for n, lb in sub[:-1]:
            node = node.setdefault(short(n), [0, {}])[1]
        node.setdefault(short(sub[-1][0]), [0, {}])[0] += c
        if len(sub) == 1:
            tot += c
    def pr(node, ind):
        for k, (c, ch) in sorted(node.items(), key=lambda kv: -kv[1][0]):
            if c < tot * minpct / 100:
                continue
            print(f'{"  " * ind}{c:6d} {k[:120]}')
            pr(ch, ind + 1)
    pr(tree, 0)


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit(__doc__)
    def opt(name, default):
        if name in a:
            i = a.index(name)
            v = a[i + 1]
            del a[i:i + 2]
            return type(default)(v)
        return default
    top, levels, depth, minpct = opt('--top', 40), opt('--levels', 8), opt('--depth', 6), opt('--min-pct', 1.0)
    cmd, path = a[0], a[1]
    if cmd == 'threads':
        return cmd_threads(path)
    if len(a) < 3:
        sys.exit(__doc__)
    tid = resolve_thread(path, a[2])
    if cmd == 'self':
        cmd_self(path, tid, top)
    elif cmd == 'libs':
        cmd_libs(path, tid)
    elif cmd == 'incl':
        cmd_incl(path, tid, a[3:])
    elif cmd == 'callers':
        cmd_callers(path, tid, a[3], levels)
    elif cmd == 'tree':
        cmd_tree(path, tid, a[3], depth, minpct)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
