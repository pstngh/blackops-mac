// gl_d3d9_draw.cpp — geometry resource creation + the draw path for GLDevice.
//
// The built-in GLSL program here only covers the *pre-transformed* (D3DFVF_XYZRHW /
// D3DDECLUSAGE_POSITIONT) vertex path — i.e. the 2D / fixed-function geometry the
// engine submits in screen space. Programmable 3D shaders go through the DX9
// bytecode→GLSL translator instead (task #5); when a vertex shader is bound this
// built-in is bypassed.
#include "gl_d3d9.h"
#include "gl_platform.h"
#include "gl_resources.h"
#include "gl_shader.h"   // GLAttribLocation / GLAttribName / GLBindAttribLocations
#include "gl_spec.h"

#include <GL/glew.h>
#include <cstdio>
#include <vector>
#include <mutex>
#include <cstring>
#include <unordered_map>
#include <cstdint>

namespace {

// The built-in 2D program consumes the canonical POSITION / COLOR0 / TEXCOORD0
// attribute names (see GLAttribName in gl_shader.cpp), so the same vertex setup
// drives it and the translated programs.
#if   defined(KB_GL_CORE)
// The desktop built-in (colour op + alpha test) in the in/out dialect.
const char *kBuiltinVS =
    "#version 300 es\n"
    "in vec4 aPos;\n"               // POSITIONT: x,y in screen pixels, z depth, w=rhw
    "in vec4 aColor0;\n"
    "in vec2 aTexCoord0;\n"
    "uniform vec2 uViewport;\n"
    "out vec4 vColor;\n"
    "out vec2 vTexCoord;\n"
    "void main() {\n"
    "  float x = ((aPos.x + 0.5) / uViewport.x) * 2.0 - 1.0;\n"   // D3D9 pixel centres are at integers
    "  float y = ((aPos.y + 0.5) / uViewport.y) * 2.0 - 1.0;\n"  // D3D row order (see gl_shader.cpp)
    "  gl_Position = vec4(x, y, aPos.z, 1.0);\n"
    "  vColor = aColor0;\n"
    "  vTexCoord = aTexCoord0;\n"
    "}\n";

const char *kBuiltinFS =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D uTex;\n"
    "uniform int uUseTexture;\n"
    "uniform int uColorOp;\n"    // 0 = SELECTARG1 (texture only), 1 = MODULATE (tex*diffuse)
    "uniform int uAlphaFunc;\n"  // GL compare func (GL_NEVER..GL_ALWAYS), 0 = alpha test off
    "uniform float uAlphaRef;\n" // reference in [0,1]
    "in vec4 vColor;\n"
    "in vec2 vTexCoord;\n"
    "out vec4 oColor;\n"
    "bool alphaPass(float a) {\n"
    "  if (uAlphaFunc == 0) return true;\n"
    "  if (uAlphaFunc == 0x0200) return false;\n"
    "  if (uAlphaFunc == 0x0201) return a <  uAlphaRef;\n"
    "  if (uAlphaFunc == 0x0202) return a == uAlphaRef;\n"
    "  if (uAlphaFunc == 0x0203) return a <= uAlphaRef;\n"
    "  if (uAlphaFunc == 0x0204) return a >  uAlphaRef;\n"
    "  if (uAlphaFunc == 0x0205) return a != uAlphaRef;\n"
    "  if (uAlphaFunc == 0x0206) return a >= uAlphaRef;\n"
    "  return true;\n"
    "}\n"
    "void main() {\n"
    "  vec4 c = vColor;\n"
    "  if (uUseTexture != 0) {\n"
    "    vec4 t = texture(uTex, vTexCoord);\n"
    "    c = (uColorOp == 0) ? t : (t * vColor);\n"
    "  }\n"
    "  if (!alphaPass(c.a)) discard;\n"
    "  oColor = c;\n"
    "}\n";
#else
const char *kBuiltinVS =
    "#version 120\n"
    "attribute vec4 aPos;\n"        // POSITIONT: x,y in screen pixels, z depth, w=rhw
    "attribute vec4 aColor0;\n"
    "attribute vec2 aTexCoord0;\n"
    "uniform vec2 uViewport;\n"
    "varying vec4 vColor;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "  float x = ((aPos.x + 0.5) / uViewport.x) * 2.0 - 1.0;\n"   // D3D9 pixel centres are at integers
    "  float y = ((aPos.y + 0.5) / uViewport.y) * 2.0 - 1.0;\n"  // D3D row order (see gl_shader.cpp)
    "  gl_Position = vec4(x, y, aPos.z, 1.0);\n"
    "  vColor = aColor0;\n"
    "  vTexCoord = aTexCoord0;\n"
    "}\n";

const char *kBuiltinFS =
    "#version 120\n"
    "uniform sampler2D uTex;\n"
    "uniform int uUseTexture;\n"
    "uniform int uColorOp;\n"    // 0 = SELECTARG1 (texture only), 1 = MODULATE (tex*diffuse)
    "uniform int uAlphaFunc;\n"  // GL compare func (GL_NEVER..GL_ALWAYS), 0 = alpha test off
    "uniform float uAlphaRef;\n" // reference in [0,1]
    "varying vec4 vColor;\n"
    "varying vec2 vTexCoord;\n"
    "bool alphaPass(float a) {\n"
    "  if (uAlphaFunc == 0) return true;\n"            // disabled
    "  if (uAlphaFunc == 0x0200) return false;\n"      // GL_NEVER
    "  if (uAlphaFunc == 0x0201) return a <  uAlphaRef;\n"  // GL_LESS
    "  if (uAlphaFunc == 0x0202) return a == uAlphaRef;\n"  // GL_EQUAL
    "  if (uAlphaFunc == 0x0203) return a <= uAlphaRef;\n"  // GL_LEQUAL
    "  if (uAlphaFunc == 0x0204) return a >  uAlphaRef;\n"  // GL_GREATER
    "  if (uAlphaFunc == 0x0205) return a != uAlphaRef;\n"  // GL_NOTEQUAL
    "  if (uAlphaFunc == 0x0206) return a >= uAlphaRef;\n"  // GL_GEQUAL
    "  return true;\n"                                  // GL_ALWAYS (0x0207) / default
    "}\n"
    "void main() {\n"
    "  vec4 c = vColor;\n"
    "  if (uUseTexture != 0) {\n"
    "    vec4 t = texture2D(uTex, vTexCoord);\n"
    "    c = (uColorOp == 0) ? t : (t * vColor);\n"  // SELECTARG1(tex) vs MODULATE(tex*diffuse)
    "  }\n"
    "  if (!alphaPass(c.a)) discard;\n"
    "  gl_FragColor = c;\n"
    "}\n";
#endif

unsigned compile(GLenum stage, const char *src) {
    unsigned s = glCreateShader(stage);
    const std::string glsl = KB_GLSLForContext(src);
    const char *p = glsl.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        log[0] = 0;
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        // An empty log is not a real failure (see gl_shader.cpp compileGL); stay quiet.
        if (log[0])
            fprintf(stderr, "[gl] built-in shader compile failed: %s\n", log);
    }
    return s;
}

