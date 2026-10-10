// gl_shader.cpp — DX9 bytecode → GLSL 120 translator + GL shader objects.
#include "gl_shader.h"
#include "gl_platform.h"
#include "gl_spec.h"
#include "../gfx_shader/sm3.h"

#include <GL/glew.h>
#include <map>
#include <set>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using namespace sm3;

// One operand as the emitters below take it: a source (swizzle, mod) or a destination
// (writemask, dmod), from the parsed shader (sm3.h).
struct Operand { int type = 0, reg = 0, swizzle = 0xE4, writemask = 0xF, mod = 0, dmod = 0;
                 bool rel = false; int relComp = 0; };  // c[a0.<relComp> + reg]

Operand fromSrc(const Src &s) {
    Operand o;
    o.type = s.type; o.reg = s.reg; o.swizzle = s.swizzle; o.mod = s.mod; o.rel = s.rel; o.relComp = s.relComp;
    return o;
}

Operand fromDst(const Dst &d) {
    Operand o;
    o.type = d.type; o.reg = d.reg; o.writemask = d.mask; o.dmod = d.mod;
    return o;
}

const char *kComp = "xyzw";

std::string maskStr(int m) {  // dest write-mask, e.g. 0b0111 -> ".xyz"
    if (m == 0xF || m == 0) return "";
    std::string s = ".";
    for (int i = 0; i < 4; ++i) if (m & (1 << i)) s += kComp[i];
    return s;
}

std::string swizStr(int sw) {  // src swizzle as a 4-component selector, "" if identity
    if (sw == 0xE4) return "";  // .xyzw
    std::string s = ".";
    for (int i = 0; i < 4; ++i) s += kComp[(sw >> (i * 2)) & 0x3];
    return s;
}

// GLSL output dialect: `#version 120` compat GLSL (Linux), or `#version 300 es`
// (GLSL ES 3.00) on the macOS core profile, which KB_GLSLForContext turns into GLSL
// 4.10 (gl_platform.h).
#if defined(KB_GL_MODERN_GLSL)
static const bool kEmitES_default = true;
#else
static const bool kEmitES_default = false;
#endif

struct Ctx {
    bool isPixel = false;
    bool emitES = kEmitES_default;              // GLSL ES 3.00 vs #version 120
    const char *cArr() const { return isPixel ? "psc" : "vsc"; }
    // Pixel-shader color output: gl_FragColor in 120, a declared `out` in ES 3.00.
    const char *fragColorName() const { return emitES ? "fragColor" : "gl_FragColor"; }
    // Texture sampler call: texture2D in 120, overloaded texture() in ES 3.00.
    const char *texFn() const { return emitES ? "texture" : "texture2D"; }
    unsigned shadowMask = 0;   // samplers typed sampler2DShadow (depth-compare lookups)
    std::map<int, std::pair<int,int>> inputs;   // reg -> (usage, usageIndex)
    std::map<int, std::pair<int,int>> outputs;  // reg -> (usage, usageIndex)  (vertex)
    std::set<int> samplers;
    std::map<int, int> samplerDim;   // sampler reg -> D3DSTT: 2=2D, 3=CUBE, 4=VOLUME
    std::map<int, float[4]> defs;
    std::map<int, int> idefs;                   // DEFI integer constant reg -> count (.x)
    std::set<int> usedTemps;
    bool usedConst = false;
    bool usedA0    = false;   // MOVA / relative addressing: declare ivec4 a0
    int  maxConst  = -1;   // highest runtime-constant register referenced (sizes vsc[]/psc[])
};

// D3DDECLUSAGE -> a stable varying name shared between the vs and ps stages.
std::string varyingName(int usage, int index) {
    static const char *u[] = {"POSITION","BLENDW","BLENDI","NORMAL","PSIZE","TEXCOORD",
                              "TANGENT","BINORMAL","TESSFACTOR","POSITIONT","COLOR","FOG",
                              "DEPTH","SAMPLE"};
    std::ostringstream s;
    s << "v_" << (usage >= 0 && usage < 14 ? u[usage] : "X") << index;
    return s.str();
}

