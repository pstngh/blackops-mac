#!/usr/bin/env python3
"""Find locals IDA split out of one array, and code that relies on the i386 frame.

IDA often turns a stack array into separate scalar locals, and the code then uses
the first one's address as the array: `Vec3Normalize(&v10)` with v10, v11, v12
declared as three floats, `*(&v29 + k)`, `I_strncpyz(&string, src, 4096)` with
`char string;`, or an inlined strcat that starts at the byte before a buffer,
`p = (char *)&prevLocal + 3; while ( *++p );`. Nothing keeps such locals adjacent
on arm64, or in any recompiled build: undefined behaviour that ASan only catches
on the paths a run takes (HANDOFF, Decisions session 14). The fix is an array
rebuilt from IDA's stack offsets.

Regex based (no compile, ~20 s) over the engine sources: third-party directories
and `#if 0` blocks are skipped. Every hit needs a look. Kinds:

  addr    '&scalar' (a local or parameter) passed where the callee, followed
          through its own calls, reads the parameter past element 0
  extent  an array, '&arr[k]' or 'arr + k' passed where the callee reads more
          elements than are left
  arith   pointer arithmetic on a scalar's address, or a cast of it to a wider type
  strcat  'p = &x' or 'p = (char *)&x + 3' then walked with '*++p' (an inlined strcat
          that starts at the byte before the buffer, or past a local)
  member  '&s.field' of a scalar field passed where a vector is read, or cast wide
  split   'char x;' declared right before '_BYTE vN[...]' at the next stack offset

  tools/audit_splitlocals.py [src_dir] [--kind addr,extent,...] [--all]

The sites reviewed in session 14 and left as they are (counts of 1, in-bounds
casts, unreachable branches) are in REVIEWED and print only with --all.
"""
import collections, functools, os, re, sys

SKIP_DIRS = {'libs', 'zlib', 'jpeg', 'vpx', 'speex', 'minilzo', 'tracy', 'mjpeg', 'nvapi',
             'binklib', 'wma', 'steam'}
EXTS = ('.cpp', '.c', '.h', '.mm')
INF = 10 ** 6
SIZES = {'char': 1, '_BYTE': 1, '__int8': 1, 'unsigned __int8': 1, 'bool': 1, 'unsigned char': 1,
         'BYTE': 1, '__int16': 2, '_WORD': 2, 'unsigned __int16': 2, 'short': 2,
         'unsigned short': 2, 'WORD': 2, 'float': 4, 'int': 4, '_DWORD': 4, 'unsigned int': 4,
         'DWORD': 4, 'unsigned': 4, 'long': 4, '__int32': 4, 'unsigned long': 4, 'vec_t': 4,
         'double': 8, '__int64': 8, '_QWORD': 8, 'unsigned __int64': 8, 'long double': 8,
         'long long': 8, 'unsigned long long': 8, '_OWORD': 16, '__m128': 16, 'float4': 16,
         '__m128i': 16, 'hybrid_vector': 16, 'phys_vec3': 16,
         'size_t': 8, 'lzo_uint': 8, 'uintptr_t': 8, 'intptr_t': 8, 'ptrdiff_t': 8}
SCALAR = re.compile(r'(?:const\s+)?(?:volatile\s+)?(?:unsigned\s+|signed\s+)?'
                    r'(?:float|double|long double|int|char|short|long|__int8|__int16|__int32|'
                    r'__int64|_DWORD|_WORD|_BYTE|_QWORD|DWORD|BYTE|bool|vec_t|unsigned|'
                    r'unsigned __int8|unsigned __int16|unsigned __int64)')
STRUCT_HEAD = re.compile(r'\b(?:struct|union|class)\s+(?:(?:__declspec|alignas)\s*\((?:[^()]|\([^()]*\))*\)\s*)*\w+'
                         r'\s*(?::[^{;()]*)?\s*$')
# Callees without a definition here that read a char pointer as a string.
STRFUNCS = {'strcpy', 'strcat', 'strncpy', 'strncat', 'sprintf', 'snprintf', '_snprintf',
            'vsnprintf', 'sscanf', 'atof', 'atoi', 'strtol', 'strtod', 'fopen', 'strlen',
            'strcmp', 'strncmp', 'strstr', 'strchr', 'strrchr', 'puts', 'printf'}