// D3DDECLTYPE -> (component count, GL type, normalized). CoD's vertex formats
// pack normals/tangents (UBYTE4N/DEC3N), colours (UBYTE4N) and texcoords
// (FLOAT16_2/4) — decoding any of these as plain floats yields garbage normals
// (dark/flat lighting, half-black triangles) and garbage UVs (skybox moire).
void declType(BYTE t, GLint *size, GLenum *type, GLboolean *norm) {
    *norm = GL_FALSE;
    switch (t) {
        case D3DDECLTYPE_FLOAT1:    *size = 1; *type = GL_FLOAT;          break;
        case D3DDECLTYPE_FLOAT2:    *size = 2; *type = GL_FLOAT;          break;
        case D3DDECLTYPE_FLOAT3:    *size = 3; *type = GL_FLOAT;          break;
        case D3DDECLTYPE_FLOAT4:    *size = 4; *type = GL_FLOAT;          break;
        case D3DDECLTYPE_D3DCOLOR:  *size = GL_BGRA; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; break;
        case D3DDECLTYPE_UBYTE4:    *size = 4; *type = GL_UNSIGNED_BYTE;  break;
        case D3DDECLTYPE_UBYTE4N:   *size = 4; *type = GL_UNSIGNED_BYTE;  *norm = GL_TRUE; break;
        case D3DDECLTYPE_SHORT2:    *size = 2; *type = GL_SHORT;          break;
        case D3DDECLTYPE_SHORT4:    *size = 4; *type = GL_SHORT;          break;
        case D3DDECLTYPE_SHORT2N:   *size = 2; *type = GL_SHORT;          *norm = GL_TRUE; break;
        case D3DDECLTYPE_SHORT4N:   *size = 4; *type = GL_SHORT;          *norm = GL_TRUE; break;
        case D3DDECLTYPE_USHORT2N:  *size = 2; *type = GL_UNSIGNED_SHORT; *norm = GL_TRUE; break;
        case D3DDECLTYPE_USHORT4N:  *size = 4; *type = GL_UNSIGNED_SHORT; *norm = GL_TRUE; break;
        // 3 components packed 10:10:10:2 (low bits = x). REV matches D3D's order.
        case D3DDECLTYPE_UDEC3:     *size = 4; *type = GL_UNSIGNED_INT_2_10_10_10_REV; break;
        case D3DDECLTYPE_DEC3N:     *size = 4; *type = GL_INT_2_10_10_10_REV; *norm = GL_TRUE; break;
        case D3DDECLTYPE_FLOAT16_2: *size = 2; *type = GL_HALF_FLOAT;     break;
        case D3DDECLTYPE_FLOAT16_4: *size = 4; *type = GL_HALF_FLOAT;     break;
        default:                    *size = 4; *type = GL_FLOAT;          break;
    }
}