std::string regName(Ctx &c, const Operand &o, bool isDest) {
    switch (o.type) {
        case RT_TEMP:    c.usedTemps.insert(o.reg); { std::ostringstream s; s << "r" << o.reg; return s.str(); }
        case RT_CONST:
            if (o.rel) {
                // c[a0.<comp> + N]: the runtime index can reach any register, so the
                // array must span all 256 (clamped — OOB indexing is UB in GLSL).
                c.usedConst = true; c.usedA0 = true; c.maxConst = 255;
                std::ostringstream s;
                s << c.cArr() << "[clamp(a0." << kComp[o.relComp] << " + " << o.reg << ", 0, 255)]";
                return s.str();
            }
            if (c.defs.count(o.reg)) { std::ostringstream s; s << "c" << o.reg << "_def"; return s.str(); }
            c.usedConst = true; if (o.reg > c.maxConst) c.maxConst = o.reg;
            { std::ostringstream s; s << c.cArr() << "[" << o.reg << "]"; return s.str(); }
        case RT_INPUT:
            if (c.isPixel) { auto it = c.inputs.find(o.reg);
                             return it != c.inputs.end() ? varyingName(it->second.first, it->second.second) : "vec4(0.0)"; }
            else { auto it = c.inputs.find(o.reg);
                   return it != c.inputs.end() ? GLAttribName(it->second.first, it->second.second) : "aPos"; }
        case RT_OUTPUT: {
            auto it = c.outputs.find(o.reg);
            if (it != c.outputs.end()) {
                if (it->second.first == D3DDECLUSAGE_POSITION || it->second.first == D3DDECLUSAGE_POSITIONT)
                    return "gl_Position";
                return varyingName(it->second.first, it->second.second);
            }
            return o.reg == 0 ? "gl_Position" : "vec4(0.0)";
        }
        case RT_RASTOUT:  return "gl_Position";
        case RT_COLOROUT: return c.fragColorName();
        case RT_SAMPLER:  { std::ostringstream s; s << "s" << o.reg; return s.str(); }
        case RT_MISCTYPE: // vPos (pixel position) / vFace (front-facing). vPos was
                          // unhandled (-> vec4(0)), breaking every screen-space sample
                          // (light/refraction passes read the scene buffer at vPos*texel).
                          // D3D9's vPos is the integer pixel position (centres at
                          // integers); gl_FragCoord's centres are at +0.5.
            return o.reg == 1 ? "vec4(gl_FrontFacing ? 1.0 : -1.0)" : "vec4(floor(gl_FragCoord.xy), gl_FragCoord.zw)";
        default: (void)isDest; return "vec4(0.0)";
    }
}

// Apply a D3DSPSM_* source modifier (bias/sign/x2 etc. expand packed normals;
// missing these left lighting/normal-map maths under-saturated -> washed out).
std::string applyMod(const std::string &e, int mod) {
    switch (mod) {
        case 1:  return "(-" + e + ")";                 // negate
        case 2:  return "(" + e + " - 0.5)";            // bias
        case 3:  return "(-(" + e + " - 0.5))";         // bias-negate
        case 4:  return "(2.0*(" + e + ") - 1.0)";      // sign / _bx2
        case 5:  return "(-(2.0*(" + e + ") - 1.0))";   // _bx2 negate
        case 6:  return "(1.0 - " + e + ")";            // complement
        case 7:  return "(2.0*(" + e + "))";            // _x2
        case 8:  return "(-2.0*(" + e + "))";           // _x2 negate
        case 11: return "abs(" + e + ")";               // abs
        case 12: return "(-abs(" + e + "))";            // abs-negate
        default: return e;                              // none / unsupported (dz,dw,not)
    }
}

std::string srcExpr(Ctx &c, const Operand &o) {
    return applyMod(regName(c, o, false) + swizStr(o.swizzle), o.mod);
}