# (path under src/, kind, name or '*') -> why it is fine
REVIEWED = {
    ('ui/ui_main.cpp', 'addr', 'replaceInt'): 'count 1',
    ('ui/ui_main.cpp', 'addr', 'replaceString'): 'count 1',
    ('clientscript/cscr_compiler.cpp', 'addr', 'childBlock'): 'Scr_*ChildBlocks count 1',
    ('server_mp/sv_snapshot_mp.cpp', 'addr', 'buffHexCharacter'): 'count 1',
    ('ui_mp/ui_gametype_custom_mp.cpp', 'addr', '*'): 'MemFile_ReadData count 1',
    ('ui_mp/ui_gametype_variants_mp.cpp', 'addr', 'p'): 'MemFile_ReadData count 1',
    ('gfx_d3d/r_marks.cpp', 'addr', 'surfCount'): 'R_BoxSurfaces list count 1',
    ('bgame/bg_fire.cpp', 'addr', 'surfCount'): 'R_BoxSurfaces list count 1',
    ('bgame/bg_animation.cpp', 'addr', '*'): 'Com_Bit*Assert on one 32-bit word',
    ('client_mp/cl_main_mp.cpp', 'addr', '*'): 'FS_Read/FS_WriteToDemo of 1 byte',
    ('win32/win_net.cpp', 'addr', 'type'): 'Sys_*DebugSocket* of 1 byte',
    ('glass/glass_shard.cpp', 'extent', 'hitPoint2d'): 'ToWorldPos(.., is3D = 0) reads 2',
    ('vehicle/nitrous_vehicle.cpp', 'extent', '*'): 'MatrixVecMultiply 3x3 overload',
    ('cgame/cg_spawn.cpp', 'arith', 'spawnFlags'): 'CG_InitTriggerWait(.., 64) never writes it',
    ('ui/ui_shared.cpp', 'member', 'horzAlign'): 'horzAlign+vertAlign, one 8-byte copy',
    ('client_mp/sv_client_mp.cpp', 'member', 'port'): 'netadr_t tail, in bounds',
    ('qcommon/net_chan_mp.cpp', 'member', 'port'): 'netadr_t tail, in bounds',
    ('physics/destructible.cpp', 'member', '*'): 'adjacent ints of one struct',
    ('game/g_mover.cpp', 'member', 'midTime'): 'a vec3 at offset 16 of the gentity union, in bounds',
    ('sound/snd_public_async_q.cpp', 'member', 'lengthMs'): 'lengthMs+playedMs in both structs',
}


def blank_if0(src):
    """Blank the lines of '#if 0' blocks up to their '#else' or '#endif'."""
    out = src.split('\n')
    dead, depth = False, 0
    for i, l in enumerate(out):
        t = l.strip()
        if not dead:
            if re.match(r'#\s*if\s+0\b', t):
                dead, depth, out[i] = True, 0, ''
            continue
        if re.match(r'#\s*if', t):
            depth += 1
        elif re.match(r'#\s*endif', t):
            if depth == 0:
                dead = False
            else:
                depth -= 1
        elif re.match(r'#\s*(else|elif)', t) and depth == 0:
            dead = False
        out[i] = ''
    return '\n'.join(out)


def strip(src):
    """Blank comments, literals and preprocessor lines, keeping offsets and newlines."""
    src = blank_if0(src)
    out, i, n = list(src), 0, len(src)
    line_start = True
    while i < n:
        c = src[i]
        if c == '\n':
            line_start = True; i += 1; continue
        if c in ' \t':
            i += 1; continue
        if c == '/' and src[i + 1:i + 2] == '/' or c == '#' and line_start:
            j = i
            while True:
                j = src.find('\n', j)
                if j < 0:
                    j = n; break
                if c == '#' and src[j - 1] == '\\':
                    j += 1; continue
                break
            for k in range(i, j):
                if out[k] != '\n': out[k] = ' '
            i = j
        elif c == '/' and src[i + 1:i + 2] == '*':
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n': out[k] = ' '
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and src[j] != c and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            for k in range(i + 1, min(j, n)):
                if out[k] != '\n': out[k] = ' '
            i = j + 1
        else:
            i += 1
        line_start = False
    return ''.join(out)


