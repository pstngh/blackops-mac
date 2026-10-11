#!/usr/bin/env python3
"""Generate src/database/db_save.cpp, the fastfile writer's Save_* functions,
from src/database/db_load.cpp's Load_* functions. Rerun it whenever db_load.cpp
changes:

    python3 tools/gen_db_save.py            # writes src/database/db_save.cpp
    python3 tools/gen_db_save.py --check    # fails if the file is out of date

Each Save_X walks the loaded (source) objects in exactly the order Load_X reads
the stream, so the bytes it emits are the bytes the loader will read. The
writer runtime (db_save_stream.cpp) keeps the loader's stream positions per
block and records where each source range went; the generated code then patches
the pointer fields of the copies:

  Load_Stream(atStreamStart, p, n)      Save_Stream: copy n source bytes at p
  DB_Push/PopStreamPos, AllocLoad_X     the same positions and alignments
  if (p) { p = AllocLoad(); ...}        the field becomes -1, data inline
  if (p == -1) {...} else ToPointer     an alias to data already written, else -1
  asset pointers (DB_InsertPointer)     -2 and a slot the first time, an alias
                                        to the slot after (DB_ConvertOffsetToAlias)
  Load_ScriptStringCustom               the string's index in the new zone's list

The loader's side effects (Load_*Asset, textures, shaders, vertex buffers) have
no counterpart; a few functions are hand-written in db_save_stream.cpp
(Save_XString, the XAssetList header) and listed in HAND_WRITTEN below.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src/database/db_load.cpp"
OUT = ROOT / "src/database/db_save.cpp"

# Load_ functions with no generated Save_ counterpart: hand-written in
# db_save_stream.cpp, or only used for the zone's own header.
HAND_WRITTEN = {
    "Load_XString",          # inline string bytes or an alias
    "Load_TempString",       # the script string list: written by the header code
    "Load_TempStringArray",
    "Load_ScriptStringList",
}

# Calls of the loader's side effects and what the writer does instead
# (None: nothing).
HOOKS = {
    "Load_ScriptStringCustom": "Save_ScriptStringCustom",
    "Load_FxEffectDefFromName": "DB_SaveFxEffectDefFromName",
    "Load_BuildVertexDecl": None,
    "Load_CreateMaterialVertexShader": None,
    "Load_CreateMaterialPixelShader": None,
    "Load_PicmipWater": None,
    "Load_VertexBuffer": None,
    "Load_Texture": None,
}

ASSET_HOOK_TYPES = {
    "Load_PhysPresetAsset": "ASSET_TYPE_PHYSPRESET",
    "Load_PhysConstraintsAsset": "ASSET_TYPE_PHYSCONSTRAINTS",
    "Load_DestructibleDefAsset": "ASSET_TYPE_DESTRUCTIBLEDEF",
    "Load_XAnimPartsAsset": "ASSET_TYPE_XANIMPARTS",
    "Load_XModelAsset": "ASSET_TYPE_XMODEL",
    "Load_MaterialAsset": "ASSET_TYPE_MATERIAL",
    "Load_MaterialTechniqueSetAsset": "ASSET_TYPE_TECHNIQUE_SET",
    "Load_GfxImageAsset": "ASSET_TYPE_IMAGE",
    "Load_SndBankAsset": "ASSET_TYPE_SOUND",
    "Load_SndPatchAsset": "ASSET_TYPE_SOUND_PATCH",
    "Load_ClipMapAsset": "ASSET_TYPE_CLIPMAP_PVS",
    "Load_ComWorldAsset": "ASSET_TYPE_COMWORLD",
    "Load_GameWorldSpAsset": "ASSET_TYPE_GAMEWORLD_SP",
    "Load_GameWorldMpAsset": "ASSET_TYPE_GAMEWORLD_MP",
    "Load_MapEntsAsset": "ASSET_TYPE_MAP_ENTS",
    "Load_GfxWorldAsset": "ASSET_TYPE_GFXWORLD",
    "Load_LightDefAsset": "ASSET_TYPE_LIGHT_DEF",
    "Load_FontAsset": "ASSET_TYPE_FONT",
    "Load_MenuListAsset": "ASSET_TYPE_MENULIST",
    "Load_MenuAsset": "ASSET_TYPE_MENU",
    "Load_LocalizeEntryAsset": "ASSET_TYPE_LOCALIZE_ENTRY",
    "Load_WeaponVariantDefAsset": "ASSET_TYPE_WEAPON",
    "Load_SndDriverGlobalsAsset": "ASSET_TYPE_SNDDRIVER_GLOBALS",
    "Load_FxEffectDefAsset": "ASSET_TYPE_FX",
    "Load_FxImpactTableAsset": "ASSET_TYPE_IMPACT_FX",
    "Load_RawFileAsset": "ASSET_TYPE_RAWFILE",
    "Load_StringTableAsset": "ASSET_TYPE_STRINGTABLE",
    "Load_PackIndexAsset": "ASSET_TYPE_PACK_INDEX",
    "Load_XGlobalsAsset": "ASSET_TYPE_XGLOBALS",
    "Load_DDLAsset": "ASSET_TYPE_DDL",
    "Load_GlassesAsset": "ASSET_TYPE_GLASSES",
    "Load_EmblemSetAsset": "ASSET_TYPE_EMBLEMSET",
}


class GenError(Exception):
    pass


def ws(s):
    return re.sub(r"\s+", " ", s).strip()


# ----- splitting the file into functions -----

FUNC_RE = re.compile(
    r"^(?P<ret>[A-Za-z_][^\n;{}()]*?)\b(?P<name>(?:Load|AllocLoad|Mark)_\w+)\s*\((?P<args>[^)]*)\)\s*\n\{",
    re.M)


def matching(text, pos, open_ch, close_ch):
    """Index just past the bracket matching text[pos] (== open_ch)."""
    depth = 0
    i = pos
    while i < len(text):
        c = text[i]
        if c == "/" and text.startswith("//", i):
            i = text.index("\n", i)
            continue
        if c == '"':
            i += 1
            while text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif c == "'":
            i += 1
            while text[i] != "'":
                i += 2 if text[i] == "\\" else 1
        elif c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise GenError(f"no match for {open_ch} at {pos}")


def split_functions(src):
    funcs = []
    for m in FUNC_RE.finditer(src):
        body_start = m.end() - 1
        body_end = matching(src, body_start, "{", "}")
        funcs.append({
            "ret": ws(m.group("ret")),
            "name": m.group("name"),
            "args": ws(m.group("args")),
            "body": src[body_start + 1:body_end - 1],
            "line": src.count("\n", 0, m.start()) + 1,
        })
    return funcs


# ----- a parser for the decompiled C subset in the loader -----

class Stmt:
    def __init__(self, text):
        self.text = text


class Comment:
    def __init__(self, text):
        self.text = text


class Block:
    def __init__(self, items):
        self.items = items


class If:
    def __init__(self, cond, then, els=None):
        self.cond, self.then, self.els = cond, then, els


class For:
    def __init__(self, head, body):
        self.head, self.body = head, body


class Switch:
    def __init__(self, expr, items):
        self.expr, self.items = expr, items  # items: Label or statements


class Label:
    def __init__(self, text):
        self.text = text


class Parser:
    def __init__(self, text):
        self.t = text
        self.i = 0

    def skip(self):
        while self.i < len(self.t) and self.t[self.i].isspace():
            self.i += 1

    def peek_word(self):
        self.skip()
        m = re.match(r"[A-Za-z_]\w*", self.t[self.i:])
        return m.group(0) if m else None

    def at(self, s):
        self.skip()
        return self.t.startswith(s, self.i)

    def expect(self, s):
        if not self.at(s):
            raise GenError(f"expected {s!r} at {self.t[self.i:self.i + 60]!r}")
        self.i += len(s)

    def paren(self):
        self.skip()
        if self.t[self.i] != "(":
            raise GenError(f"expected ( at {self.t[self.i:self.i + 60]!r}")
        end = matching(self.t, self.i, "(", ")")
        inner = self.t[self.i + 1:end - 1]
        self.i = end
        return ws(inner)

    def block_items(self):
        items = []
        while True:
            self.skip()
            if self.i >= len(self.t) or self.at("}"):
                return items
            items.append(self.stmt())

    def stmt(self):
        self.skip()
        if self.at("//"):
            end = self.t.index("\n", self.i)
            text = self.t[self.i:end]
            self.i = end
            return Comment(text.strip())
        if self.at("{"):
            self.i += 1
            items = self.block_items()
            self.expect("}")
            return Block(items)
        w = self.peek_word()
        if w == "if":
            self.i += 2
            cond = self.paren()
            then = self.stmt()
            els = None
            if self.peek_word() == "else":
                self.i += 4
                els = self.stmt()
            return If(cond, then, els)
        if w == "for":
            self.i += 3
            head = self.paren()
            return For(head, self.stmt())
        if w == "switch":
            self.i += 6
            expr = self.paren()
            self.expect("{")
            items = []
            while True:
                self.skip()
                if self.at("}"):
                    self.i += 1
                    break
                w2 = self.peek_word()
                if w2 in ("case", "default"):
                    end = self.t.index(":", self.i)
                    items.append(Label(ws(self.t[self.i:end + 1])))
                    self.i = end + 1
                else:
                    items.append(self.stmt())
            return Switch(expr, items)
        # a simple statement up to ';' outside parentheses
        depth = 0
        j = self.i
        while True:
            c = self.t[j]
            if c in "([":
                depth += 1
            elif c in ")]":
                depth -= 1
            elif c == '"':
                j += 1
                while self.t[j] != '"':
                    j += 2 if self.t[j] == "\\" else 1
            elif c == ";" and depth == 0:
                break
            j += 1
        text = ws(self.t[self.i:j + 1])
        self.i = j + 1
        return Stmt(text)


def parse_body(body):
    p = Parser(body)
    items = p.block_items()
    p.skip()
    if p.i != len(p.t):
        raise GenError("trailing text in body")
    return items


# ----- printing -----

def emit(items, indent, out):
    pad = "    " * indent
    for it in items:
        emit_one(it, indent, out)


def emit_one(it, indent, out):
    pad = "    " * indent
    if isinstance(it, Stmt):
        out.append(pad + it.text)
    elif isinstance(it, Comment):
        out.append(pad + it.text)
    elif isinstance(it, Block):
        out.append(pad + "{")
        emit(it.items, indent + 1, out)
        out.append(pad + "}")
    elif isinstance(it, If):
        out.append(f"{pad}if ( {it.cond} )")
        emit_sub(it.then, indent, out)
        if it.els is not None:
            if isinstance(it.els, If):
                sub = []
                emit_one(it.els, indent, sub)
                out.append(pad + "else " + sub[0].lstrip())
                out.extend(sub[1:])
            else:
                out.append(pad + "else")
                emit_sub(it.els, indent, out)
    elif isinstance(it, For):
        out.append(f"{pad}for ( {it.head} )")
        emit_sub(it.body, indent, out)
    elif isinstance(it, Switch):
        out.append(f"{pad}switch ( {it.expr} )")
        out.append(pad + "{")
        for x in it.items:
            if isinstance(x, Label):
                out.append(pad + "    " + x.text)
            else:
                emit_one(x, indent + 2, out)
        out.append(pad + "}")
    else:
        raise GenError(f"cannot emit {it}")


def emit_sub(it, indent, out):
    if isinstance(it, Block):
        emit_one(it, indent, out)
    else:
        emit_one(it, indent + 1, out)


# ----- the transformation -----

ALLOC_RE = re.compile(r"^(?P<lhs>[^=]+?)\s*=\s*(?P<cast>\(.*\))?\s*(?P<alloc>AllocLoad_\w+)\(\);$")
CONVPTR_RE = re.compile(r"^DB_ConvertOffsetToPointer\(\s*\((?:unsigned int|uint32_t)\s*\*\s*\)\s*(?P<addr>.+)\);$")
CALL_RE = re.compile(r"^(?P<name>\w+)\((?P<args>.*)\);$")
ASSERT_WORDS = ("Assert_MyHandler",)


class Ctx:
    def __init__(self, funcs, sizes):
        self.funcs = funcs        # names of Load_ functions defined in db_load.cpp
        self.sizes = sizes        # Load_X -> (size expression, has count)


def is_assert_if(it):
    return (isinstance(it, If) and any(w in it.cond for w in ASSERT_WORDS)
            and it.els is None)


def strip_asserts(items):
    out = []
    for it in items:
        if is_assert_if(it):
            continue
        out.append(it)
    return out


def call_size(ctx, stmt_text):
    """Byte size the first Load call of an inline block reads at its start."""
    m = CALL_RE.match(stmt_text)
    if not m or m.group("name") not in ctx.sizes:
        return None
    expr, has_count = ctx.sizes[m.group("name")]
    args = [a.strip() for a in split_args(m.group("args"))]
    if has_count:
        if len(args) != 2:
            return None
        return re.sub(r"\bcount\b", f"({args[1]})", expr)
    return expr


def split_args(s):
    args, depth, cur = [], 0, ""
    for c in s:
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        if c == "," and depth == 0:
            args.append(cur)
            cur = ""
        else:
            cur += c
    if cur.strip():
        args.append(cur)
    return args


def xform_stmt(ctx, text):
    """Rename one simple statement; returns a list of Stmt (maybe empty)."""
    if re.match(r"^\w+\s*=\s*.*AllocLoad_", text) or "AllocLoad_" in text:
        raise GenError(f"unhandled AllocLoad: {text}")
    if "DB_ConvertOffsetTo" in text or "DB_InsertPointer" in text:
        raise GenError(f"unhandled conversion: {text}")
    if "DB_GetStreamPos" in text:
        raise GenError(f"unhandled stream position use: {text}")
    m = CALL_RE.match(text)
    if m:
        name = m.group("name")
        if name in HOOKS:
            new = HOOKS[name]
            return [] if new is None else [Stmt(f"{new}({m.group('args')});")]
        if name in ASSET_HOOK_TYPES:
            raise GenError(f"asset hook outside an asset pointer: {text}")
        if name == "Load_Stream":
            return [Stmt(f"Save_Stream({m.group('args')});")]
        if name == "DB_PushStreamPos":
            return [Stmt(f"DB_SavePushStreamPos({m.group('args')});")]
        if name == "DB_PopStreamPos":
            return [Stmt("DB_SavePopStreamPos();")]
        if name.startswith("Load_"):
            if name not in ctx.funcs and name not in HAND_WRITTEN:
                raise GenError(f"unknown loader call: {text}")
            return [Stmt("Save_" + name[5:] + f"({m.group('args')});")]
        if name in ("static_assert",):
            return [Stmt(text)]
    if text.startswith("Load_") or re.search(r"\bLoad_\w+\(", text):
        raise GenError(f"unhandled loader call: {text}")
    return [Stmt(text)]


def xform_items(ctx, items):
    out = []
    for it in strip_asserts(items):
        out.extend(xform(ctx, it))
    return out


def block_items(it):
    return it.items if isinstance(it, Block) else [it]


def real_items(items):
    return [x for x in items if not isinstance(x, Comment)]


def xform_inline_body(ctx, items, field_addr, shared):
    """The statements after `field = AllocLoad_X();`."""
    first = items[0]
    m = ALLOC_RE.match(first.text)
    lhs = m.group("lhs").strip()
    alloc = m.group("alloc")
    rest = items[1:]
    body = xform_items(ctx, rest)
    loads = [x for x in real_items(rest) if isinstance(x, Stmt) and CALL_RE.match(x.text)
             and CALL_RE.match(x.text).group("name").startswith("Load_")
             and CALL_RE.match(x.text).group("name") not in HOOKS]
    alloc_stmt = Stmt(f"AllocSave_{alloc[len('AllocLoad_'):]}();")
    if shared:
        size = call_size(ctx, loads[0].text) if loads else None
        if size is None:
            if not loads or CALL_RE.match(loads[0].text).group("name") != "Load_XString":
                raise GenError(f"no size for shared pointer {lhs}")
            size = "0"
        return [If(f"!DB_SaveAlias({field_addr}, {lhs}, {size})",
                   Block([alloc_stmt] + body))]
    return [Stmt(f"DB_SaveSetPointer(&{paren_if_needed(lhs)}, 0xFFFFFFFF);"), alloc_stmt] + body


def paren_if_needed(e):
    return e if re.match(r"^[\w\->.\[\]]+$", e) or e.startswith("*") and re.match(r"^\*\w+$", e) else f"({e})"


def xform(ctx, it):
    if isinstance(it, Comment):
        return [it]
    if isinstance(it, Stmt):
        return xform_stmt(ctx, it.text)
    if isinstance(it, Block):
        return [Block(xform_items(ctx, it.items))]
    if isinstance(it, For):
        return [For(it.head, xform(ctx, it.body)[0] if not isinstance(it.body, Block)
                    else Block(xform_items(ctx, it.body.items)))]
    if isinstance(it, Switch):
        items = []
        for x in it.items:
            if isinstance(x, Label):
                items.append(x)
            else:
                items.extend(xform(ctx, x))
        return [Switch(it.expr, items)]
    if isinstance(it, If):
        then_items = real_items(block_items(it.then))
        # shared pointer: if (p) { if (p == -1) { p = AllocLoad(); ... } else ToPointer(&p); }
        if (it.els is None and len(then_items) == 1 and isinstance(then_items[0], If)
                and re.match(r"^.*==\s*(\(.*\))?\s*-1$", then_items[0].cond)):
            inner = then_items[0]
            ins = real_items(block_items(inner.then))
            els = real_items(block_items(inner.els)) if inner.els is not None else []
            if (ins and isinstance(ins[0], Stmt) and ALLOC_RE.match(ins[0].text)
                    and len(els) == 1 and isinstance(els[0], Stmt) and CONVPTR_RE.match(els[0].text)):
                addr = CONVPTR_RE.match(els[0].text).group("addr").strip()
                return [If(it.cond, Block(xform_inline_body(ctx, block_items(inner.then), addr, True)))]
            raise GenError(f"unrecognised -1 pointer under {it.cond}")
        # inline pointer: if (p) { p = AllocLoad(); ... }
        if (it.els is None and then_items and isinstance(then_items[0], Stmt)
                and ALLOC_RE.match(then_items[0].text)):
            return [If(it.cond, Block(xform_inline_body(ctx, block_items(it.then), None, False)))]
        then = Block(xform_items(ctx, it.then.items)) if isinstance(it.then, Block) else single(ctx, it.then)
        els = None
        if it.els is not None:
            if isinstance(it.els, If):
                els = xform(ctx, it.els)[0]
            elif isinstance(it.els, Block):
                els = Block(xform_items(ctx, it.els.items))
            else:
                els = single(ctx, it.els)
        return [If(it.cond, then, els)]
    raise GenError(f"cannot transform {it}")


def single(ctx, it):
    r = xform(ctx, it)
    if len(r) == 1:
        return r[0]
    return Block(r)


# ----- asset pointers -----

def xform_asset_ptr(ctx, f, items):
    stmts = flatten(items)
    texts = [s.text for s in stmts if isinstance(s, Stmt) and not is_decl(s.text)]
    m = re.match(r"^Load_Stream\(atStreamStart, \(unsigned __int8 \*\)(?P<ptr>\w+), 4\);$", texts[0])
    if not m:
        raise GenError(f"{f['name']}: unexpected first statement {texts[0]}")
    ptr = m.group("ptr")
    var_assign = None
    load_call = None
    hook = None
    alloc = None
    for i, t in enumerate(texts):
        am = ALLOC_RE.match(t)
        if am:
            alloc = am.group("alloc")
            var_assign = texts[i + 1]
        cm = CALL_RE.match(t)
        if cm and cm.group("name").startswith("Load_") and cm.group("args") == "1":
            load_call = cm.group("name")
            hook = CALL_RE.match(texts[i + 1]).group("name")
    if not (var_assign and load_call and hook and alloc):
        raise GenError(f"{f['name']}: unrecognised asset pointer")
    save_call = "Save_" + load_call[5:]
    alloc_save = "AllocSave_" + alloc[len("AllocLoad_"):]
    lines = [
        f"Save_Stream(atStreamStart, (unsigned __int8 *){ptr}, 4);",
        "DB_SavePushStreamPos(0);",
        f"if ( *{ptr} )",
        "{",
    ]
    if hook in ASSET_HOOK_TYPES:
        # The writer may put another header in this one's place (a stub's real
        # asset): DB_SaveResolveAsset.
        var = re.match(r"^(\w+) = ", var_assign).group(1)
        lines += [
            f"    header = DB_SaveResolveAsset({ASSET_HOOK_TYPES[hook]}, *{ptr});",
            f"    if ( !DB_SaveAssetAlias({ASSET_HOOK_TYPES[hook]}, {ptr}, header) )",
            "    {",
            f"        {alloc_save}();",
            "        inserted = DB_SaveInsertPointer();",
            f"        {var} = (decltype({var}))header;",
            f"        {save_call}(1);",
            "        DB_SaveAssetWritten(header, inserted);",
            "    }",
        ]
        decl = ["const void *header;", "unsigned int inserted;", ""]
    else:
        # Not an asset (the image's texture load definition): inline, no slot.
        lines += [
            f"    DB_SaveSetPointer({ptr}, 0xFFFFFFFF);",
            f"    {alloc_save}();",
            f"    {var_assign}",
            f"    {save_call}(1);",
        ]
        decl = []
    lines += ["}", "DB_SavePopStreamPos();"]
    return decl + lines


DECL_RE = re.compile(r"^[A-Za-z_][\w\s<>:*]*[\s*]\**\w+(?:\[\d+\])?;$|^[\w\s]+\(\*\w+\)\[\d+\];$")


def is_decl(text):
    return (bool(DECL_RE.match(text)) and not text.startswith(("return", "break"))
            and " " in text.strip())


def flatten(items):
    out = []
    for it in items:
        if isinstance(it, Block):
            out.extend(flatten(it.items))
        elif isinstance(it, If):
            out.extend(flatten([it.then]))
            if it.els is not None:
                out.extend(flatten([it.els]))
        elif isinstance(it, (Stmt, Comment)):
            out.append(it)
    return out


# ----- sizes for alias checks -----

def leading_sizes(funcs):
    sizes = {}
    for f in funcs:
        if not f["name"].startswith("Load_"):
            continue
        try:
            items = strip_asserts(parse_body(f["body"]))
        except GenError:
            continue
        items = [x for x in real_items(items) if isinstance(x, Stmt) and not is_decl(x.text)]
        if not items:
            continue
        m = re.match(r"^Load_Stream\((?:atStreamStart|1), (.+), (?P<size>[^,]+)\);$", items[0].text)
        if not m:
            continue
        size = m.group("size").strip()
        has_count = "count" in f["args"]
        sizes[f["name"]] = (size, has_count)
    return sizes


# ----- the file -----

HEADER = """\
// Generated by tools/gen_db_save.py from db_load.cpp. Don't edit: change the
// script or db_load.cpp and rerun it (python3 tools/gen_db_save.py).
//
// The fastfile writer: each Save_X walks loaded objects in the order Load_X
// reads them and emits what Load_X will read back (db_save_stream.cpp has the
// runtime and the hand-written parts).
"""


def var_block(src):
    start = src.index("// This seriously has to be one of the most aids parts of the COD codebases")
    start = src.index("\n", start) + 1
    end = src.index("void __cdecl Load_byteArray")
    lines = [l for l in src[start:end].splitlines() if l.strip()]
    return lines


def includes(src):
    return [l for l in src.splitlines() if l.startswith("#include")]


def generate():
    src = SRC.read_text(encoding="latin-1")
    funcs = split_functions(src)
    names = {f["name"] for f in funcs}
    loads = [f for f in funcs if f["name"].startswith("Load_")]
    allocs = [f for f in funcs if f["name"].startswith("AllocLoad_")]
    ctx = Ctx(names, leading_sizes(funcs))
    out = [HEADER]
    out += includes(src)
    out += ['#include "db_save.h"', "", "namespace dbsave", "{", ""]
    out += var_block(src)
    out.append("")
    # allocations
    for f in allocs:
        m = re.search(r"DB_AllocStreamPos\((\d+)\)", f["body"])
        if not m:
            raise GenError(f"{f['name']}: no alignment")
        out.append(f"static void AllocSave_{f['name'][len('AllocLoad_'):]}()")
        out.append("{")
        out.append(f"    DB_SaveAllocStreamPos({m.group(1)});")
        out.append("}")
        out.append("")
    gen = []
    protos = []
    errors = []
    count = 0
    for f in loads:
        if f["name"] in HAND_WRITTEN:
            continue
        sname = "Save_" + f["name"][5:]
        sig = f"void __cdecl {sname}({f['args']})"
        try:
            items = parse_body(f["body"])
            body_lines = []
            if "DB_InsertPointer" in f["body"]:
                body_lines = xform_asset_ptr(ctx, f, items)
                body_lines = ["    " + l if l else "" for l in body_lines]
            else:
                new = xform_items(ctx, items)
                # Drop the locals the loader needed for -2 handling only.
                emit(new, 1, body_lines)
        except GenError as e:
            errors.append(f"{f['name']} (db_load.cpp:{f['line']}): {e}")
            continue
        protos.append(sig + ";")
        gen.append(f"// db_load.cpp:{f['line']} {f['name']}")
        gen.append(sig)
        gen.append("{")
        gen.extend(body_lines)
        gen.append("}")
        gen.append("")
        count += 1
    if errors:
        raise GenError("\n".join(errors))
    out += protos
    out.append("")
    out += gen
    out.append("} // namespace dbsave")
    out.append("")
    return "\n".join(out), count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    try:
        text, count = generate()
    except GenError as e:
        print(f"gen_db_save.py: {e}", file=sys.stderr)
        return 1
    if a.check:
        if not OUT.exists() or OUT.read_text() != text:
            print("src/database/db_save.cpp is out of date: rerun tools/gen_db_save.py",
                  file=sys.stderr)
            return 1
        print(f"db_save.cpp up to date ({count} Save_ functions)")
        return 0
    OUT.write_text(text)
    print(f"wrote {OUT.relative_to(ROOT)}: {count} Save_ functions "
          f"(+{len(HAND_WRITTEN)} hand-written in db_save_stream.cpp)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
