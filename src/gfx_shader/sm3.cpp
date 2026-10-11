// sm3.cpp — D3D9 shader bytecode parser and disassembler (sm3.h).
#include "sm3.h"

#include <cstdio>
#include <cstring>

namespace sm3 {

namespace {

Dst decodeDst(unsigned t) {
    Dst d;
    d.reg   = (int)(t & 0x7FF);
    d.type  = (int)(((t >> 28) & 0x7) | ((t >> 8) & 0x18));
    d.mask  = (int)((t >> 16) & 0xF);
    d.mod   = (int)((t >> 20) & 0xF);
    d.shift = (int)((t >> 24) & 0xF);
    d.rel   = (t & 0x2000) != 0;
    return d;
}

// A source parameter; with relative addressing (bit 13) the address register's token
// follows it (shader model 2 and up). Returns the tokens used.
int decodeSrc(const unsigned *t, Src &s) {
    s.reg     = (int)(t[0] & 0x7FF);
    s.type    = (int)(((t[0] >> 28) & 0x7) | ((t[0] >> 8) & 0x18));
    s.swizzle = (int)((t[0] >> 16) & 0xFF);
    s.mod     = (int)((t[0] >> 24) & 0xF);
    s.rel     = (t[0] & 0x2000) != 0;
    if (!s.rel) return 1;
    s.relReg  = (int)(t[1] & 0x7FF);
    s.relType = (int)(((t[1] >> 28) & 0x7) | ((t[1] >> 8) & 0x18));
    s.relComp = (int)((t[1] >> 16) & 0x3);
    return 2;
}

bool opHasDst(int op) {
    switch (op) {
    case OP_IF: case OP_IFC: case OP_ELSE: case OP_ENDIF: case OP_REP: case OP_ENDREP: case OP_LOOP:
    case OP_ENDLOOP: case OP_BREAK: case OP_BREAKC: case OP_BREAKP: case OP_CALL: case OP_CALLNZ:
    case OP_LABEL: case OP_RET: case OP_NOP:
        return false;
    default:
        return true;
    }
}

} // namespace

const Decl *Shader::sampler(int reg) const {
    for (const Decl &d : decls)
        if (d.type == RT_SAMPLER && d.reg == reg) return &d;
    return nullptr;
}

size_t TokenCount(const unsigned *t) {
    if (!t) return 0;
    size_t n = 1;
    while (t[n] != 0x0000FFFFu) {
        n += (t[n] & 0xFFFFu) == OP_COMMENT ? ((t[n] >> 16) & 0x7FFF) + 1 : 1;
        if (n > (1u << 20)) return 0;
    }
    return n + 1;
}

// The constant table fxc leaves in a comment (D3DXSHADER_CONSTANTTABLE, offsets from
// its start): the samplers' names, which say what a sampler reads (the shadow maps).
static void readSamplerNames(const unsigned *table, size_t bytes, Shader &out) {
    const unsigned char *base = (const unsigned char *)table;
    auto word = [&](size_t off) { unsigned v; memcpy(&v, base + off, 4); return v; };
    if (bytes < 28) return;
    const unsigned count = word(12), info = word(16);
    for (unsigned i = 0; i < count; ++i) {
        const size_t c = (size_t)info + i * 20;
        if (c + 20 > bytes) return;
        const unsigned name = word(c), regs = word(c + 4);
        if ((regs & 0xFFFF) != 3 || name >= bytes) continue;   // D3DXRS_SAMPLER
        const unsigned char *end = (const unsigned char *)memchr(base + name, 0, bytes - name);
        if (!end) continue;
        out.samplerNames[(int)(regs >> 16)] = std::string((const char *)base + name, (const char *)end);
    }
}

bool Parse(const unsigned *tok, Shader &out, std::string &error) {
    out = Shader();
    const unsigned ver = *tok++;
    if ((ver >> 16) != 0xFFFF && (ver >> 16) != 0xFFFE) { error = "not a shader version token"; return false; }
    out.pixel = (ver >> 16) == 0xFFFF;
    out.major = (int)((ver >> 8) & 0xFF);
    out.minor = (int)(ver & 0xFF);
    if (out.major < 2) { error = "shader model 1 (no instruction lengths)"; return false; }
    for (size_t guard = 0;; ++guard) {
        if (guard > (1u << 20)) { error = "no end token"; return false; }
        const unsigned t = *tok++;
        const int op = (int)(t & 0xFFFF);
        if (op == OP_END) break;
        if (op == OP_COMMENT) {
            const unsigned words = (t >> 16) & 0x7FFF;
            if (words >= 8 && tok[0] == 0x42415443u) readSamplerNames(tok + 1, (words - 1) * 4, out);   // 'CTAB'
            tok += words;
            continue;
        }
        const int len = (int)((t >> 24) & 0xF);
        if (op == OP_DCL) {
            Decl d;
            const unsigned usage = tok[0];
            const Dst r = decodeDst(tok[1]);
            d.type = r.type; d.reg = r.reg; d.mask = r.mask; d.mod = r.mod;
            if (r.type == RT_SAMPLER) d.dim = (int)((usage >> 27) & 0xF);
            else { d.usage = (int)(usage & 0x1F); d.index = (int)((usage >> 16) & 0xF); }
            out.decls.push_back(d);
            tok += len;
            continue;
        }
        if (op == OP_DEF) {
            std::array<float, 4> f;
            memcpy(f.data(), &tok[1], sizeof(f));
            out.defs[decodeDst(tok[0]).reg] = f;
            tok += len;
            continue;
        }
        if (op == OP_DEFI) {
            std::array<int, 4> v;
            memcpy(v.data(), &tok[1], sizeof(v));
            out.idefs[decodeDst(tok[0]).reg] = v;
            tok += len;
            continue;
        }
        if (op == OP_DEFB) {
            out.bdefs[decodeDst(tok[0]).reg] = tok[1] != 0;
            tok += len;
            continue;
        }
        Instr in;
        in.op = op;
        in.ctrl = (int)((t >> 16) & 0xFF);
        in.predicated = (t & 0x10000000) != 0;
        int k = 0;
        if (opHasDst(op) && k < len) {
            in.hasDst = true;
            in.dst = decodeDst(tok[k]);
            k += in.dst.rel ? 2 : 1;
        }
        if (in.predicated && k < len) k += decodeSrc(tok + k, in.pred);
        while (k < len) {
            if (in.nsrc == 4) { error = "more than 4 sources"; return false; }
            k += decodeSrc(tok + k, in.src[in.nsrc++]);
        }
        if (k != len) { error = "operand tokens overrun the instruction"; return false; }
        out.code.push_back(in);
        tok += len;
    }
    return true;
}

const char *OpName(int op) {
    switch (op) {
    case OP_NOP: return "nop";       case OP_MOV: return "mov";       case OP_ADD: return "add";
    case OP_SUB: return "sub";       case OP_MAD: return "mad";       case OP_MUL: return "mul";
    case OP_RCP: return "rcp";       case OP_RSQ: return "rsq";       case OP_DP3: return "dp3";
    case OP_DP4: return "dp4";       case OP_MIN: return "min";       case OP_MAX: return "max";
    case OP_SLT: return "slt";       case OP_SGE: return "sge";       case OP_EXP: return "exp";
    case OP_LOG: return "log";       case OP_LIT: return "lit";       case OP_DST: return "dst";
    case OP_LRP: return "lrp";       case OP_FRC: return "frc";       case OP_M4x4: return "m4x4";
    case OP_M4x3: return "m4x3";     case OP_M3x4: return "m3x4";     case OP_M3x3: return "m3x3";
    case OP_M3x2: return "m3x2";     case OP_CALL: return "call";     case OP_CALLNZ: return "callnz";
    case OP_LOOP: return "loop";     case OP_RET: return "ret";       case OP_ENDLOOP: return "endloop";
    case OP_LABEL: return "label";   case OP_POW: return "pow";       case OP_CRS: return "crs";
    case OP_SGN: return "sgn";       case OP_ABS: return "abs";       case OP_NRM: return "nrm";
    case OP_SINCOS: return "sincos"; case OP_REP: return "rep";       case OP_ENDREP: return "endrep";
    case OP_IF: return "if";         case OP_IFC: return "ifc";       case OP_ELSE: return "else";
    case OP_ENDIF: return "endif";   case OP_BREAK: return "break";   case OP_BREAKC: return "breakc";
    case OP_MOVA: return "mova";     case OP_TEXKILL: return "texkill"; case OP_TEX: return "texld";
    case OP_CMP: return "cmp";       case OP_DP2ADD: return "dp2add"; case OP_DSX: return "dsx";
    case OP_DSY: return "dsy";       case OP_TEXLDD: return "texldd"; case OP_SETP: return "setp";
    case OP_TEXLDL: return "texldl"; case OP_BREAKP: return "breakp";
    default: return "op?";
    }
}

namespace {

std::string regAsm(bool pixel, int type, int reg) {
    const char *t;
    switch (type) {
    case RT_TEMP: t = "r"; break;          case RT_INPUT: t = "v"; break;
    case RT_CONST: t = "c"; break;         case RT_ADDR: t = pixel ? "t" : "a"; break;
    case RT_RASTOUT: t = "oPos"; break;    case RT_ATTROUT: t = "oD"; break;
    case RT_OUTPUT: t = "o"; break;        case RT_CONSTINT: t = "i"; break;
    case RT_COLOROUT: t = "oC"; break;     case RT_DEPTHOUT: t = "oDepth"; break;
    case RT_SAMPLER: t = "s"; break;       case RT_CONSTBOOL: t = "b"; break;
    case RT_LOOP: t = "aL"; break;         case RT_MISCTYPE: return reg ? "vFace" : "vPos";
    case RT_LABEL: t = "l"; break;         case RT_PREDICATE: t = "p"; break;
    default: t = "x?"; break;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%s%d", t, reg);
    return buf;
}

std::string maskAsm(int m) {
    if (m == 0xF) return "";
    std::string s = ".";
    for (int i = 0; i < 4; ++i) if (m & (1 << i)) s += "xyzw"[i];
    return s;
}

std::string srcAsm(bool pixel, const Src &o) {
    std::string s = regAsm(pixel, o.type, o.reg);
    if (o.rel) { s += "[" + regAsm(pixel, o.relType, o.relReg) + "." + "xyzw"[o.relComp & 3] + "]"; }
    if (o.swizzle != 0xE4) {
        s += ".";
        for (int i = 0; i < 4; ++i) s += "xyzw"[(o.swizzle >> (2 * i)) & 3];
    }
    switch (o.mod) {
    case MOD_NEG: return "-" + s;           case MOD_BIAS: return s + "_bias";
    case MOD_BIASNEG: return "-" + s + "_bias"; case MOD_SIGN: return s + "_bx2";
    case MOD_SIGNNEG: return "-" + s + "_bx2";  case MOD_COMP: return "1-" + s;
    case MOD_X2: return s + "_x2";          case MOD_X2NEG: return "-" + s + "_x2";
    case MOD_DZ: return s + "_dz";          case MOD_DW: return s + "_dw";
    case MOD_ABS: return s + "_abs";        case MOD_ABSNEG: return "-" + s + "_abs";
    case MOD_NOT: return "!" + s;           default: return s;
    }
}

} // namespace

std::string Disassemble(const Shader &s) {
    std::string out;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s_%d_%d\n", s.pixel ? "ps" : "vs", s.major, s.minor);
    out += buf;
    for (const Decl &d : s.decls) {
        static const char *usages[] = { "position", "blendweight", "blendindices", "normal", "psize", "texcoord",
                                        "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth",
                                        "sample" };
        static const char *dims[] = { "?", "?", "2d", "cube", "volume" };
        std::string name = d.type == RT_SAMPLER ? std::string("_") + (d.dim >= 2 && d.dim <= 4 ? dims[d.dim] : "?")
                         : (d.type == RT_INPUT || d.type == RT_OUTPUT)
                             ? std::string("_") + (d.usage < 14 ? usages[d.usage] : "?") + std::to_string(d.index) : "";
        out += "dcl" + name + (d.mod & DMOD_CENTROID ? "_centroid" : "") + " " + regAsm(s.pixel, d.type, d.reg) +
               maskAsm(d.mask) + "\n";
    }
    for (const auto &d : s.defs) {
        snprintf(buf, sizeof(buf), "def c%d, %.9g, %.9g, %.9g, %.9g\n", d.first, d.second[0], d.second[1], d.second[2],
                 d.second[3]);
        out += buf;
    }
    for (const auto &d : s.idefs) {
        snprintf(buf, sizeof(buf), "defi i%d, %d, %d, %d, %d\n", d.first, d.second[0], d.second[1], d.second[2],
                 d.second[3]);
        out += buf;
    }
    for (const auto &d : s.bdefs)
        out += "defb b" + std::to_string(d.first) + ", " + (d.second ? "true" : "false") + "\n";
    for (const Instr &in : s.code) {
        std::string line;
        if (in.predicated) line += "(" + srcAsm(s.pixel, in.pred) + ") ";
        line += OpName(in.op);
        if (in.op == OP_TEX && in.ctrl == TEX_PROJECT) line += "p";
        if (in.op == OP_TEX && in.ctrl == TEX_BIAS) line += "b";
        if ((in.op == OP_IFC || in.op == OP_BREAKC || in.op == OP_SETP) && in.ctrl) {
            static const char *cmp[] = { "", "_gt", "_eq", "_ge", "_lt", "_ne", "_le" };
            line += in.ctrl <= 6 ? cmp[in.ctrl] : "_?";
        }
        if (in.hasDst) {
            if (in.dst.mod & DMOD_SAT) line += "_sat";
            if (in.dst.mod & DMOD_PP) line += "_pp";
            line += " " + regAsm(s.pixel, in.dst.type, in.dst.reg) + (in.dst.rel ? "[rel]" : "") + maskAsm(in.dst.mask);
        }
        for (int i = 0; i < in.nsrc; ++i)
            line += (i || in.hasDst ? ", " : " ") + srcAsm(s.pixel, in.src[i]);
        out += line + "\n";
    }
    return out;
}

} // namespace sm3