def top_blocks(s):
    """(start, end) of each top-level '{...}' block."""
    res, depth, start = [], 0, 0
    for i, c in enumerate(s):
        if c == '{':
            if depth == 0: start = i
            depth += 1
        elif c == '}' and depth:
            depth -= 1
            if depth == 0: res.append((start, i + 1))
    return res


def param_list_start(s, body):
    """Index of the '(' of the parameter list before a function body, or None."""
    k = body - 1
    while k >= 0 and (s[k].isalnum() or s[k] in '_ \t\r\n'): k -= 1
    if k < 0 or s[k] != ')': return None
    depth = 0
    while k >= 0:
        if s[k] == ')': depth += 1
        elif s[k] == '(':
            depth -= 1
            if depth == 0: return k
        k -= 1
    return None


DECL = (r'\s*((?:const\s+|volatile\s+|struct\s+|unsigned\s+|signed\s+)*[A-Za-z_][\w:<>]*'
        r'(?:\s+(?:int|double|__int8|__int16|__int32|__int64|char|short|long))?(?:\s*[*&]+\s*|\s+)'
        r'(?:__restrict\s+)?)%s\s*(\[[^\]]*\](?:\s*\[[^\]]*\])*)?\s*(?=[;,)=])')


def decl_of(s, lo, pos, name):
    """The last declaration of name in s[lo:pos] (lo: the parameter list's '(' or the
    body's '{'): (type, array suffix or None). In the body a declaration starts a
    statement, so '(a * b)' is not taken for one."""
    body = s.find('{', lo, pos)
    last = None
    if s[lo] == '(':
        end = body if body >= 0 else pos
        for last in decl_pattern(name, True).finditer(s, lo, end): pass
        lo = end
    for last in decl_pattern(name, False).finditer(s, lo, pos): pass
    return (last.group(1).strip(), last.group(2)) if last else None


@functools.lru_cache(maxsize=None)
def decl_pattern(name, params):
    return re.compile((r'(?<=[(,])' if params else r'(?:(?<=[;{}])|^)') + DECL % re.escape(name), re.M)


def call_context(s, pos, lo):
    """(callee, argument index) of the innermost call around s[pos], or None."""
    depth, commas, j = 0, 0, pos - 1
    while j > lo:
        c = s[j]
        if c in ')]':
            depth += 1
        elif c in '([':
            if depth == 0:
                if c == '[': return None
                k = j - 1
                while k > lo and s[k] in ' \t\r\n': k -= 1
                e = k + 1
                while k > lo and (s[k].isalnum() or s[k] in '_:'): k -= 1
                name = s[k + 1:e]
                if not name or name in ('if', 'while', 'for', 'switch', 'return', 'sizeof'):
                    return None
                return name.split('::')[-1], commas
            depth -= 1
        elif c == ',' and depth == 0:
            commas += 1
        elif c in ';{}' and depth == 0:
            return None
        j -= 1
    return None


def size_of(t):
    t = re.sub(r'\b(const|volatile|signed|struct)\b', '', t)
    return SIZES.get(re.sub(r'\s+', ' ', t).strip())