// Emit one instruction as `dst<mask> = (<expr>)<mask>;`.
void emitInstr(Ctx &c, int op, const Operand *src, int nsrc, const Operand &dst,
               std::ostringstream &body, const std::string &ind, int ctrl = 0) {
    auto s = [&](int i) { return srcExpr(c, src[i]); };
    // TEXKILL discards the fragment when any of the tested register's xyz < 0; it has
    // no destination, so handle it before the dst<mask>= path below.
    if (op == OP_TEXKILL) {
        body << ind << "if (any(lessThan((" << regName(c, dst, true) << ").xyz, vec3(0.0)))) discard;\n";
        return;
    }
    // MOVA loads the address register a0 (round-to-nearest per D3D9 spec); a0 then
    // drives relative constant addressing (bone-matrix indexing in skinned meshes).
    if (op == OP_MOVA) {
        c.usedA0 = true;
        std::string m = maskStr(dst.writemask);
        body << ind << "a0" << m << " = ivec4(floor(" << s(0) << " + 0.5))" << m << ";\n";
        return;
    }
    std::string expr;
    switch (op) {
        case OP_MOV:   expr = s(0); break;
        case OP_ADD:   expr = s(0) + " + " + s(1); break;
        case OP_SUB:   expr = s(0) + " - " + s(1); break;
        case OP_MUL:   expr = s(0) + " * " + s(1); break;
        case OP_MAD:   expr = s(0) + " * " + s(1) + " + " + s(2); break;
        case OP_LRP:   expr = "mix(" + s(2) + ", " + s(1) + ", " + s(0) + ")"; break;  // dst = src2 + src0*(src1-src2)
        case OP_MIN:   expr = "min(" + s(0) + ", " + s(1) + ")"; break;
        case OP_MAX:   expr = "max(" + s(0) + ", " + s(1) + ")"; break;
        case OP_FRC:   expr = "fract(" + s(0) + ")"; break;
        case OP_ABS:   expr = "abs(" + s(0) + ")"; break;
        case OP_SLT:   expr = "vec4(lessThan(" + s(0) + ", " + s(1) + "))"; break;
        case OP_SGE:   expr = "vec4(greaterThanEqual(" + s(0) + ", " + s(1) + "))"; break;
        case OP_CMP:   expr = "mix(" + s(2) + ", " + s(1) + ", vec4(greaterThanEqual(" + s(0) + ", vec4(0.0))))"; break;  // src0>=0 ? src1 : src2
        case OP_RCP:   expr = "vec4(1.0 / (" + s(0) + ").x)"; break;
        case OP_RSQ:   expr = "vec4(inversesqrt((" + s(0) + ").x))"; break;
        case OP_EXP:   expr = "vec4(exp2((" + s(0) + ").x))"; break;
        case OP_LOG:   expr = "vec4(log2(abs((" + s(0) + ").x)))"; break;
        case OP_POW:   expr = "vec4(pow(abs((" + s(0) + ").x), (" + s(1) + ").x))"; break;
        case OP_NRM:   expr = "vec4(normalize((" + s(0) + ").xyz), 0.0)"; break;
        case OP_SINCOS:expr = "vec4(cos((" + s(0) + ").x), sin((" + s(0) + ").x), 0.0, 0.0)"; break;
        case OP_DP3:   expr = "vec4(dot((" + s(0) + ").xyz, (" + s(1) + ").xyz))"; break;
        case OP_DP4:   expr = "vec4(dot(" + s(0) + ", " + s(1) + "))"; break;
        case OP_DP2ADD:expr = "vec4(dot((" + s(0) + ").xy, (" + s(1) + ").xy) + (" + s(2) + ").x)"; break;
        // Screen-space partial derivatives. Dropping these (the dest stayed vec4(0)) broke
        // every pixel shader that uses ddx/ddy — mip selection and distance detail — which
        // is what made distant surfaces blow out to white/blue on mp_mountain. dFdx/dFdy are
        // core in both GLSL ES 3.00 and desktop 1.20.
        case OP_DSX:   expr = "dFdx(" + s(0) + ")"; break;
        case OP_DSY:   expr = "dFdy(" + s(0) + ")"; break;
        case OP_TEX: {
            // The instruction-token control bits select the texld FLAVOUR (ignored before
            // -- which broke every projected lookup): 1 = texldp (projective divide; on a
            // depth texture this is the D3D9 hardware-shadow compare), 2 = texldb (bias).
            std::string samp = regName(c, src[1], false);
            int sreg = src[1].reg;
            bool shadow = (c.shadowMask >> sreg) & 1;
            if (shadow) {
                // sampler2DShadow: PCF depth-comparison result. KB_sc does the projective
                // divide AND the render-target Y flip (see prologue note).
                if (ctrl == 1)
                    expr = c.emitES ? "vec4(texture(" + samp + ", KB_sc(" + s(0) + ")))"
                                    : "vec4(shadow2D(" + samp + ", KB_sc(" + s(0) + ")).x)";
                else
                    expr = c.emitES ? "vec4(texture(" + samp + ", KB_sc(vec4((" + s(0) + ").xyz, 1.0))))"
                                    : "vec4(shadow2D(" + samp + ", KB_sc(vec4((" + s(0) + ").xyz, 1.0))).x)";
            } else if (ctrl == 1) {
                expr = std::string(c.emitES ? "textureProj" : "texture2DProj") + "(" + samp + ", " + s(0) + ")";
            } else {
                // Coordinate arity follows the sampler's declared dimension: cube and
                // volume lookups take .xyz (model lighting samples a 3D texture in the
                // VERTEX shader; .xy there is a GLSL type error or wrong texel).
                int dim = c.samplerDim.count(sreg) ? c.samplerDim.at(sreg) : 2;
                const char *coords = (dim == 3 || dim == 4) ? ".xyz" : ".xy";
                const char *fn = c.emitES ? "texture"
                                  : dim == 4 ? "texture3D" : dim == 3 ? "textureCube" : "texture2D";
                if (ctrl == 2)
                    expr = std::string(fn) + "(" + samp + ", (" + s(0) + ")" + coords + ", (" + s(0) + ").w)";
                else
                    expr = std::string(fn) + "(" + samp + ", (" + s(0) + ")" + coords + ")";
            }
            break;
        }
        case OP_TEXLDL: {
            // Explicit-LOD sample: the LOD rides in coord .w. DROPPING it sampled mip 0
            // forever — every cubemap reflection razor-sharp = the "vaseline gloss"
            // (CoD blurs reflections by gloss via this LOD).
            int sreg2 = src[1].reg;
            int dim = c.samplerDim.count(sreg2) ? c.samplerDim.at(sreg2) : 2;
            const char *coords = (dim == 3 || dim == 4) ? ".xyz" : ".xy";
            if (c.emitES)
                expr = "textureLod(" + regName(c, src[1], false) + ", (" + s(0) + ")" + coords + ", (" + s(0) + ").w)";
            else {
                const char *fn = dim == 4 ? "texture3D" : dim == 3 ? "textureCube" : "texture2D";
                expr = std::string(fn) + "(" + regName(c, src[1], false) + ", (" + s(0) + ")" + coords + ")";
            }
            break;
        }
        case OP_TEXLDD: {
            // Explicit-gradient sample (ddx/ddy in src[2]/src[3]).
            int sreg2 = src[1].reg;
            int dim = c.samplerDim.count(sreg2) ? c.samplerDim.at(sreg2) : 2;
            const char *coords = (dim == 3 || dim == 4) ? ".xyz" : ".xy";
            if (c.emitES && nsrc >= 4)
                expr = "textureGrad(" + regName(c, src[1], false) + ", (" + s(0) + ")" + coords +
                       ", (" + srcExpr(c, src[2]) + ")" + coords + ", (" + srcExpr(c, src[3]) + ")" + coords + ")";
            else {
                const char *fn = c.emitES ? "texture"
                                  : dim == 4 ? "texture3D" : dim == 3 ? "textureCube" : "texture2D";
                expr = std::string(fn) + "(" + regName(c, src[1], false) + ", (" + s(0) + ")" + coords + ")";
            }
            break;
        }
        default: (void)nsrc; return;
    }
    std::string dn = regName(c, dst, true);
    std::string m  = maskStr(dst.writemask);
    if (dst.dmod & 1) expr = "clamp(" + expr + ", 0.0, 1.0)";   // _sat: clamp result to [0,1]
    body << ind << dn << m << " = (" << expr << ")" << m << ";\n";
}

} // namespace

