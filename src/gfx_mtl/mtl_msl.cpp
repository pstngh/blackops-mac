// mtl_msl.cpp — D3D9 shader bytecode to MSL (mtl_msl.h).
#include "mtl_msl.h"
#include "../gfx_shader/sm3.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <vector>

using namespace sm3;

namespace {

// D3DDECLUSAGE_* values.
enum { U_POSITION = 0, U_BLENDWEIGHT, U_BLENDINDICES, U_NORMAL, U_PSIZE, U_TEXCOORD, U_TANGENT, U_BINORMAL,
       U_TESSFACTOR, U_POSITIONT, U_COLOR, U_FOG, U_DEPTH, U_SAMPLE };

// A varying (vertex output / pixel input) by usage: its member name and slot bit.
bool varying(int usage, int index, std::string &name, int &slot) {
    static const char *names[] = { "position", "blendweight", "blendindices", "normal", "psize", "texcoord",
                                   "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth",
                                   "sample" };
    if (usage < 0 || usage > U_SAMPLE || index < 0 || index > 15) return false;
    switch (usage) {
    case U_TEXCOORD: slot = index; break;                              // 0..15
    case U_COLOR:    if (index > 1) return false; slot = 16 + index; break;
    case U_FOG:      if (index) return false; slot = 18; break;
    case U_POSITION: if (index < 1 || index > 4) return false; slot = 18 + index; break;   // 19..22
    case U_NORMAL:   if (index > 3) return false; slot = 23 + index; break;
    case U_TANGENT:  if (index > 3) return false; slot = 27 + index; break;
    case U_BINORMAL: if (index > 3) return false; slot = 31 + index; break;
    case U_DEPTH:    if (index) return false; slot = 35; break;
    default: return false;
    }
    name = std::string(names[usage]) + std::to_string(index);
    return true;
}

std::string fmtFloat(float f) {
    if (std::isnan(f) || std::isinf(f)) {
        unsigned bits;
        memcpy(&bits, &f, 4);
        char b[48];
        snprintf(b, sizeof(b), "as_type<float>(0x%08xu)", bits);
        return b;
    }
    char b[32];
    snprintf(b, sizeof(b), "%.9g", f);
    if (!strpbrk(b, ".e")) strcat(b, ".0");
    return b;
}

const char *kComp = "xyzw";

std::string maskStr(int m) {
    if (m == 0xF) return "";
    std::string s = ".";
    for (int i = 0; i < 4; ++i) if (m & (1 << i)) s += kComp[i];
    return s;
}

std::string swizStr(int sw) {
    if (sw == 0xE4) return "";
    std::string s = ".";
    for (int i = 0; i < 4; ++i) s += kComp[(sw >> (2 * i)) & 3];
    return s;
}

struct Emitter {
    const Shader &sh;
    MSLShader &out;
    std::string prefix;          // type/entry suffix
    std::ostringstream body;
    std::set<int> temps, outputs, colorOuts;
    bool usesA0 = false, usesPos = false, usesFace = false, failed = false;
    int maxConst = -1;
    bool relConst = false;
    int depth = 1, loopCount = 0;
    std::vector<int> blocks;      // open control flow: 0 if/rep, 1 loop (two braces)
    std::vector<std::string> aL;  // loop counter names, innermost last
    bool perSample = false;
    bool rawShadows = false;      // shadow maps read as raw depth (the GL layer's way)
    unsigned shadow = 0;          // pixel samplers the constant table names shadowmapSampler*
    bool relDefs = false;         // a relative read may land on a def'd constant

    Emitter(const Shader &s, MSLShader &o) : sh(s), out(o) {}

    void fail(const std::string &why) {
        if (!failed) out.error = why;
        failed = true;
    }
    std::string ind() const { return std::string(2 * depth, ' '); }