class Source:
    def __init__(self, root):
        self.root = root
        self.files = {}                         # path -> (raw lines, stripped text, functions)
        self.defs = collections.defaultdict(list)  # name -> [(params, body)]
        self.members = collections.defaultdict(set)
        for dp, dns, fns in os.walk(root):
            dns[:] = sorted(d for d in dns if d not in SKIP_DIRS)
            for fn in sorted(fns):
                if fn.endswith(EXTS):
                    self.load(os.path.join(dp, fn))
        self.memo = {}

    def load(self, path):
        raw = open(path, 'rb').read().decode('latin-1')
        s = strip(raw)
        funcs = []
        for fs, fe in top_blocks(s):
            hs = param_list_start(s, fs)
            if hs is None or STRUCT_HEAD.search(s, max(0, fs - 200), fs):
                self.add_members(s[fs + 1:fe - 1])
                continue
            k = hs - 1
            while k >= 0 and s[k] in ' \t\r\n': k -= 1
            e = k + 1
            while k >= 0 and (s[k].isalnum() or s[k] in '_:'): k -= 1
            name = s[k + 1:e].split('::')[-1]
            close = s.rindex(')', hs, fs)
            params, depth, cur = [], 0, ''
            for c in s[hs + 1:close]:
                depth += c in '([<'
                depth -= c in ')]>'
                if c == ',' and depth == 0:
                    params.append(cur); cur = ''
                else:
                    cur += c
            params.append(cur)
            plist = []
            for p in params:
                p = re.sub(r'=.*$', '', p.strip()).strip()
                m = re.search(r'\(\s*\*\s*(\w+)\s*\)\s*(\[|\()', p)
                if m:   # pointer to array (a matrix), or a function pointer
                    plist.append((m.group(1) if m.group(2) == '[' else '', p, True)); continue
                m = re.search(r'(\w+)\s*(\[[^\]]*\])*\s*$', p)
                plist.append((m.group(1) if m else '', p, False))
            funcs.append((hs, fs, fe, name))
            if name:
                self.defs[name].append((plist, s[fs:fe]))
        self.files[path] = (raw.split('\n'), s, funcs)

    def add_members(self, body):
        # members of struct/union/class bodies (namespace bodies' functions are not seen)
        for d in re.finditer(r'(?:^|;|\{|\})\s*((?:const\s+|volatile\s+|unsigned\s+|signed\s+)*'
                             r'[\w:<>]+(?:\s+(?:int|double|__int\d+|char|short|long))?)\s*(\**)\s*'
                             r'(\w+)\s*(\[[^\]]*\])?\s*(?::\s*\d+)?\s*;', body):
            typ, star, name, arr = d.groups()
            self.members[name].add('array' if arr else 'ptr' if star else 'scalar:' + typ)

    def scalar_member(self, name):
        k = self.members.get(name)
        return bool(k) and all(x.startswith('scalar:') and SCALAR.fullmatch(x[7:].strip()) for x in k)

    def param_size(self, callee, arg):
        """Size of the scalar element type of a pointer parameter: 4 when it can't be
        told (vec3r), None for a struct pointer (how much of it is read is unknown)."""
        for plist, body in self.defs.get(callee, ()):
            if arg < len(plist):
                m = re.match(r'(?:const\s+)?([\w ]+?)\s*\*', plist[arg][1])
                if m:
                    return size_of(m.group(1)) if SCALAR.fullmatch(m.group(1).strip()) else None
        return 4

    def extent(self, callee, arg, depth=0):
        """Elements the callee reads through parameter arg: None unknown, INF unbounded."""
        key = (callee, arg)
        if key in self.memo: return self.memo[key]
        if depth > 5 or callee not in self.defs: return None
        self.memo[key] = None
        best = 0
        for plist, body in self.defs[callee]:
            if arg >= len(plist): continue
            p, decl, is2d = plist[arg]
            if not p or not re.search(r'\*|\[|vec3r', decl) or re.search(r'\bvoid\b', decl): continue
            if is2d:
                best = INF; continue
            P = re.escape(p)
            for m in re.finditer(r'(?<![\w.>])' + P + r'\s*\[\s*([^\]]+?)\s*\]', body):
                idx = m.group(1)
                best = max(best, int(idx, 0) + 1 if re.fullmatch(r'\d+|0x[0-9a-fA-F]+', idx) else INF)
            if re.search(r'(?<![\w.>])\*\s*' + P + r'\b', body):
                best = max(best, 1)
            # pointer arithmetic (p++, p += n, p + n; not *p + n), wide reads, mem* on p
            for m in re.finditer(r'(?<![\w.>])' + P + r'\b(?!\s*[\[.(]|\s*->)', body):
                pre, post = body[:m.start()].rstrip(), body[m.end():].lstrip()
                if pre.endswith('&') or pre.endswith('*') and not re.search(r'\(\s*\w[\w\s]*\*\s*$', pre):
                    continue
                if re.match(r'(\+\+|--|\+=|-=|\+(?![+=])|-(?![->=]))', post):
                    best = INF
            if re.search(r'\*\s*\(\s*(?:const\s+)?(?:double|__int64|_QWORD|unsigned __int64|float4|'
                         r'__m128|_OWORD)\s*\*\s*\)\s*' + P + r'\b', body):
                best = max(best, 2)
            if re.search(r'\bmem(?:cpy|set|move|cmp)\s*\(\s*(?:\([^()]*\)\s*)?' + P + r'\b|'
                         r'\bmem(?:cpy|move|cmp)\s*\([^,]+,\s*(?:\([^()]*\)\s*)?' + P + r'\b', body):
                best = INF
            # passed on: f(.., p, ..), f(.., p + c, ..), f(.., &p[c], ..)
            for m in re.finditer(r'([(,]\s*)(?:\(\s*[\w\s]+\*\s*\)\s*)?(?:&\s*' + P + r'\s*\[\s*(\d+)\s*\]|'
                                 + P + r'\s*\+\s*(\d+)|' + P + r')\s*(?=[,)])', body):
                ctx = call_context(body, m.start() + len(m.group(1)), 0)
                if ctx:
                    sub = self.extent(ctx[0], ctx[1], depth + 1)
                    if sub:
                        best = max(best, min(INF, int(m.group(2) or m.group(3) or 0) + sub))
        self.memo[key] = best or None
        return self.memo[key]