// ---- Canonical vertex-attribute mapping (shared, see gl_shader.h) ----------
//
// One generic attribute location per (usage, usageIndex), laid out to fit the 16
// locations GL guarantees. The same (usage,index) always yields the same location
// and name across the translator, the linker and the device's vertex setup.
int GLAttribLocation(int usage, int usageIndex) {
    switch (usage) {
        case D3DDECLUSAGE_POSITION:
        case D3DDECLUSAGE_POSITIONT:   return 0;
        case D3DDECLUSAGE_BLENDWEIGHT: return 1;
        case D3DDECLUSAGE_BLENDINDICES:return 2;
        case D3DDECLUSAGE_NORMAL:      return 3;
        case D3DDECLUSAGE_TANGENT:     return 4;
        case D3DDECLUSAGE_COLOR:       return usageIndex <= 1 ? 5 + usageIndex : -1;   // COLOR0..1  -> 5,6
        case D3DDECLUSAGE_TEXCOORD:    return usageIndex <= 8 ? 7 + usageIndex : -1;   // TEXCOORD0..8 -> 7..15
        default:                       return -1;
    }
}

std::string GLAttribName(int usage, int usageIndex) {
    std::ostringstream s;
    switch (usage) {
        case D3DDECLUSAGE_POSITION:
        case D3DDECLUSAGE_POSITIONT:    return "aPos";
        case D3DDECLUSAGE_BLENDWEIGHT:  return "aBlendWeight";
        case D3DDECLUSAGE_BLENDINDICES: return "aBlendIndices";
        case D3DDECLUSAGE_NORMAL:       return "aNormal";
        case D3DDECLUSAGE_TANGENT:      return "aTangent";
        case D3DDECLUSAGE_COLOR:        s << "aColor"    << usageIndex; return s.str();
        case D3DDECLUSAGE_TEXCOORD:     s << "aTexCoord" << usageIndex; return s.str();
        default:                        return "aPos";
    }
}

void GLBindAttribLocations(unsigned program) {
    // Bind every canonical name; GL ignores those a given shader doesn't declare.
    static const int kUsages[] = { D3DDECLUSAGE_POSITION, D3DDECLUSAGE_BLENDWEIGHT,
                                   D3DDECLUSAGE_BLENDINDICES, D3DDECLUSAGE_NORMAL,
                                   D3DDECLUSAGE_TANGENT, D3DDECLUSAGE_COLOR,
                                   D3DDECLUSAGE_TEXCOORD };
    for (int u : kUsages) {
        int maxIdx = (u == D3DDECLUSAGE_TEXCOORD) ? 8 : (u == D3DDECLUSAGE_COLOR ? 1 : 0);
        for (int i = 0; i <= maxIdx; ++i) {
            int loc = GLAttribLocation(u, i);
            if (loc >= 0) glBindAttribLocation(program, loc, GLAttribName(u, i).c_str());
        }
    }
}

static bool KB_DumpEnvWanted() {
    static int en = -1;
    if (en < 0) en = getenv("KB_DUMPENV") ? 1 : 0;
    return en == 1;
}