    std::string reg(const Src &o) {
        char b[64];
        switch (o.type) {
        case RT_TEMP: temps.insert(o.reg); snprintf(b, sizeof(b), "r%d", o.reg); return b;
        case RT_INPUT: snprintf(b, sizeof(b), "v%d", o.reg); return b;
        case RT_CONST:
            if (o.rel) {
                std::string addr;
                if (o.relType == RT_ADDR && !sh.pixel) { usesA0 = true; addr = std::string("a0.") + kComp[o.relComp]; }
                else if (o.relType == RT_LOOP && !aL.empty()) addr = aL.back();
                else { fail("relative addressing through an unknown register"); return "float4(0.0)"; }
                relConst = true;
                maxConst = 255;
                for (const auto &d : sh.defs)
                    if (d.first >= o.reg) relDefs = true;
                if (relDefs)
                    snprintf(b, sizeof(b), "kb_crel%s(c, %s + %d)", prefix.empty() ? "" : ("_" + prefix).c_str(),
                             addr.c_str(), o.reg);
                else
                    snprintf(b, sizeof(b), "c[clamp(%s + %d, 0, 255)]", addr.c_str(), o.reg);
                return b;
            }
            if (sh.defs.count(o.reg)) { snprintf(b, sizeof(b), "c%dd", o.reg); return b; }
            if (o.reg > maxConst) maxConst = o.reg;
            snprintf(b, sizeof(b), "c[%d]", o.reg);
            return b;
        case RT_MISCTYPE:
            if (!sh.pixel) break;
            if (o.reg == 0) { usesPos = true; return "float4(floor(in.kbPos.xy), in.kbPos.zw)"; }
            if (o.reg == 1) { usesFace = true; return "float4(in.kbFace ? 1.0 : -1.0)"; }
            break;
        case RT_LOOP:
            if (aL.empty()) break;
            return "float4(float(" + aL.back() + "))";
        default:
            break;
        }
        snprintf(b, sizeof(b), "source register type %d", o.type);
        fail(b);
        return "float4(0.0)";
    }

    std::string src(const Src &o) {
        const std::string e = reg(o) + swizStr(o.swizzle);
        switch (o.mod) {
        case MOD_NONE:    return e;
        case MOD_NEG:     return "(-" + e + ")";
        case MOD_BIAS:    return "(" + e + " - 0.5)";
        case MOD_BIASNEG: return "(0.5 - " + e + ")";
        case MOD_SIGN:    return "(2.0 * " + e + " - 1.0)";
        case MOD_SIGNNEG: return "(1.0 - 2.0 * " + e + ")";
        case MOD_COMP:    return "(1.0 - " + e + ")";
        case MOD_X2:      return "(2.0 * " + e + ")";
        case MOD_X2NEG:   return "(-2.0 * " + e + ")";
        case MOD_ABS:     return "abs(" + e + ")";
        case MOD_ABSNEG:  return "(-abs(" + e + "))";
        default:          fail("source modifier " + std::to_string(o.mod)); return e;
        }
    }

    // The destination register and whether it is a scalar (oDepth).
    std::string dst(const Dst &d, bool &scalar) {
        scalar = false;
        if (d.rel) { fail("relative destination"); return "r0"; }
        if (d.shift) { fail("result shift"); return "r0"; }
        char b[32];
        switch (d.type) {
        case RT_TEMP: temps.insert(d.reg); snprintf(b, sizeof(b), "r%d", d.reg); return b;
        case RT_OUTPUT:
            if (sh.pixel) break;
            outputs.insert(d.reg); snprintf(b, sizeof(b), "o%d", d.reg); return b;
        case RT_COLOROUT:
            if (!sh.pixel || d.reg > 3) break;
            colorOuts.insert(d.reg); snprintf(b, sizeof(b), "oC%d", d.reg); return b;
        case RT_DEPTHOUT:
            if (!sh.pixel) break;
            out.writesDepth = true; scalar = true; return "oDepth";
        default: break;
        }
        snprintf(b, sizeof(b), "destination register type %d", d.type);
        fail(b);
        return "r0";
    }

    void assign(const Instr &in, const std::string &expr) {
        bool scalar;
        const std::string d = dst(in.dst, scalar);
        std::string e = expr;
        if (in.dst.mod & DMOD_SAT) e = "saturate(" + e + ")";
        if (scalar)
            body << ind() << d << " = (" << e << ").x;\n";
        else
            body << ind() << d << maskStr(in.dst.mask) << " = (" << e << ")" << maskStr(in.dst.mask) << ";\n";
    }