def scan(src):
    """Yield (path, line, kind, name, message)."""
    for path, (lines, s, funcs) in src.files.items():
        for hs, fs, fe, fname in funcs:
            yield from scan_addr(src, path, s, hs, fs, fe)
            yield from scan_extent(src, path, s, hs, fs, fe)
            yield from scan_arith(path, s, hs, fs, fe)
            yield from scan_member(src, path, s, fs, fe)
        yield from scan_split(path, lines)


def single(d, pointers=True):
    """A declaration of one scalar, or (with pointers) one pointer."""
    return d and not d[1] and (SCALAR.fullmatch(d[0]) or pointers and
                               re.fullmatch(r'(?:const\s+)?[\w:<> ]+?\s*\*', d[0]))


CAST_BEFORE = re.compile(r'\(\s*(?:const\s+)?[\w ]+?\s*\*+\s*\)\s*$')


def where(s, pos):
    return s.count('\n', 0, pos) + 1


def scan_addr(src, path, s, hs, fs, fe):
    for m in re.finditer(r'(?<![&\w\]])&\s*([A-Za-z_]\w*)\b(?!\s*(?:\[|\.|->|\(|::))', s[fs:fe]):
        p = fs + m.start()
        cast = CAST_BEFORE.search(s, fs, p)
        q = (cast.start() if cast else p) - 1
        while q > fs and s[q] in ' \t\r\n': q -= 1
        if s[q] not in '(,=?:!{;' and not s[q - 5:q + 1].endswith('return'):
            continue                            # binary '&'
        d = decl_of(s, hs, p, m.group(1))
        if not single(d): continue
        if cast and re.match(r'\(\s*(?:const\s+)?(?:unsigned __int8|unsigned char|char|_BYTE|void)\s*\*\s*\)',
                             cast.group(0)):
            continue                            # a byte view, sized by the call
        ctx = call_context(s, p, fs)
        if not ctx: continue
        ext = src.extent(*ctx)
        char = size_of(d[0]) == 1
        if ext and ext > 1 or char and ctx[0] in STRFUNCS and ctx[0] not in src.defs:
            line = where(s, p)
            yield (path, line, 'addr', m.group(1), '%s %s -> %s() arg %d reads %s' % (
                d[0], m.group(1), ctx[0], ctx[1], 'a string' if not ext else
                'unbounded' if ext >= INF else '%d' % ext))


