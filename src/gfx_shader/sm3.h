// sm3.h — D3D9 shader bytecode (vs/ps 2.0-3.0), parsed once for both translators.
//
// The game ships compiled D3D9 shaders in its fastfiles. sm3::Parse decodes the token
// stream into declarations, constants and instructions with their operands; the GL
// backend emits GLSL from that (gl_shader.cpp) and the Metal backend MSL
// (gfx_mtl/mtl_msl.cpp). Token layout: the D3D9 driver documentation ("Shader Code
// Format"), as Wine's d3d9 parser reads it.
#ifndef KISAK_SM3_H
#define KISAK_SM3_H

#include <array>
#include <map>
#include <string>
#include <vector>

namespace sm3 {

enum Op {   // D3DSIO_*
    OP_NOP = 0, OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_RSQ = 7,
    OP_DP3 = 8, OP_DP4 = 9, OP_MIN = 10, OP_MAX = 11, OP_SLT = 12, OP_SGE = 13, OP_EXP = 14, OP_LOG = 15,
    OP_LIT = 16, OP_DST = 17, OP_LRP = 18, OP_FRC = 19, OP_M4x4 = 20, OP_M4x3 = 21, OP_M3x4 = 22,
    OP_M3x3 = 23, OP_M3x2 = 24, OP_CALL = 25, OP_CALLNZ = 26, OP_LOOP = 27, OP_RET = 28, OP_ENDLOOP = 29,
    OP_LABEL = 30, OP_DCL = 31, OP_POW = 32, OP_CRS = 33, OP_SGN = 34, OP_ABS = 35, OP_NRM = 36,
    OP_SINCOS = 37, OP_REP = 38, OP_ENDREP = 39, OP_IF = 40, OP_IFC = 41, OP_ELSE = 42, OP_ENDIF = 43,
    OP_BREAK = 44, OP_BREAKC = 45, OP_MOVA = 46, OP_DEFB = 47, OP_DEFI = 48,
    OP_TEXCOORD = 64, OP_TEXKILL = 65, OP_TEX = 66, OP_DEF = 81, OP_CMP = 88, OP_DP2ADD = 90,
    OP_DSX = 91, OP_DSY = 92, OP_TEXLDD = 93, OP_SETP = 94, OP_TEXLDL = 95, OP_BREAKP = 96,
    OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
};
// texld's flavour in the instruction's control bits (OP_TEX).
enum { TEX_PLAIN = 0, TEX_PROJECT = 1, TEX_BIAS = 2 };

enum RegType {   // D3DSPR_*
    RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR = 3, RT_TEXTURE = 3, RT_RASTOUT = 4, RT_ATTROUT = 5,
    RT_OUTPUT = 6, RT_CONSTINT = 7, RT_COLOROUT = 8, RT_DEPTHOUT = 9, RT_SAMPLER = 10, RT_CONST2 = 11,
    RT_CONST3 = 12, RT_CONST4 = 13, RT_CONSTBOOL = 14, RT_LOOP = 15, RT_TEMPFLOAT16 = 16, RT_MISCTYPE = 17,
    RT_LABEL = 18, RT_PREDICATE = 19,
};

// D3DSPSM_* source modifiers.
enum { MOD_NONE = 0, MOD_NEG, MOD_BIAS, MOD_BIASNEG, MOD_SIGN, MOD_SIGNNEG, MOD_COMP, MOD_X2, MOD_X2NEG,
       MOD_DZ, MOD_DW, MOD_ABS, MOD_ABSNEG, MOD_NOT };
// Destination modifier bits.
enum { DMOD_SAT = 1, DMOD_PP = 2, DMOD_CENTROID = 4 };
// Sampler texture types in a sampler dcl (D3DSTT_*).
enum { DIM_UNKNOWN = 0, DIM_2D = 2, DIM_CUBE = 3, DIM_VOLUME = 4 };

struct Src {
    int  type = 0, reg = 0;
    int  swizzle = 0xE4;      // 2 bits per component, xyzw = 0xE4
    int  mod = MOD_NONE;
    bool rel = false;         // reg[relType relReg . relComp]: a0 or aL
    int  relType = 0, relReg = 0, relComp = 0;
};

struct Dst {
    int  type = 0, reg = 0;
    int  mask = 0xF;          // write mask, bit 0 = x
    int  mod = 0;             // DMOD_*
    int  shift = 0;           // result shift (ps 1.x only)
    bool rel = false;
};

struct Instr {
    int   op = OP_NOP;
    int   ctrl = 0;           // comparison (ifc, breakc, setp) or texld flavour
    bool  hasDst = false;
    Dst   dst;
    bool  predicated = false;
    Src   pred;
    int   nsrc = 0;
    Src   src[4];
};

struct Decl {
    int type = 0, reg = 0;
    int usage = 0, index = 0; // D3DDECLUSAGE_* and usage index (inputs and outputs)
    int mask = 0xF;
    int mod = 0;              // DMOD_CENTROID, DMOD_PP
    int dim = DIM_UNKNOWN;    // samplers
};

struct Shader {
    bool pixel = false;
    int  major = 0, minor = 0;
    std::vector<Decl> decls;
    std::map<int, std::array<float, 4>> defs;   // def c#
    std::map<int, std::array<int, 4>>   idefs;  // defi i#
    std::map<int, bool>                 bdefs;  // defb b#
    std::vector<Instr> code;                    // everything but dcl, def and comments

    // The sampler dcl for s<reg>, or nullptr.
    const Decl *sampler(int reg) const;
};

// Decode a token stream (version token first, up to the end token). On a malformed
// stream returns false with the reason in `error`.
bool Parse(const unsigned int *tokens, Shader &out, std::string &error);

// Words in the stream including the end token, or 0 if malformed.
size_t TokenCount(const unsigned int *tokens);

// D3D9 assembly of the parsed shader, one instruction per line (for dumps and test
// output; the translators never use it).
std::string Disassemble(const Shader &s);
const char *OpName(int op);

} // namespace sm3

#endif // KISAK_SM3_H