    // A texture lookup's coordinates for the sampler's dimension.
    std::string coords(int dim, const std::string &c, bool project) {
        const std::string sel = dim == DIM_2D ? ".xy" : ".xyz";
        return project ? "(" + c + sel + " / " + c + ".w)" : c + sel;
    }

    std::string texture(const Instr &in, const char *kind) {
        if (in.nsrc < 2 || in.src[1].type != RT_SAMPLER) { fail("texture lookup without a sampler"); return "float4(0.0)"; }
        const int s = in.src[1].reg;
        const Decl *dcl = sh.sampler(s);
        if (!dcl || s > 15) { fail("undeclared sampler"); return "float4(0.0)"; }
        const int dim = dcl->dim;
        if (dim != DIM_2D && dim != DIM_CUBE && dim != DIM_VOLUME) { fail("sampler dimension"); return "float4(0.0)"; }
        out.samplerMask |= 1u << s;
        out.samplerDim[s] = (unsigned char)dim;
        const std::string c = src(in.src[0]);
        char t[32];
        std::string e;
        snprintf(t, sizeof(t), "t%d.sample(s%d, ", s, s);
        if (!strcmp(kind, "tex")) {
            if (!sh.pixel) { fail("texld in a vertex shader"); return "float4(0.0)"; }
            if (in.ctrl == TEX_PLAIN)        e = t + coords(dim, c, false) + ")";
            else if (in.ctrl == TEX_PROJECT) e = t + coords(dim, c, true) + ")";
            else if (in.ctrl == TEX_BIAS)    e = t + coords(dim, c, false) + ", bias(" + c + ".w))";
            else { fail("texld flavour"); return "float4(0.0)"; }
        } else if (!strcmp(kind, "lod")) {
            e = t + coords(dim, c, false) + ", level(" + c + ".w))";
        } else {
            if (!sh.pixel || in.nsrc < 4) { fail("texldd operands"); return "float4(0.0)"; }
            const char *g = dim == DIM_2D ? "gradient2d" : dim == DIM_CUBE ? "gradientcube" : "gradient3d";
            const std::string sel = dim == DIM_2D ? ".xy" : ".xyz";
            e = t + coords(dim, c, false) + ", " + g + "(" + src(in.src[2]) + sel + ", " + src(in.src[3]) + sel + "))";
        }
        if (in.src[1].mod) fail("sampler modifier");
        // A shadow map (the device binds a depth texture there, nothing else) is read
        // with texldl and texldp only (the spec): D3D9's hardware shadow-map lookup
        // compares z (texldp: z/w) with the stored depths, filtered (the sampler's
        // compare function, set by the device).
        if (sh.pixel && (shadow >> s & 1)) {
            const bool project = !strcmp(kind, "tex") && in.ctrl == TEX_PROJECT;
            if (dim != DIM_2D || (!project && strcmp(kind, "lod"))) { fail("shadow map lookup that can't compare"); return "float4(0.0)"; }
            if (!rawShadows) {
                const std::string z = project ? c + ".z / " + c + ".w" : c + ".z";
                snprintf(t, sizeof(t), "t%d.sample_compare(s%d, ", s, s);
                e = std::string("float4(") + t + coords(dim, c, project) + ", " + z + "))";
            }
        }
        return e + swizStr(in.src[1].swizzle);
    }

    std::string cmp(int ctrl, const Src &a, const Src &b) {
        static const char *ops[] = { "", ">", "==", ">=", "<", "!=", "<=" };
        if (ctrl < 1 || ctrl > 6) { fail("comparison"); return "false"; }
        return "(" + src(a) + ").x " + ops[ctrl] + " (" + src(b) + ").x";
    }