bool TranslateD3D9Shader(const DWORD *tok, std::string &out, bool *outIsPixel,
                         unsigned shadowSamplerMask, unsigned *outSamplerMask) {
    sm3::Shader sh;
    std::string perr;
    if (!sm3::Parse(tok, sh, perr)) {
        out = "// " + perr + "\n";
        return false;
    }
    Ctx c;
    c.isPixel = sh.pixel;
    c.shadowMask = shadowSamplerMask;
    if (outIsPixel) *outIsPixel = c.isPixel;
    for (const Decl &d : sh.decls) {
        if (d.type == RT_SAMPLER) {
            c.samplers.insert(d.reg);
            // dcl_2d/dcl_cube/dcl_volume: ignoring this typed EVERY sampler sampler2D;
            // sampling the bound 3D model-lighting volume (or a cubemap) through a
            // sampler2D is a sampler-type conflict that INVALIDATES the draw — static
            // models went black/vanished (cupboard/drapes/paintings).
            c.samplerDim[d.reg] = d.dim;
        }
        else if (d.type == RT_INPUT)  c.inputs[d.reg]  = {d.usage, d.index};
        else if (d.type == RT_OUTPUT) c.outputs[d.reg] = {d.usage, d.index};
    }
    for (const auto &d : sh.defs) std::memcpy(c.defs[d.first], d.second.data(), sizeof(float[4]));
    for (const auto &d : sh.idefs) c.idefs[d.first] = d.second[0];

    std::ostringstream body;
    int indent = 1, loopId = 0;
    auto ind = [](int n) { return std::string(2 * (n < 1 ? 1 : n), ' '); };
    // D3DSHADER_COMPARISON (ctrl): 1 GT, 2 EQ, 3 GE, 4 LT, 5 NE, 6 LE.
    auto cmpStr = [&](int cc, const Operand &a, const Operand &b) {
        static const char *ops[] = { ">", ">", "==", ">=", "<", "!=", "<=" };
        const char *o = (cc >= 1 && cc <= 6) ? ops[cc] : ">";
        return "(" + srcExpr(c, a) + ").x " + o + " (" + srcExpr(c, b) + ").x";
    };
    // Control-flow operands are read without relative addressing.
    auto plain = [](const Src &s) { Operand o = fromSrc(s); o.rel = false; return o; };
    for (const Instr &in : sh.code) {
        const int op = in.op;
        if (op == OP_LABEL || op == OP_CALL || op == OP_CALLNZ || op == OP_RET || op == OP_SETP) continue;

        // Structured control flow. (Dropping IF/ELSE/ENDIF would run BOTH branches
        // unconditionally, so an `if(lit) {..} else {flat}` left the world flat.)
        if (op == OP_ELSE)    { if (indent > 1) --indent; body << ind(indent) << "} else {\n"; ++indent; continue; }
        if (op == OP_ENDIF || op == OP_ENDREP || op == OP_ENDLOOP) { if (indent > 1) --indent; body << ind(indent) << "}\n"; continue; }
        if (op == OP_BREAK)   { body << ind(indent) << "break;\n"; continue; }
        if (op == OP_IF)      { body << ind(indent) << "if (bool((" << srcExpr(c, plain(in.src[0])) << ").x)) {\n"; ++indent; continue; }
        if (op == OP_IFC)     { body << ind(indent) << "if (" << cmpStr(in.ctrl, plain(in.src[0]), plain(in.src[1])) << ") {\n"; ++indent; continue; }
        if (op == OP_BREAKC)  { body << ind(indent) << "if (" << cmpStr(in.ctrl, plain(in.src[0]), plain(in.src[1])) << ") break;\n"; continue; }
        if (op == OP_REP || op == OP_LOOP) {
            // The count is the first operand's register's defi .x (for loop that operand
            // is aL, register 0).
            int n = 4; auto di = c.idefs.find(in.src[0].reg); if (di != c.idefs.end() && di->second > 0) n = di->second;
            int id = loopId++;
            body << ind(indent) << "for (int aL" << id << " = 0; aL" << id << " < " << n << "; ++aL" << id << ") {\n";
            ++indent; continue;
        }

        // Arithmetic / sample: the destination, then up to 4 sources (texldd carries
        // coords, sampler, ddx, ddy).
        Operand dst = fromDst(in.dst);
        Operand src[4];
        for (int i = 0; i < in.nsrc; ++i) src[i] = fromSrc(in.src[i]);
        emitInstr(c, op, src, in.nsrc, dst, body, ind(indent), in.ctrl);
    }

    // Assemble the GLSL translation unit. Two dialects, parameterized on c.emitES:
    //   * #version 120 (desktop compat): attribute/varying, gl_FragColor,
    //     texture2D, fixed-function GL_ALPHA_TEST does the cutout.
    //   * #version 300 es (macOS core profile): in/out, a declared fragColor out, texture(),
    //     explicit precision, and alpha test emulated with discard (no
    //     GL_ALPHA_TEST in ES) against uAlphaTestFunc/uAlphaRef uniforms that the
    //     state layer feeds in place of glAlphaFunc.
    std::ostringstream o;
    if (c.emitES) {
        o << "#version 300 es\n";
        o << "precision highp float;\n";
        o << "precision highp int;\n";
        // ES 3.00 has NO default precision for sampler3D (unlike sampler2D/Cube) —
        // same trap as sampler2DShadow in B66. Declare it unconditionally.
        o << "precision highp sampler3D;\n";
        if (c.isPixel) {
            for (auto &in : c.inputs)  o << "in vec4 " << varyingName(in.second.first, in.second.second) << ";\n";
            // ES 3.00 defines NO default precision for sampler2DShadow — without this
            // single line every shadow variant failed to compile (and the affected
            // draws fell to the builtin, which used to fail too -> invisible geometry).
            if (c.shadowMask) o << "precision highp sampler2DShadow;\n";
            for (int sN : c.samplers)
            {
                int dim = c.samplerDim.count(sN) ? c.samplerDim.at(sN) : 2;
                const char *ty = ((c.shadowMask >> sN) & 1) ? "sampler2DShadow"
                                 : dim == 4 ? "sampler3D" : dim == 3 ? "samplerCube" : "sampler2D";
                o << "uniform " << ty << " s" << sN << ";\n";
            }
            o << "out vec4 " << c.fragColorName() << ";\n";
            // Alpha-test-via-discard uniforms (D3DCMP_* in uAlphaTestFunc; 0 = off).
            o << "uniform int uAlphaTestFunc;\n";
            o << "uniform float uAlphaRef;\n";
            if (c.shadowMask)
                // Shadow lookup coord: projective divide. No Y adjustment: FBO
                // viewports keep D3D placement (gl_d3d9.cpp SetViewport), so the
                // stored shadow atlas matches D3D-convention coords exactly.
                o << "vec3 KB_sc(vec4 c) { return c.xyz / c.w; }\n";
        } else {
            for (auto &in : c.inputs)  o << "in vec4 " << GLAttribName(in.second.first, in.second.second) << ";\n";
            for (auto &ou : c.outputs)
                if (ou.second.first != D3DDECLUSAGE_POSITION && ou.second.first != D3DDECLUSAGE_POSITIONT)
                    o << "out vec4 " << varyingName(ou.second.first, ou.second.second) << ";\n";
        }
    } else {
        o << "#version 120\n";
        if (c.isPixel) {
            for (auto &in : c.inputs)  o << "varying vec4 " << varyingName(in.second.first, in.second.second) << ";\n";
            for (int sN : c.samplers)
            {
                int dim = c.samplerDim.count(sN) ? c.samplerDim.at(sN) : 2;
                const char *ty = ((c.shadowMask >> sN) & 1) ? "sampler2DShadow"
                                 : dim == 4 ? "sampler3D" : dim == 3 ? "samplerCube" : "sampler2D";
                o << "uniform " << ty << " s" << sN << ";\n";
            }
            if (c.shadowMask)
                o << "vec3 KB_sc(vec4 c) { return c.xyz / c.w; }\n";
        } else {
            for (auto &in : c.inputs)  o << "attribute vec4 " << GLAttribName(in.second.first, in.second.second) << ";\n";
            for (auto &ou : c.outputs)
                if (ou.second.first != D3DDECLUSAGE_POSITION && ou.second.first != D3DDECLUSAGE_POSITIONT)
                    o << "varying vec4 " << varyingName(ou.second.first, ou.second.second) << ";\n";
        }
    }
    // Size the constant array to the highest register the shader references (not a blanket
    // 256), so the device uploads only that many vec4s per draw. Relative addressing
    // (c[a0.x+N]) forces maxConst to 255 — the runtime index can reach any register.
    if (c.usedConst) o << "uniform vec4 " << c.cArr() << "[" << (c.maxConst + 1) << "];\n";
    for (auto &d : c.defs)
        o << "const vec4 c" << d.first << "_def = vec4(" << d.second[0] << ", " << d.second[1]
          << ", " << d.second[2] << ", " << d.second[3] << ");\n";
    // Depth-pre-pass invariance: COD draws world/model geometry in a depth PRE-PASS, then
    // re-draws color with an equal/<= depth test — which requires the SAME vertex to produce
    // the EXACT same gl_Position.z in both passes. D3D9 guarantees that ("position
    // invariance"); GLSL does NOT unless gl_Position is declared invariant, so the prepass
    // and color shaders' depths drift by a ULP and surfaces fail the depth test and drop out
    // (heavy world-geometry flicker). Declaring it invariant forces the
    // compiler to compute it identically across shaders/passes.
    if (!c.isPixel) o << "invariant gl_Position;\n";
    if (!c.isPixel) o << "uniform vec4 kbPosFixup;\n";
    o << "void main() {\n";
    for (int r : c.usedTemps) o << "  vec4 r" << r << " = vec4(0.0);\n";
    if (c.usedA0) o << "  ivec4 a0 = ivec4(0);\n";
    o << body.str();
    // D3D9 -> GL clip-space depth fixup. D3D9 vertex shaders emit clip-space z in [0,w]
    // (post-divide NDC z in [0,1]); OpenGL expects [-w,w] (NDC z in [-1,1]). GL 4.5's
    // glClipControl(GL_ZERO_TO_ONE) would handle it, but macOS stops at GL 4.1 (and
    // Linux's compat path at 2.1-era GLSL), so we remap in the shader. Without
    // it, every depth value is compressed into the far half [0.5,1.0] of the depth buffer
    // -> half the precision is wasted and distant coplanar surfaces z-fight (buildings
    // flickering in and out). z' = 2z - w maps NDC [0,1] -> [-1,1] exactly.
    if (!c.isPixel)
        o << "  gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n";
    // Render with D3D's row order: negating clip-space Y puts D3D's top row at GL row 0
    // in every target, so render targets sample the right way up with D3D texture
    // coordinates, viewports/scissors/blits use D3D rects unchanged, and gl_FragCoord.y
    // counts from the top like vPos. Present flips the back buffer onto the window.
    // D3D9 samples pixel centres at integer window coordinates, GL at +0.5: shift
    // by half a pixel right and down so coverage and texel alignment match (an
    // unshifted full-screen pass resamples between texels and moves the image half a
    // pixel). kbPosFixup = (1/viewport width, 1/viewport height).
    if (!c.isPixel)
        o << "  gl_Position.y = -gl_Position.y;\n"
          << "  gl_Position.xy += kbPosFixup.xy * gl_Position.w;\n";
    // ES alpha test: D3D's fixed-function cutout, done in-shader. D3DCMP_*:
    // 1=NEVER 2=LESS 3=EQUAL 4=LEQUAL 5=GREATER 6=NOTEQUAL 7=GEQUAL 8=ALWAYS.
    // Discard when the test FAILS. uAlphaTestFunc==0 disables it.
    if (c.emitES && c.isPixel) {
        const char *a = c.fragColorName();
        o << "  if (uAlphaTestFunc != 0) {\n"
          << "    float aT = " << a << ".a; bool passA = true;\n"
          << "    if      (uAlphaTestFunc == 1) passA = false;\n"
          << "    else if (uAlphaTestFunc == 2) passA = (aT <  uAlphaRef);\n"
          << "    else if (uAlphaTestFunc == 3) passA = (aT == uAlphaRef);\n"
          << "    else if (uAlphaTestFunc == 4) passA = (aT <= uAlphaRef);\n"
          << "    else if (uAlphaTestFunc == 5) passA = (aT >  uAlphaRef);\n"
          << "    else if (uAlphaTestFunc == 6) passA = (aT != uAlphaRef);\n"
          << "    else if (uAlphaTestFunc == 7) passA = (aT >= uAlphaRef);\n"
          << "    if (!passA) discard;\n"
          << "  }\n";
    }
    o << "}\n";
    if (outSamplerMask) {
        unsigned m = 0;
        for (int sN : c.samplers) if (sN >= 0 && sN < 32) m |= 1u << sN;
        *outSamplerMask = m;
    }
    out = o.str();
    // KB_DUMPENV=1: print cube-sampling PIXEL shaders (the envmap/specular users) with the
    // ORIGINAL D3D9 disassembly side-by-side, so the term math can be diffed op-by-op
    // against the translation. world=1 marks lightmap-sampling (lit-world) techniques —
    // the mirror-surfaces class; up to 2 model + 2 world shaders per run.
    if (KB_DumpEnvWanted() && c.isPixel) {
        bool hasCube = false;
        for (auto &kv : c.samplerDim) if (kv.second == 3) hasCube = true;
        bool isWorld = c.samplers.count(12) != 0;   // s12 = primary lightmap
        static int dumpedModel = 0, dumpedWorld = 0;
        int &slot = isWorld ? dumpedWorld : dumpedModel;
        if (hasCube && slot < 2) {
            ++slot;
            fprintf(stderr, "[gl] ===== ENVMAP PS DUMP %s%d world=%d =====\n"
                            "[gl] --- d3d9 asm ---\n%s"
                            "[gl] --- glsl ---\n%s\n[gl] ===== END DUMP =====\n",
                    isWorld ? "W" : "M", slot, isWorld ? 1 : 0,
                    sm3::Disassemble(sh).c_str(), out.c_str());
        }
    }
    return true;
}
// ---- GL shader objects ----------------------------------------------------