def scan_extent(src, path, s, hs, fs, fe):
    arrays = {}
    for m in re.finditer(r'(?:(?<=[;{}(,])|^)\s*(' + SCALAR.pattern + r')\s+(\w+)\s*\[\s*(\d+|0x[0-9A-Fa-f]+)\s*\]'
                         r'\s*(?=[;=,)])', s[hs:fe], re.M):
        arrays[m.group(2)] = (m.group(1), int(m.group(3), 0))
    if not arrays: return
    names = '|'.join(re.escape(a) for a in arrays)
    pat = re.compile(r'([(,]\s*)(?:\(\s*[\w\s]+\*\s*\)\s*)?(?:&\s*(' + names + r')\s*\[\s*(\d+|0x[0-9A-Fa-f]+)\s*\]|('
                     + names + r')\s*\+\s*(\d+)|(' + names + r'))\s*(?=[,)])')
    for m in pat.finditer(s, fs, fe):
        name = m.group(2) or m.group(4) or m.group(6)
        off = int(m.group(3) or m.group(5) or '0', 0)
        ctx = call_context(s, m.start() + len(m.group(1)), fs)
        if not ctx: continue
        ext, psize = src.extent(*ctx), src.param_size(*ctx)
        typ, n = arrays[name]
        if ext and ext < INF and psize and off * (size_of(typ) or 4) + ext * psize > n * (size_of(typ) or 4):
            yield (path, where(s, m.start()), 'extent', name, '%s %s[%d] + %d -> %s() arg %d reads %d' % (
                typ, name, n, off, ctx[0], ctx[1], ext))
    # (T *)arr or (T *)&arr[k] with T wider than what is left of the array
    cast = re.compile(r'\(\s*(?:const\s+)?([\w ]+?)\s*\*\s*\)\s*(?:&\s*(' + names + r')\s*\[\s*(\d+)\s*\]|('
                      + names + r')\b(?!\s*\[))')
    for m in cast.finditer(s, fs, fe):
        name = m.group(2) or m.group(4)
        typ, n = arrays[name]
        st, se = size_of(m.group(1)), size_of(typ) or 4
        if m.group(1).strip() == 'phys_vec3':
            st = 12     # the physics code reads and writes x, y, z (phys_dot, Phys_*Vec*, *multiply)
        if st and int(m.group(3) or 0) * se + st > n * se and not re.match(r'\s*\+', s[m.end():m.end() + 4]):
            yield (path, where(s, m.start()), 'extent', name, '(%s *) of %s %s[%d] at %s: %d bytes past it' % (
                m.group(1), typ, name, n, m.group(3) or 0, int(m.group(3) or 0) * se + st - n * se))


def scan_arith(path, s, hs, fs, fe):
    body = s[fs:fe]
    # casts of a scalar's address: wider type, or an offset past its size
    for m in re.finditer(r'\(\s*((?:const\s+)?(?:volatile\s+)?[\w ]+?)\s*\*\s*\)\s*&\s*(\w+)\b(?!\s*(?:\[|\.|->|\())', body):
        p = fs + m.start()
        d = decl_of(s, hs, p, m.group(2))
        if not d or d[1] or not SCALAR.fullmatch(d[0]): continue
        st, sd = size_of(m.group(1)), size_of(d[0])
        if not st or not sd: continue
        after = body[m.end():m.end() + 16]
        derefd = body[:m.start()].rstrip().endswith('*')    # *(T *)&x + n adds to the value
        off = None if derefd else re.match(r'\s*\+\s*(\d+)\b(?!\.)', after)
        off = off or re.match(r'\s*\)\s*\[\s*(\d+)\s*\]', after)
        if re.match(r'\s*\+\s*\d+\s*;', after):
            continue                            # p = (char *)&x + 3; the walk is checked below
        if st > sd or off and (int(off.group(1)) + 1) * st > sd:
            yield (path, where(s, p), 'arith', m.group(2), '(%s *)&%s, %s is %d bytes' % (
                m.group(1), m.group(2), d[0], sd))
    # (&x)[k], *(&x + k), &x + k
    for m in re.finditer(r'\(\s*&\s*(\w+)\s*\)\s*\[|(?<![&\w)\]])&\s*(\w+)\s*[+-]\s*[\w(]', body):
        p = fs + m.start()
        name = m.group(1) or m.group(2)
        d = decl_of(s, hs, p, name)
        if single(d):
            yield (path, where(s, p), 'arith', name, 'pointer arithmetic on &%s (%s)' % (name, d[0]))
    # p = <..&x..>; then a walk over p: *++p, p++, (char *)p + 1 (the inlined strcat)
    for m in re.finditer(r'(?<![\w.>])(\w+)\s*=\s*([^;=]*?&\s*(\w+)\b(?!\s*(?:\[|\.|->|\())[^;]*);', body):
        walker, name = m.group(1), m.group(3)
        after = body[m.end():m.end() + 240]
        if not re.search(r'\*\+\+\s*' + walker + r'\b|\b' + walker + r'\s*\+\+|\)\s*' + walker + r'\s*\+\s*1\b', after):
            continue
        p = fs + m.start()
        d = decl_of(s, hs, p, name)
        if single(d):
            yield (path, where(s, p), 'strcat', name, '%s = %s, then walked: an inlined strcat starting at a %s' % (
                walker, m.group(2).strip()[:40], d[0]))