void primInfo(D3DPRIMITIVETYPE pt, UINT primCount, GLenum *mode, GLsizei *verts) {
    switch (pt) {
        case D3DPT_POINTLIST:     *mode = GL_POINTS;         *verts = primCount;     break;
        case D3DPT_LINELIST:      *mode = GL_LINES;          *verts = primCount * 2; break;
        case D3DPT_LINESTRIP:     *mode = GL_LINE_STRIP;     *verts = primCount + 1; break;
        case D3DPT_TRIANGLELIST:  *mode = GL_TRIANGLES;      *verts = primCount * 3; break;
        case D3DPT_TRIANGLESTRIP: *mode = GL_TRIANGLE_STRIP; *verts = primCount + 2; break;
        case D3DPT_TRIANGLEFAN:   *mode = GL_TRIANGLE_FAN;   *verts = primCount + 2; break;
        default:                  *mode = GL_TRIANGLES;      *verts = primCount * 3; break;
    }
}

} // namespace

// ---- Resource creation ----------------------------------------------------
HRESULT WINAPI GLDevice::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage,
                                       D3DFORMAT Format, D3DPOOL Pool,
                                       IDirect3DTexture9 **ppTexture, HANDLE *) {
    if (!ppTexture) return E_INVALIDARG;
    KB_SPEC("CreateTexture %s usage=%x fmt=%u pool=%u levels=%s pow2=%d", KB_SpecThread(), (unsigned)Usage, (unsigned)Format,
            (unsigned)Pool, Levels == 0 ? "0" : Levels == 1 ? "1" : "n", (int)(!(Width & (Width - 1)) && !(Height & (Height - 1))));
    *ppTexture = new GLTexture(this, Width, Height, Levels, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
                                             D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *) {
    if (!ppVolumeTexture) return E_INVALIDARG;
    KB_SPEC("CreateVolumeTexture %s usage=%x fmt=%u pool=%u levels=%u", KB_SpecThread(), (unsigned)Usage, (unsigned)Format,
            (unsigned)Pool, Levels);
    *ppVolumeTexture = new GLVolumeTexture(this, Width, Height, Depth, Levels, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format,
                                           D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *) {
    if (!ppCubeTexture) return E_INVALIDARG;
    KB_SPEC("CreateCubeTexture %s usage=%x fmt=%u pool=%u levels=%s", KB_SpecThread(), (unsigned)Usage, (unsigned)Format,
            (unsigned)Pool, Levels == 0 ? "0" : Levels == 1 ? "1" : "n");
    *ppCubeTexture = new GLCubeTexture(this, EdgeLength, Levels, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                            IDirect3DVertexBuffer9 **ppVB, HANDLE *) {
    if (!ppVB) return E_INVALIDARG;
    KB_SPEC("CreateVertexBuffer %s usage=%x fvf=%x pool=%u", KB_SpecThread(), (unsigned)Usage, (unsigned)FVF, (unsigned)Pool);
    *ppVB = new GLVertexBuffer(this, Length, Usage, FVF, Pool);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                           IDirect3DIndexBuffer9 **ppIB, HANDLE *) {
    if (!ppIB) return E_INVALIDARG;
    KB_SPEC("CreateIndexBuffer %s usage=%x fmt=%u pool=%u", KB_SpecThread(), (unsigned)Usage, (unsigned)Format, (unsigned)Pool);
    *ppIB = new GLIndexBuffer(this, Length, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pElements,
                                                 IDirect3DVertexDeclaration9 **ppDecl) {
    if (!ppDecl || !pElements) return E_INVALIDARG;
    KB_SPEC("CreateVertexDeclaration %s", KB_SpecThread());
    for (const D3DVERTEXELEMENT9 *e = pElements; g_kbSpec && e->Stream != 0xFF; ++e)
        KB_SPEC("DeclElement stream=%u off%%4=%u type=%u method=%u usage=%u index=%u", (unsigned)e->Stream,
                (unsigned)e->Offset % 4, (unsigned)e->Type, (unsigned)e->Method, (unsigned)e->Usage, (unsigned)e->UsageIndex);
    *ppDecl = new GLVertexDeclaration(this, pElements);
    return D3D_OK;
}

// ---- Geometry binding (device holds non-owning refs; the app owns lifetime) -
HRESULT WINAPI GLDevice::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData,
                                         UINT OffsetInBytes, UINT Stride) {
    GLVertexBuffer *vb = static_cast<GLVertexBuffer *>(pStreamData);
    KB_SPEC("SetStreamSource %s stream=%u null=%d off%%4=%u stride=%u dynamic=%d", KB_SpecThread(), StreamNumber, vb == nullptr,
            OffsetInBytes % 4, Stride, vb ? (int)vb->isDynamic() : -1);
    if (StreamNumber >= 4) return D3D_OK;  // TODO: support >4 streams if needed
    Stream &s = streams_[StreamNumber];
    // No-change fast path: the engine re-sets identical bindings around most draws;
    // an unconditional flush here kept draw batches at size 1 (flushes/f == draws/f).
    if (s.vb == vb && s.offset == OffsetInBytes && s.stride == Stride) return D3D_OK;
    s.vb = vb; s.offset = OffsetInBytes; s.stride = Stride;
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetIndices(IDirect3DIndexBuffer9 *pIndexData) {
    GLIndexBuffer *ib = static_cast<GLIndexBuffer *>(pIndexData);
    KB_SPEC("SetIndices %s null=%d fmt=%u dynamic=%d", KB_SpecThread(), ib == nullptr, ib ? (unsigned)ib->format() : 0u,
            ib ? (int)ib->isDynamic() : -1);
    if (ib_ == ib) return D3D_OK;   // no-change fast path
    ib_ = ib;
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) {
    GLVertexDeclaration *d = static_cast<GLVertexDeclaration *>(pDecl);
    KB_SPEC("SetVertexDeclaration %s null=%d", KB_SpecThread(), d == nullptr);
    if (decl_ == d) return D3D_OK;   // no-change fast path
    decl_ = d;
    return D3D_OK;
}

// ---- Draw path ------------------------------------------------------------
void GLDevice::ensureBuiltinProgram() {
    if (builtinProg_) return;
    unsigned vs = compile(GL_VERTEX_SHADER, kBuiltinVS);
    unsigned fs = compile(GL_FRAGMENT_SHADER, kBuiltinFS);
    builtinProg_ = glCreateProgram();
    glAttachShader(builtinProg_, vs);
    glAttachShader(builtinProg_, fs);
    GLBindAttribLocations(builtinProg_);
    glLinkProgram(builtinProg_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    builtinViewportLoc_  = glGetUniformLocation(builtinProg_, "uViewport");
    builtinTexLoc_       = glGetUniformLocation(builtinProg_, "uTex");
    builtinUseTexLoc_    = glGetUniformLocation(builtinProg_, "uUseTexture");
    builtinColorOpLoc_   = glGetUniformLocation(builtinProg_, "uColorOp");
    builtinAlphaFuncLoc_ = glGetUniformLocation(builtinProg_, "uAlphaFunc");
    builtinAlphaRefLoc_  = glGetUniformLocation(builtinProg_, "uAlphaRef");
}

// D3DCMP_* -> the GL compare-func enum (GL_NEVER..GL_ALWAYS). The built-in
// fragment shader emulates the alpha test against these values; 0 means the test
// is disabled.
static GLenum alphaFuncToGL(DWORD d3dCmp) {
    switch (d3dCmp) {
        case D3DCMP_NEVER:        return GL_NEVER;
        case D3DCMP_LESS:         return GL_LESS;
        case D3DCMP_EQUAL:        return GL_EQUAL;
        case D3DCMP_LESSEQUAL:    return GL_LEQUAL;
        case D3DCMP_GREATER:      return GL_GREATER;
        case D3DCMP_NOTEQUAL:     return GL_NOTEQUAL;
        case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
        default:                  return GL_ALWAYS;
    }
}

// Bind the built-in program and set its frame/texture uniforms before a draw.
void GLDevice::bindBuiltinForDraw() {
    ensureBuiltinProgram();
    if (curProgram_ != builtinProg_) { glUseProgram(builtinProg_); curProgram_ = builtinProg_; }
    if (builtinViewportLoc_ >= 0)
        glUniform2f(builtinViewportLoc_, (float)fbWidth_, (float)fbHeight_);
    bool sampling = applyTextures();
    if (builtinTexLoc_ >= 0)    glUniform1i(builtinTexLoc_, 0);
    if (builtinUseTexLoc_ >= 0) glUniform1i(builtinUseTexLoc_, sampling ? 1 : 0);

    // Fixed-function stage-0 combine: SELECTARG1 (texture only) vs MODULATE
    // (texture * diffuse). Both arg slots default to the (TEXTURE, DIFFUSE) pair,
    // which is all the engine's 2D path uses.
    if (builtinColorOpLoc_ >= 0)
        glUniform1i(builtinColorOpLoc_, texStage0_.colorOp == D3DTOP_SELECTARG1 ? 0 : 1);

    // Alpha test (emulated by discard). Pass the GL compare func, or 0 to disable.
    if (builtinAlphaFuncLoc_ >= 0)
        glUniform1i(builtinAlphaFuncLoc_, alphaTest_.enable ? (int)alphaFuncToGL(alphaTest_.func) : 0);
    if (builtinAlphaRefLoc_ >= 0)
        glUniform1f(builtinAlphaRefLoc_, (float)alphaTest_.ref / 255.0f);
}

// Selective VAO-cache invalidation. Wiping the whole cache on every buffer/decl death (the
// old design) made the streaming system's periodic geometry eviction rebuild every VAO at
// once the next frame: a periodic stutter (KB_NOVAO=1, which has no cache to wipe, removed
// it). Instead, record just the dead resource and drop ONLY the
// cache entries that reference it. Destructors (gl_resources.cpp, possibly off-thread) push
// here; applyVertexState drains on the render thread BEFORE any miss-build, so a reused GL
// name can't alias a stale entry (its entries are erased first).
static std::mutex            g_kbDeadMu;
static std::vector<unsigned> g_kbDeadBufs;    // dead VB/IB GL buffer names (one shared namespace)
static std::vector<const void*> g_kbDeadDecls;// dead GLVertexDeclaration addresses
extern "C" void KB_VaoNoteDeadBuf(unsigned name) {
    if (!name) return;
    std::lock_guard<std::mutex> g(g_kbDeadMu); g_kbDeadBufs.push_back(name);
}
extern "C" void KB_VaoNoteDeadDecl(const void *decl) {
    std::lock_guard<std::mutex> g(g_kbDeadMu); g_kbDeadDecls.push_back(decl);
}

void GLDevice::applyVertexState() {
    // KB_NOVAO=1 kill switch: bypass the VAO cache (full attrib re-spec per draw, the
    // pre-B100 path) to bisect geometry corruption vs the cache.
    static int noVao = -1;
    if (noVao < 0) { const char *v = getenv("KB_NOVAO"); noVao = (v && *v == '1') ? 1 : 0; }
    // A D3DUSAGE_DYNAMIC stream's bound offset changes every draw (the NOOVERWRITE/DISCARD ring),
    // so a per-(buffer,offset) cache entry never hits — it just accumulates throwaway VAOs until
    // the size cap nukes the whole cache = the periodic stutter (KB_NOVAO=1 removed
    // it). Route dynamic draws through the shared-VAO re-spec path (no alloc, no cache churn);
    // only static layouts (stable offset) get cached, where hits actually pay off.
    bool dynamic = false;
    for (int i = 0; i < 4; ++i) if (streams_[i].vb && streams_[i].vb->isDynamic()) { dynamic = true; break; }
    if (noVao || dynamic) {
        // Shared re-spec VAO. The old code disabled all 16 attribs + re-enabled + re-bound the
        // buffer PER ELEMENT every draw (~35 GL calls per dynamic draw). But a
        // dynamic stream almost always reuses the SAME decl draw-to-draw — only the per-draw OFFSET
        // changes. So when the decl is unchanged we skip the disables/enables entirely (the shared
        // VAO's enabled set + formats persist) and only re-point; and we bind each buffer once, not
        // per element. Same-decl dynamic draws drop from ~35 calls to ~(1 bind + N point).
        static const void *s_dynLastDecl = nullptr;
        static unsigned    s_dynEnabledMask = 0;
        bool freshVao = !vao_;
        if (freshVao) { glGenVertexArrays(1, &vao_); s_dynLastDecl = nullptr; s_dynEnabledMask = 0; }
        if (curVao_ != vao_) glBindVertexArray(vao_);
        curVao_ = vao_; curVaoEnt_ = nullptr;   // element-bind gate disabled (no entry)
        if (!decl_) return;
        bool sameDecl = !noVao && s_dynLastDecl == (const void *)decl_;
        unsigned newMask = 0;
        for (const D3DVERTEXELEMENT9 &e : decl_->elements()) {
            int loc = GLAttribLocation(e.Usage, e.UsageIndex);
            if (loc >= 0 && loc < 16 && e.Stream < 4 && streams_[e.Stream].vb) newMask |= (1u << loc);
        }
        if (!sameDecl) {
            unsigned dis = s_dynEnabledMask & ~newMask;            // were on, no longer needed
            for (int i = 0; i < 16; ++i) if (dis & (1u << i)) glDisableVertexAttribArray(i);
            glVertexAttrib4f(GLAttribLocation(D3DDECLUSAGE_COLOR, 0), 1.0f, 1.0f, 1.0f, 1.0f);
        }
        unsigned boundBuf = ~0u;
        for (const D3DVERTEXELEMENT9 &e : decl_->elements()) {
            int loc = GLAttribLocation(e.Usage, e.UsageIndex);
            if (loc < 0 || e.Stream >= 4) continue;
            const Stream &s = streams_[e.Stream];
            if (!s.vb) continue;
            GLint size; GLenum type; GLboolean norm;
            declType(e.Type, &size, &type, &norm);
            unsigned bn = s.vb->glName();
            if (bn != boundBuf) { glBindBuffer(GL_ARRAY_BUFFER, bn); boundBuf = bn; }   // once per buffer
            if (!sameDecl || !(s_dynEnabledMask & (1u << loc))) glEnableVertexAttribArray(loc);
            glVertexAttribPointer(loc, size, type, norm, s.stride,
                                  reinterpret_cast<const void *>(size_t(s.offset + e.Offset)));
        }
        s_dynEnabledMask = newMask; s_dynLastDecl = decl_;
        return;
    }

    // Selective invalidation: drop ONLY cache entries that reference a just-destroyed buffer
    // (VB name in key[1/4/7/10] or the captured element binding) or declaration (key[0]). A
    // reused GL name/decl-addr would otherwise alias a stale VAO. Draining here — before the
    // miss-build below — guarantees a reused name's stale entries are gone before a new entry
    // could be created for it. (Was a wholesale clear on any death = the periodic stutter.)
    {
        std::vector<unsigned>     dBufs;
        std::vector<const void *> dDecls;
        {
            std::lock_guard<std::mutex> g(g_kbDeadMu);
            if (!g_kbDeadBufs.empty())  dBufs.swap(g_kbDeadBufs);
            if (!g_kbDeadDecls.empty()) dDecls.swap(g_kbDeadDecls);
        }
        if (!dBufs.empty() || !dDecls.empty()) {
            bool erasedCur = false;
            for (auto it = vaoCache_.begin(); it != vaoCache_.end(); ) {
                const auto &k = it->first;
                bool dead = false;
                for (const void *d : dDecls) if (k[0] == (unsigned)(uintptr_t)d) { dead = true; break; }
                if (!dead)
                    for (unsigned nm : dBufs)
                        if (k[1]==nm || k[4]==nm || k[7]==nm || k[10]==nm || it->second.elem==nm) { dead = true; break; }
                if (dead) {
                    if (curVaoEnt_ == &it->second) erasedCur = true;
                    glDeleteVertexArrays(1, &it->second.vao);
                    it = vaoCache_.erase(it);
                } else ++it;
            }
            if (erasedCur) {
                // The live VAO (and the merge code's s_bElemSlot pointer into it) was erased.
                curVaoEnt_ = nullptr; curVao_ = 0;
                glBindVertexArray(0);
            }
        }
    }
    // Safety cap (rare): bound cache size to avoid unbounded VAO accumulation.
    if (vaoCache_.size() > 4096) {
        for (auto &kv : vaoCache_) glDeleteVertexArrays(1, &kv.second.vao);
        vaoCache_.clear();
        curVaoEnt_ = nullptr; curVao_ = 0;
        glBindVertexArray(0);
    }

    // Cache key: declaration + every stream's (buffer, offset, stride). glName() runs
    // even on hits — pending CPU-side uploads sync at bind time there.
    std::array<unsigned, 13> key{};
    key[0] = (unsigned)(uintptr_t)decl_;
    int n = 1;
    for (int i = 0; i < 4; ++i) {
        const Stream &s = streams_[i];
        key[n++] = s.vb ? s.vb->glName() : 0u;
        key[n++] = (unsigned)s.offset;
        key[n++] = (unsigned)s.stride;
    }
    auto it = vaoCache_.find(key);
    if (it != vaoCache_.end()) {
        if (curVao_ != it->second.vao) { glBindVertexArray(it->second.vao); curVao_ = it->second.vao; }
        curVaoEnt_ = &it->second;
        return;
    }

    // Miss: build a fresh VAO (starts with all arrays disabled — no per-draw disables).
    unsigned vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    curVao_ = vao;
    // D3D's default (unspecified) diffuse colour is white; GL's default generic
    // attribute is (0,0,0,1). Without this, a texcoord-only vertex would multiply
    // the texture by black. Context-global (not VAO state) — re-set per miss is cheap.
    glVertexAttrib4f(GLAttribLocation(D3DDECLUSAGE_COLOR, 0), 1.0f, 1.0f, 1.0f, 1.0f);
    if (decl_) {
        for (const D3DVERTEXELEMENT9 &e : decl_->elements()) {
            int loc = GLAttribLocation(e.Usage, e.UsageIndex);
            if (loc < 0 || e.Stream >= 4) continue;
            const Stream &s = streams_[e.Stream];
            if (!s.vb) continue;

            GLint size; GLenum type; GLboolean norm;
            declType(e.Type, &size, &type, &norm);
            glBindBuffer(GL_ARRAY_BUFFER, s.vb->glName());
            glEnableVertexAttribArray(loc);
            glVertexAttribPointer(loc, size, type, norm, s.stride,
                                  reinterpret_cast<const void *>(size_t(s.offset + e.Offset)));
        }
    }
    VaoEntry &ent = vaoCache_[key];
    ent.vao = vao; ent.elem = 0;   // fresh VAO has no element binding captured yet
    curVaoEnt_ = &ent;
}

// KB_SPEC: what a draw uses: shaders, declaration, streams, targets, bound textures.
void GLDevice::specDraw(const char *kind, unsigned prim, int baseVertex) {
    extern const char *KB_SpecSurfKind(IDirect3DSurface9 *);
    const unsigned vsh = vs_ ? vs_->hash : 0, psh = ps_ ? ps_->hash : 0;
    KB_SpecNote("Draw %s %s prim=%u vs=%d ps=%d decl=%d ib=%u base<0=%d", KB_SpecThread(), kind, prim, vs_ != nullptr,
                ps_ != nullptr, decl_ != nullptr, ib_ ? (unsigned)ib_->format() : 0u, (int)(baseVertex < 0));
    KB_SpecNote("Pair vs=%08x ps=%08x", vsh, psh);
    if (decl_) KB_SpecNote("VsDecl vs=%08x decl=%s", vsh, decl_->specSig.c_str());
    extern unsigned long g_kbPresentEnter;
    for (int i = 0; i < 4; ++i)
        if (streams_[i].vb && streams_[i].vb->isDynamic() && streams_[i].vb->specDiscardPresent != g_kbPresentEnter)
            KB_SpecNote("DrawStream dynamic VB not DISCARDed this frame%s",
                        streams_[i].vb->specDiscardPresent == ~0ul ? " (never)" : "");
    if (ib_ && ib_->isDynamic() && ib_->specDiscardPresent != g_kbPresentEnter)
        KB_SpecNote("Draw dynamic IB not DISCARDed this frame%s", ib_->specDiscardPresent == ~0ul ? " (never)" : "");
    for (int i = 0; i < 4; ++i)
        if (streams_[i].vb) {
            streams_[i].vb->specUsed = true;
            KB_SpecNote("DrawStream %d stride=%u off%%4=%u dynamic=%d", i, streams_[i].stride, streams_[i].offset % 4,
                        (int)streams_[i].vb->isDynamic());
        }
    if (ib_) ib_->specUsed = true;
    KB_SpecNote("DrawTarget rt=%s fmt=%u ds=%s dsfmt=%u bbsamples=%d", KB_SpecSurfKind(specRT_),
                specRT_ ? (unsigned)static_cast<GLSurface *>(specRT_)->format() : 0u, KB_SpecSurfKind(specDS_),
                specDS_ ? (unsigned)static_cast<GLSurface *>(specDS_)->format() : 0u,
                bbSamples_);
    if (ps_)
        for (int i = 0; i < kMaxStages; ++i)
            if (ps_->samplerMask() & (1u << i))
                KB_SpecNote("PsSampler ps=%08x s%d %s fmt=%u", psh, i, specTexKind_[i] ? specTexKind_[i] : "none", specTexFmt_[i]);
    if (alphaTestOn_) KB_SpecNote("DrawAlphaTest func=%u ps=%d", (unsigned)alphaFunc_, ps_ != nullptr);
}

HRESULT WINAPI GLDevice::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                                       UINT PrimitiveCount) {
    extern unsigned long g_kbDraws; ++g_kbDraws;
    if (g_kbSpec) specDraw("DP", (unsigned)PrimitiveType, 0);
    if (!useDrawProgram()) return D3D_OK;   // shader still linking -> skip (pops in next frame)
    applyVertexState();

    GLenum mode; GLsizei verts;
    primInfo(PrimitiveType, PrimitiveCount, &mode, &verts);
    extern int g_kbTrace;
    if (g_kbTrace) fprintf(stderr, "[trace] DP prims=%u fbo=%d cw=%x vs=%d ps=%d tex=%u,%u,%u,%u\n", PrimitiveCount, (int)fboActive_,
                           (unsigned)rsCache_[D3DRS_COLORWRITEENABLE], vs_ ? (int)vs_->ok() : -1, ps_ ? (int)ps_->ok() : -1,
                           boundTexName_[0], boundTexName_[1], boundTexName_[2], boundTexName_[3]);
    glDrawArrays(mode, (GLint)StartVertex, verts);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::DrawIndexedPrimitive(D3DPRIMITIVETYPE Type, INT BaseVertexIndex,
                                              UINT /*MinVertexIndex*/, UINT /*NumVertices*/,
                                              UINT startIndex, UINT primCount) {
    if (g_kbSpec) specDraw("DIP", (unsigned)Type, BaseVertexIndex);
    if (!ib_) return D3D_OK;
    extern unsigned long g_kbDraws; ++g_kbDraws;

    GLenum mode; GLsizei verts;
    primInfo(Type, primCount, &mode, &verts);
    bool is16 = (ib_->format() == D3DFMT_INDEX16);
    GLenum idxType = is16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    size_t idxSize = is16 ? 2 : 4;
    const void *offset = reinterpret_cast<const void *>(size_t(startIndex) * idxSize);

    if (!useDrawProgram()) return D3D_OK;   // shader still linking -> skip (pops in next frame)
    applyVertexState();
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib_->glName());
    extern int g_kbTrace;
    if (g_kbTrace) {
        GLint vp[4] = {}; glGetIntegerv(GL_VIEWPORT, vp);
        GLfloat dr[2] = {}; glGetFloatv(GL_DEPTH_RANGE, dr);
        GLint df = 0, prog = 0, fb = 0; glGetIntegerv(GL_DEPTH_FUNC, &df); glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        GLboolean cm[4] = {}, dm = 0; glGetBooleanv(GL_COLOR_WRITEMASK, cm); glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
        fprintf(stderr, "[trace] GL dt=%d df=%x dm=%d dr=%g..%g cm=%d%d%d%d blend=%d prog=%d fb=%d stencil=%d scissor=%d err=%x\n",
                (int)glIsEnabled(GL_DEPTH_TEST), df, (int)dm, dr[0], dr[1], cm[0], cm[1], cm[2], cm[3],
                (int)glIsEnabled(GL_BLEND), prog, fb, (int)glIsEnabled(GL_STENCIL_TEST), (int)glIsEnabled(GL_SCISSOR_TEST),
                glGetError());
        fprintf(stderr, "[trace] DIP prims=%u start=%u base=%d fbo=%d vp=%d,%d,%d,%d z=%u/%u/%u cull=%u cw=%x vs=%d ps=%d "
                        "c0=%g,%g,%g,%g c1=%g,%g,%g,%g c2=%g,%g,%g,%g c3=%g,%g,%g,%g tex=%u,%u,%u,%u\n",
                primCount, startIndex, BaseVertexIndex, (int)fboActive_, vp[0], vp[1], vp[2], vp[3],
                (unsigned)rsCache_[D3DRS_ZENABLE], (unsigned)rsCache_[D3DRS_ZFUNC], (unsigned)rsCache_[D3DRS_ZWRITEENABLE],
                (unsigned)rsCache_[D3DRS_CULLMODE], (unsigned)rsCache_[D3DRS_COLORWRITEENABLE],
                vs_ ? (int)vs_->ok() : -1, ps_ ? (int)ps_->ok() : -1,
                vsConst_[0], vsConst_[1], vsConst_[2], vsConst_[3], vsConst_[4], vsConst_[5], vsConst_[6], vsConst_[7],
                vsConst_[8], vsConst_[9], vsConst_[10], vsConst_[11], vsConst_[12], vsConst_[13], vsConst_[14], vsConst_[15],
                boundTexName_[0], boundTexName_[1], boundTexName_[2], boundTexName_[3]);
    }
    glDrawElementsBaseVertex(mode, verts, idxType, const_cast<void *>(offset), BaseVertexIndex);
    if (g_kbTrace) fprintf(stderr, "[trace] DIP done err=%x\n", glGetError());
    return D3D_OK;
}
