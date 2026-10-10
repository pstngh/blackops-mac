// mtl_msl.h — D3D9 shader bytecode (sm3.h) to Metal Shading Language.
//
// One MSL translation unit per D3D9 shader, entry point "kb_main" (or "kb_<prefix>"
// with a prefix, so a test can put many into one file). D3D9 semantics kept:
//   * clip space is Metal's (depth 0..1, +y up; targets and textures top row first),
//     so positions pass through, plus D3D9's half-pixel offset per viewport size;
//   * vPos is the integer pixel position, vFace +1/-1 (front face clockwise);
//   * rsq, log and pow take |x|, as the D3D9 instructions do;
//   * the alpha test (fixed-function in D3D9) is a function constant;
//   * a 2D pixel sampler whose bit is set in the per-draw mask (a depth texture is
//     bound) reads it as D3D9's hardware shadow maps do: texldl and texldp lookups
//     compare the coordinate's z (texldp: z/w) with the stored depths, filtered
//     bilinearly (percentage closer, kb_pcf in the prelude); compareMask lists the
//     samplers read only that way;
//   * relative constant reads see the shader's def'd constants;
//   * `perSample` makes a pixel shader run per sample (it reads [[sample_id]], and
//     its inputs are interpolated at the sample): the transparency supersampling of
//     alpha-tested draws (r_aaAlpha).
// Bindings (shared with mtl_device.mm):
//   vertex buffers 0..3 = D3D streams, buffer 20 = constants c#, buffer 21 = the
//   per-draw values (vertex: half-pixel offset; pixel: x the alpha reference, y the
//   bits of the samplers holding a depth texture), texture and sampler N = sampler sN
//   (D3DVERTEXTEXTURESAMPLERn = vertex sampler n).
// Anything outside what is implemented makes the translation fail with a reason.
#ifndef KISAK_MTL_MSL_H
#define KISAK_MTL_MSL_H

#include <string>

namespace sm3 { struct Shader; }

enum {
    MSL_BUF_CONSTANTS = 20,
    MSL_BUF_DRAW      = 21,
    MSL_BUF_DEFAULTS  = 22,   // constant attribute values for inputs a declaration lacks
    MSL_FC_ALPHAFUNC  = 0,    // function constant: D3DCMP_* of the alpha test, 0 = off
    MSL_MAX_ATTRIBS   = 20,
};

// Vertex attribute index for a D3DDECLUSAGE and usage index, or -1.
int MSL_AttribIndex(int usage, int usageIndex);

struct MSLShader {
    std::string source;
    std::string entry;
    unsigned attribMask = 0;      // vertex: attribute indices read
    unsigned samplerMask = 0;     // samplers sampled
    unsigned compareMask = 0;     // pixel: samplers whose lookups all can read a shadow map
    unsigned char samplerDim[16] = {};   // sm3::DIM_* per sampler
    int constCount = 0;           // float4 constants read (from c0)
    unsigned long long varyingsOut = 0;  // vertex: varying slots written (see MSL_Varying)
    unsigned long long varyingsIn = 0;   // pixel: varying slots read
    bool writesDepth = false;
    unsigned colorOutMask = 0;    // pixel: oC# written
    std::string error;            // set on failure
};

// What every translation unit starts with (once per file).
const char *MSL_Prelude();

// Translate. `prefix` names the entry kb_<prefix> and its structs (empty: kb_main);
// the source does not include MSL_Prelude().
bool MSL_Translate(const sm3::Shader &sh, MSLShader &out, const std::string &prefix = std::string(),
                   bool perSample = false);

#endif // KISAK_MTL_MSL_H