def scan_member(src, path, s, fs, fe):
    for m in re.finditer(r'&\s*([\w\]\[]+(?:\s*(?:\.|->)\s*[\w\]\[]+)*\s*(?:\.|->)\s*(\w+))\b'
                         r'(?!\s*(?:\[|\.|->|\())', s[fs:fe]):
        name = m.group(2)
        if m.group(1).count('[') != m.group(1).count(']') or not src.scalar_member(name): continue
        p = fs + m.start()
        cast = re.search(r'\(\s*(?:const\s+)?([\w ]+?)\s*\*\s*\)\s*$', s[max(fs, p - 40):p])
        ctx = call_context(s, p, fs)
        ext = src.extent(*ctx) if ctx else None
        if ext and 1 < ext < INF:
            yield (path, where(s, p), 'member', name, '&..%s -> %s() arg %d reads %d' % (name, ctx[0], ctx[1], ext))
        elif cast and re.search(r'\b(phys_vec3|float4|hybrid_vector|vec\w*|__m128\w*|double|__int64|_QWORD|_OWORD)\b', cast.group(1)):
            yield (path, where(s, p), 'member', name, '(%s *)&..%s' % (cast.group(1).strip(), name))


def scan_split(path, lines):
    off = lambda l: re.search(r'\[ebp-([0-9A-F]+)h\]', l)
    for i in range(1, len(lines)):
        a, b = lines[i - 1], lines[i]
        if re.match(r'\s*(char|_BYTE|unsigned __int8)\s+\w+;', a) and re.match(r'\s*_BYTE\s+\w+\[\d+\];', b) \
                and off(a) and off(b) and int(off(a).group(1), 16) - int(off(b).group(1), 16) == 1:
            name = re.match(r'\s*[\w ]+?\s+(\w+);', a).group(1)
            yield (path, i, 'split', name, 'one buffer: %s / %s' % (a.strip()[:40], b.strip()[:40]))


def main():
    argv = sys.argv[1:]
    show_all = '--all' in argv
    kinds = None
    if '--kind' in argv:
        i = argv.index('--kind'); kinds = set(argv[i + 1].split(',')); del argv[i:i + 2]
    argv = [a for a in argv if a != '--all']
    root = os.path.realpath(argv[0] if argv else os.path.join(os.path.dirname(__file__), '..', 'src'))
    src = Source(root)
    seen, shown, reviewed = set(), 0, 0
    for path, line, kind, name, msg in scan(src):
        rel = os.path.relpath(path, root)
        if (rel, line, kind, name) in seen or kinds and kind not in kinds: continue
        seen.add((rel, line, kind, name))
        why = REVIEWED.get((rel, kind, name)) or REVIEWED.get((rel, kind, '*'))
        if why:
            reviewed += 1
            if not show_all: continue
            msg += '  [reviewed: %s]' % why
        shown += 1
        text = src.files[path][0][line - 1].strip()[:100]
        print('%s:%d: %s: %s | %s' % (rel, line, kind, msg, text))
    print('%d sites%s' % (shown, '' if show_all else ', %d reviewed ones hidden (--all)' % reviewed),
          file=sys.stderr)
    if show_all:
        hit = {(r, k, nm) for r, _, k, nm in seen} | {(r, k, '*') for r, _, k, _ in seen}
        for key in sorted(set(REVIEWED) - hit):
            print('REVIEWED entry matches nothing now: %s' % (key,), file=sys.stderr)


main()