    void instr(const Instr &in) {
        if (in.predicated) { fail("predicated instruction"); return; }
        auto s = [&](int i) { return src(in.src[i]); };
        switch (in.op) {
        case OP_NOP: return;
        case OP_MOV:    assign(in, s(0)); return;
        case OP_ADD:    assign(in, s(0) + " + " + s(1)); return;
        case OP_SUB:    assign(in, s(0) + " - " + s(1)); return;
        case OP_MUL:    assign(in, s(0) + " * " + s(1)); return;
        case OP_MAD:    assign(in, s(0) + " * " + s(1) + " + " + s(2)); return;
        case OP_RCP:    assign(in, "float4(1.0 / (" + s(0) + ").x)"); return;
        case OP_RSQ:    assign(in, "float4(rsqrt(abs((" + s(0) + ").x)))"); return;
        case OP_DP3:    assign(in, "float4(dot((" + s(0) + ").xyz, (" + s(1) + ").xyz))"); return;
        case OP_DP4:    assign(in, "float4(dot(" + s(0) + ", " + s(1) + "))"); return;
        case OP_DP2ADD: assign(in, "float4(dot((" + s(0) + ").xy, (" + s(1) + ").xy) + (" + s(2) + ").x)"); return;
        case OP_MIN:    assign(in, "min(" + s(0) + ", " + s(1) + ")"); return;
        case OP_MAX:    assign(in, "max(" + s(0) + ", " + s(1) + ")"); return;
        case OP_SLT:    assign(in, "float4(" + s(0) + " < " + s(1) + ")"); return;
        case OP_SGE:    assign(in, "float4(" + s(0) + " >= " + s(1) + ")"); return;
        case OP_EXP:    assign(in, "float4(exp2((" + s(0) + ").x))"); return;
        case OP_LOG:    assign(in, "float4(log2(abs((" + s(0) + ").x)))"); return;
        case OP_POW:    assign(in, "float4(pow(abs((" + s(0) + ").x), (" + s(1) + ").x))"); return;
        case OP_FRC:    assign(in, "fract(" + s(0) + ")"); return;
        case OP_ABS:    assign(in, "abs(" + s(0) + ")"); return;
        case OP_NRM:    assign(in, "kb_nrm(" + s(0) + ")"); return;
        case OP_SINCOS: assign(in, "float4(cos((" + s(0) + ").x), sin((" + s(0) + ").x), 0.0, 0.0)"); return;
        case OP_LRP:    assign(in, "mix(" + s(2) + ", " + s(1) + ", " + s(0) + ")"); return;
        case OP_CMP:    assign(in, "select(" + s(2) + ", " + s(1) + ", " + s(0) + " >= 0.0)"); return;
        case OP_DSX:    assign(in, "dfdx(" + s(0) + ")"); return;
        case OP_DSY:    assign(in, "dfdy(" + s(0) + ")"); return;
        case OP_TEX:    assign(in, texture(in, "tex")); return;
        case OP_TEXLDL: assign(in, texture(in, "lod")); return;
        case OP_TEXLDD: assign(in, texture(in, "grad")); return;
        case OP_MOVA: {
            if (sh.pixel || in.dst.type != RT_ADDR) { fail("mova destination"); return; }
            usesA0 = true;
            const std::string m = maskStr(in.dst.mask);
            body << ind() << "a0" << m << " = int4(floor(" << s(0) << " + 0.5))" << m << ";\n";
            return;
        }
        case OP_TEXKILL: {
            if (!sh.pixel || in.dst.type != RT_TEMP) { fail("texkill operand"); return; }
            Src r;
            r.type = RT_TEMP; r.reg = in.dst.reg;
            std::string m = maskStr(in.dst.mask);
            if (m.empty()) m = ".xyzw";
            const std::string v = reg(r) + m;
            body << ind() << "if (any(" << v << " < 0.0)) discard_fragment();\n";
            return;
        }
        case OP_IF:
            if (in.src[0].type != RT_CONSTBOOL || !sh.bdefs.count(in.src[0].reg)) { fail("if on a runtime boolean"); return; }
            body << ind() << "if (" << (sh.bdefs.at(in.src[0].reg) ? "true" : "false") << ") {\n";
            ++depth; blocks.push_back(0);
            return;
        case OP_IFC:
            body << ind() << "if (" << cmp(in.ctrl, in.src[0], in.src[1]) << ") {\n";
            ++depth; blocks.push_back(0);
            return;
        case OP_ELSE:
            if (blocks.empty() || blocks.back() != 0) { fail("else outside if"); return; }
            body << std::string(2 * (depth - 1), ' ') << "} else {\n";
            return;
        case OP_ENDIF: case OP_ENDREP: case OP_ENDLOOP:
            if (blocks.empty()) { fail("unbalanced control flow"); return; }
            --depth;
            body << ind() << (blocks.back() == 1 ? "}}\n" : "}\n");
            if (blocks.back() == 1) aL.pop_back();
            blocks.pop_back();
            return;
        case OP_BREAK:  body << ind() << "break;\n"; return;
        case OP_BREAKC: body << ind() << "if (" << cmp(in.ctrl, in.src[0], in.src[1]) << ") break;\n"; return;
        case OP_REP: {
            if (in.src[0].type != RT_CONSTINT || !sh.idefs.count(in.src[0].reg)) { fail("rep count"); return; }
            const int n = sh.idefs.at(in.src[0].reg)[0], id = loopCount++;
            body << ind() << "for (int rep" << id << " = 0; rep" << id << " < " << n << "; ++rep" << id << ") {\n";
            ++depth; blocks.push_back(0);
            return;
        }
        case OP_LOOP: {
            if (in.nsrc < 2 || in.src[1].type != RT_CONSTINT || !sh.idefs.count(in.src[1].reg)) { fail("loop count"); return; }
            const std::array<int, 4> &i = sh.idefs.at(in.src[1].reg);
            const int id = loopCount++;
            const std::string v = "aL" + std::to_string(id);
            body << ind() << "{ int " << v << " = " << i[1] << "; for (int lc" << id << " = 0; lc" << id << " < " << i[0]
                 << "; ++lc" << id << ", " << v << " += " << i[2] << ") {\n";
            aL.push_back(v);
            ++depth; blocks.push_back(1);
            return;
        }
        default:
            fail(std::string("instruction ") + OpName(in.op));
            return;
        }
    }