static unsigned compileGL(GLenum stage, const std::string &src, const char *label) {
    // Diagnostic: dump every translated shader to $KB_DUMP_GLSL/<n>.<vert|frag>.
    if (const char *dir = getenv("KB_DUMP_GLSL")) {
        static int n = 0;
        char path[512];
        snprintf(path, sizeof(path), "%s/%04d.%s", dir, n++, stage == GL_VERTEX_SHADER ? "vert" : "frag");
        if (FILE *f = fopen(path, "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
    }
    unsigned s = glCreateShader(stage);
    if (!s) {
        // glCreateShader=0 = NO GL CONTEXT on this thread (or context lost) — never a
        // GLSL problem. (Pre-B38 this happened for loader-thread creates; now deferred.)
        fprintf(stderr, "[gl] %s: glCreateShader returned 0 (no context on this thread / context lost)\n", label);
        return 0;
    }
    while (glGetError() != GL_NO_ERROR) {}  // drain stale errors so the report below is ours
    const std::string glsl = KB_GLSLForContext(src.c_str());
    const char *p = glsl.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        log[0] = 0;
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        if (!log[0]) {
            // status=0 with an EMPTY log is treated as success (on the WebGL target it
            // meant "still compiling"); a real failure shows up when the program links.
            return s;
        }
        fprintf(stderr, "[gl] %s translate/compile failed (shader=%u glErr=0x%x):\n%s\nGLSL:\n%s\n",
                label, s, glGetError(), log, src.c_str());
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// KB_DUMP_SHADERS=<dir>: each shader's bytecode as <dir>/<hash>.vs or .ps, once.
static void KB_DumpShader(const DWORD *t, unsigned hash, bool pixel) {
    static const char *dir = getenv("KB_DUMP_SHADERS");
    if (!dir) return;
    size_t n = 1;
    while (t[n] != 0x0000FFFFu)
        n += (t[n] & 0xFFFFu) == 0xFFFEu ? ((t[n] >> 16) & 0x7FFF) + 1 : 1;
    char path[512];
    snprintf(path, sizeof(path), "%s/%08x.%s", dir, hash, pixel ? "ps" : "vs");
    if (FILE *f = fopen(path, "rb")) { fclose(f); return; }
    if (FILE *f = fopen(path, "wb")) { fwrite(t, sizeof(DWORD), n + 1, f); fclose(f); }
}

// Constructors only TRANSLATE (pure CPU, any thread). The GL compile happens lazily
// in glShader() on the thread that links/draws, which is the one with the GL context
// current — see the note in gl_shader.h (loader threads have no GL context).
GLVertexShader::GLVertexShader(IDirect3DDevice9 *device, const DWORD *function) : device_(device) {
    hash = KB_ShaderHash(function);
    KB_SPEC("CreateVertexShader %s", KB_SpecThread());
    KB_DumpShader(function, hash, false);
    bool isPixel = false;
    translatedOk_ = TranslateD3D9Shader(function, glsl_, &isPixel) && !isPixel;
}
// Failed compiles RETRY with a cooldown instead of latching (a transient failure must not
// leave a material permanently black). Bounded: 8 tries, 30 presents apart — a real GLSL
// error just prints a few times and stops.
extern unsigned long g_kbPresentEnter;
static bool kbCompileGate(unsigned shader, int &tries, unsigned long &lastPres) {
    if (shader) return false;
    if (tries == 0) return true;
    if (tries >= 8) return false;
    return g_kbPresentEnter - lastPres >= 30;
}

unsigned GLVertexShader::glShader() {
    if (translatedOk_ && kbCompileGate(shader_, tries_, lastTryPres_)) {
        lastTryPres_ = g_kbPresentEnter;
        shader_ = compileGL(GL_VERTEX_SHADER, glsl_, "vertex shader");
        tries_ = shader_ ? 0 : tries_ + 1;
    }
    return shader_;
}
GLVertexShader::~GLVertexShader() { if (shader_) glDeleteShader(shader_); }
HRESULT WINAPI GLVertexShader::GetDevice(IDirect3DDevice9 **pp) { if (!pp) return E_INVALIDARG; *pp = device_; if (device_) device_->AddRef(); return D3D_OK; }

// Walk the token stream to its END marker (0x0000FFFF), skipping comment blocks, so the
// bytecode can be copied for shadow-variant retranslation.
static size_t shaderTokenLen(const DWORD *t) {
    if (!t) return 0;
    size_t n = 1;   // version token
    while (t[n] != 0x0000FFFFu) {
        if ((t[n] & 0xFFFFu) == 0xFFFEu) n += ((t[n] >> 16) & 0x7FFF) + 1;  // comment block
        else ++n;
        if (n > (1u << 20)) return 0;   // runaway guard: malformed stream
    }
    return n + 1;
}

GLPixelShader::GLPixelShader(IDirect3DDevice9 *device, const DWORD *function) : device_(device) {
    hash = KB_ShaderHash(function);
    KB_SPEC("CreatePixelShader %s", KB_SpecThread());
    KB_DumpShader(function, hash, true);
    bool isPixel = false;
    translatedOk_ = TranslateD3D9Shader(function, glsl_, &isPixel, 0, &samplerMask_) && isPixel;
    if (translatedOk_) {
        size_t n = shaderTokenLen(function);
        if (n) func_.assign(function, function + n);
    }
}
unsigned GLPixelShader::glShader(unsigned shadowMask) {
    shadowMask &= samplerMask_;          // only samplers this shader actually reads
    if (shadowMask == 0 || func_.empty()) {
        if (translatedOk_ && kbCompileGate(shader_, tries_, lastTryPres_)) {
            lastTryPres_ = g_kbPresentEnter;
            shader_ = compileGL(GL_FRAGMENT_SHADER, glsl_, "pixel shader");
            tries_ = shader_ ? 0 : tries_ + 1;
        }
        return shader_;
    }
    Variant &v = variants_[shadowMask];
    if (kbCompileGate(v.gl, v.tries, v.lastPres)) {
        v.lastPres = g_kbPresentEnter;
        // Retranslate with the masked samplers typed sampler2DShadow (texldp becomes the
        // depth-compared textureProj — D3D9 hardware-shadow semantics) and compile.
        std::string glsl; bool isPix = false;
        if (TranslateD3D9Shader(func_.data(), glsl, &isPix, shadowMask) && isPix)
            v.gl = compileGL(GL_FRAGMENT_SHADER, glsl, "pixel shader (shadow variant)");
        v.tries = v.gl ? 0 : v.tries + 1;
    }
    return v.gl;
}
GLPixelShader::~GLPixelShader() { if (shader_) glDeleteShader(shader_); }
HRESULT WINAPI GLPixelShader::GetDevice(IDirect3DDevice9 **pp) { if (!pp) return E_INVALIDARG; *pp = device_; if (device_) device_->AddRef(); return D3D_OK; }