    bool run(const std::string &pfx) {
        prefix = pfx;
        if (sh.pixel)
            for (const auto &n : sh.samplerNames)
                if (n.first < 16 && !n.second.compare(0, 16, "shadowmapSampler")) shadow |= 1u << n.first;
        const std::string sfx = pfx.empty() ? "" : "_" + pfx;
        out.entry = pfx.empty() ? "kb_main" : "kb_" + pfx;
        for (const Instr &in : sh.code) instr(in);
        out.shadowMask = shadow & out.samplerMask;
        if (!blocks.empty()) fail("unclosed control flow");
        if (failed) return false;

        std::ostringstream o;
        // Inputs.
        std::vector<std::pair<int, std::string>> ins;   // register -> struct member
        std::ostringstream inStruct;
        if (!sh.pixel) {
            for (const Decl &d : sh.decls) {
                if (d.type != RT_INPUT) continue;
                const int a = MSL_AttribIndex(d.usage, d.index);
                if (a < 0) { fail("vertex input usage " + std::to_string(d.usage)); return false; }
                if (out.attribMask & (1u << a)) { fail("two inputs with one usage"); return false; }
                out.attribMask |= 1u << a;
                inStruct << "  float4 a" << a << " [[attribute(" << a << ")]];\n";
                ins.push_back({d.reg, "a" + std::to_string(a)});
            }
        } else {
            for (const Decl &d : sh.decls) {
                if (d.type != RT_INPUT) continue;
                std::string name;
                int slot;
                if (!varying(d.usage, d.index, name, slot)) { fail("pixel input usage " + std::to_string(d.usage)); return false; }
                out.varyingsIn |= 1ull << slot;
                // Per sample, the inputs too: at the pixel centre every sample would
                // see the same texcoord and pass or fail the alpha test together.
                inStruct << "  float4 " << name << " [[user(" << name << ")"
                         << (perSample ? ", sample_perspective" : d.mod & DMOD_CENTROID ? ", centroid_perspective" : "")
                         << "]];\n";
                ins.push_back({d.reg, name});
            }
            if (usesPos)  inStruct << "  float4 kbPos [[position]];\n";
            if (usesFace) inStruct << "  bool kbFace [[front_facing]];\n";
            if (perSample) inStruct << "  uint kbSample [[sample_id]];\n";
        }
        const bool haveIn = !inStruct.str().empty();
        if (haveIn) o << "struct In" << sfx << " {\n" << inStruct.str() << "};\n";

        // Outputs.
        std::string posReg;
        std::vector<std::pair<std::string, std::string>> outs;   // member <- local
        o << "struct Out" << sfx << " {\n";
        if (!sh.pixel) {
            for (const Decl &d : sh.decls) {
                if (d.type != RT_OUTPUT) continue;
                const std::string local = "o" + std::to_string(d.reg);
                if (d.usage == U_POSITION && d.index == 0) { posReg = local; continue; }
                if (d.usage == U_PSIZE) { fail("point size output"); return false; }
                std::string name;
                int slot;
                if (!varying(d.usage, d.index, name, slot)) { fail("vertex output usage " + std::to_string(d.usage)); return false; }
                out.varyingsOut |= 1ull << slot;
                o << "  float4 " << name << " [[user(" << name << ")]];\n";
                outs.push_back({name, local});
            }
            if (posReg.empty()) { fail("no position output"); return false; }
            o << "  float4 kbPos [[position, invariant]];\n";
        } else {
            for (int c : colorOuts) o << "  float4 c" << c << " [[color(" << c << ")]];\n";
            if (out.writesDepth) o << "  float depth [[depth(any)]];\n";
            if (colorOuts.empty() && !out.writesDepth) o << "  float4 c0 [[color(0)]];\n";
        }
        o << "};\n\n";

        // Entry point.
        out.constCount = maxConst + 1;
        std::vector<std::string> params;
        if (haveIn) params.push_back("In" + sfx + " in [[stage_in]]");
        if (out.constCount > 0)
            params.push_back("constant float4 *c [[buffer(" + std::to_string(MSL_BUF_CONSTANTS) + ")]]");
        params.push_back("constant float4 &kbDraw [[buffer(" + std::to_string(MSL_BUF_DRAW) + ")]]");
        for (int s = 0; s < 16; ++s) {
            if (!(out.samplerMask & (1u << s))) continue;
            const char *ty = (out.shadowMask >> s & 1) && !rawShadows ? "depth2d<float>"
                           : out.samplerDim[s] == DIM_2D ? "texture2d<float>"
                           : out.samplerDim[s] == DIM_CUBE ? "texturecube<float>" : "texture3d<float>";
            params.push_back(std::string(ty) + " t" + std::to_string(s) + " [[texture(" + std::to_string(s) + ")]]");
            params.push_back("sampler s" + std::to_string(s) + " [[sampler(" + std::to_string(s) + ")]]");
        }
        if (relDefs) {
            // Relative reads see the def'd constants, as D3D9's constant file holds them.
            o << "static float4 kb_crel" << sfx << "(constant float4 *c, int i) {\n";
            for (const auto &d : sh.defs)
                o << "  if (i == " << d.first << ") return float4(" << fmtFloat(d.second[0]) << ", " << fmtFloat(d.second[1])
                  << ", " << fmtFloat(d.second[2]) << ", " << fmtFloat(d.second[3]) << ");\n";
            o << "  return c[clamp(i, 0, 255)];\n}\n\n";
        }
        o << (sh.pixel ? "fragment" : "vertex") << " Out" << sfx << " " << out.entry << "(";
        for (size_t i = 0; i < params.size(); ++i) o << (i ? ",\n    " : "") << params[i];
        o << ")\n{\n";
        for (const auto &d : sh.defs)
            o << "  const float4 c" << d.first << "d = float4(" << fmtFloat(d.second[0]) << ", " << fmtFloat(d.second[1])
              << ", " << fmtFloat(d.second[2]) << ", " << fmtFloat(d.second[3]) << ");\n";
        for (const auto &i : ins) o << "  float4 v" << i.first << " = in." << i.second << ";\n";
        for (int r : temps) o << "  float4 r" << r << " = float4(0.0);\n";
        if (usesA0) o << "  int4 a0 = int4(0);\n";
        if (!sh.pixel) {
            for (const Decl &d : sh.decls)
                if (d.type == RT_OUTPUT) o << "  float4 o" << d.reg << " = float4(0.0);\n";
        } else {
            for (int c : colorOuts) o << "  float4 oC" << c << " = float4(0.0);\n";
            if (colorOuts.empty() && !out.writesDepth) o << "  float4 oC0 = float4(0.0);\n";
            if (out.writesDepth) o << "  float oDepth = 0.0;\n";
        }
        o << body.str();
        o << "  Out" << sfx << " out;\n";
        if (!sh.pixel) {
            for (const auto &p : outs) o << "  out." << p.first << " = " << p.second << ";\n";
            // D3D9 samples pixel centres at integer coordinates, Metal at +0.5: move the
            // geometry half a pixel right and down (kbDraw = 1/width, -1/height).
            o << "  out.kbPos = " << posReg << ";\n";
            o << "  out.kbPos.xy += kbDraw.xy * out.kbPos.w;\n";
        } else {
            const int a = colorOuts.empty() ? 0 : *colorOuts.begin();
            if (!colorOuts.count(0) && !colorOuts.empty()) { fail("alpha test without oC0"); return false; }
            // The D3D9 alpha test on oC0 (kbDraw.x = the reference, 0..1).
            o << "  if (kbAlphaFunc != 0) {\n"
              << "    const float a = oC" << a << ".a, ref = kbDraw.x;\n"
              << "    bool pass = kbAlphaFunc == 8 || (kbAlphaFunc == 2 && a < ref) || (kbAlphaFunc == 3 && a == ref) ||\n"
              << "                (kbAlphaFunc == 4 && a <= ref) || (kbAlphaFunc == 5 && a > ref) ||\n"
              << "                (kbAlphaFunc == 6 && a != ref) || (kbAlphaFunc == 7 && a >= ref);\n"
              << "    if (!pass) discard_fragment();\n"
              << "  }\n";
            for (int c : colorOuts) o << "  out.c" << c << " = oC" << c << ";\n";
            if (colorOuts.empty() && !out.writesDepth) o << "  out.c0 = oC0;\n";
            if (out.writesDepth) o << "  out.depth = oDepth;\n";
            for (int c : colorOuts) out.colorOutMask |= 1u << c;
        }
        o << "  return out;\n}\n";
        out.source = o.str();
        return !failed;
    }
};

} // namespace

const char *MSL_Prelude() {
    // kbAlphaFunc: the pixel shaders' alpha test (MSL_FC_ALPHAFUNC).
    return "#include <metal_stdlib>\nusing namespace metal;\n\n"
           "static inline float4 kb_nrm(float4 v) { return v * rsqrt(dot(v.xyz, v.xyz)); }\n"
           "constant int kbAlphaFunc [[function_constant(0)]];\n\n";
}

int MSL_AttribIndex(int usage, int usageIndex) {
    switch (usage) {
    case U_POSITION: case U_POSITIONT: return usageIndex == 0 ? 0 : -1;
    case U_BLENDWEIGHT:  return usageIndex == 0 ? 1 : -1;
    case U_BLENDINDICES: return usageIndex == 0 ? 2 : -1;
    case U_NORMAL:       return usageIndex == 0 ? 3 : -1;
    case U_TANGENT:      return usageIndex == 0 ? 4 : -1;
    case U_COLOR:        return usageIndex <= 1 ? 5 + usageIndex : -1;
    case U_TEXCOORD:     return usageIndex <= 8 ? 7 + usageIndex : -1;    // 7..15
    case U_BINORMAL:     return usageIndex == 0 ? 16 : -1;
    case U_PSIZE:        return usageIndex == 0 ? 17 : -1;
    default:             return -1;
    }
}

bool MSL_Translate(const Shader &sh, MSLShader &out, const std::string &prefix, bool perSample, bool rawShadows) {
    out = MSLShader();
    if (sh.major != 3) { out.error = "shader model " + std::to_string(sh.major) + "." + std::to_string(sh.minor); return false; }
    Emitter e(sh, out);
    e.perSample = perSample && sh.pixel;
    e.rawShadows = rawShadows;
    return e.run(prefix);
}
