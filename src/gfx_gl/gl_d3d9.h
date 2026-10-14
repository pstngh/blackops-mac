// gl_d3d9.h — OpenGL-backed implementations of the D3D9 interfaces.
//
// This is the heart of the translation layer: GLD3D9 implements IDirect3D9 and
// GLDevice implements IDirect3DDevice9 on top of OpenGL. The renderer in
// src/gfx_d3d is unchanged — it still calls Direct3DCreate9()/CreateDevice() and
// drives the returned interfaces.
//
// Methods that are not yet ported are stubbed inline so the layer keeps building
// and the device keeps running while they are filled in:
//   * state setters return D3D_OK (no-op) — wrong pixels, but no crash;
//   * resource creators return E_NOTIMPL until their GL resource classes land.
// Each stub is marked TODO so coverage is greppable.
#ifndef KISAK_GL_D3D9_H
#define KISAK_GL_D3D9_H

#include "gl_object.h"
#include "gl_spec.h"
#include <map>
#include <set>
#include <array>
#include <cstdint>

class GLContext;

// Shadow-stack switch (gl_resources.cpp; KB_SHADOWS=0 turns it off).
extern "C" int KB_ShadowsEnabled();
class GLVertexBuffer;
class GLIndexBuffer;
class GLVertexDeclaration;
class GLTexture;
class GLSurface;
class GLSwapChain;
class GLVertexShader;
class GLPixelShader;

// D3D sampler state, kept in D3D terms and translated to GL at draw time.
struct GLSamplerState {
    DWORD minFilter = D3DTEXF_LINEAR;
    DWORD magFilter = D3DTEXF_LINEAR;
    DWORD addressU  = D3DTADDRESS_WRAP;
    DWORD addressV  = D3DTADDRESS_WRAP;
};

// Fixed-function texture-stage state (D3DTSS_*), kept in D3D terms and folded into
// the built-in fragment shader's tex/diffuse combine at draw time. Defaults match
// D3D's stage-0 defaults: COLOROP = MODULATE(TEXTURE, DIFFUSE).
struct GLTextureStageState {
    DWORD colorOp   = D3DTOP_MODULATE;
    DWORD colorArg1 = D3DTA_TEXTURE;
    DWORD colorArg2 = D3DTA_DIFFUSE;
};

// Alpha-test state (removed from core GL), emulated via discard in the fragment
// shader. Defaults match D3D: disabled, ALWAYS, ref 0.
struct GLAlphaTestState {
    bool  enable = false;
    DWORD func   = D3DCMP_ALWAYS;
    DWORD ref    = 0;  // 0..255
};

// ---- IDirect3DDevice9 -> OpenGL -------------------------------------------
class GLDevice final : public GLObject<IDirect3DDevice9> {
public:
    GLDevice(GLContext *ctx, int width, int height, int samples, bool fullscreen);
    ~GLDevice() override;

    // --- Frame / target (gl_d3d9.cpp) ---
    HRESULT WINAPI TestCooperativeLevel() override { KB_SPEC("TestCooperativeLevel %s", KB_SpecThread()); return D3D_OK; }
    // No portable query; any GPU this layer runs on has more than 1 GB. Automatic texture detail (R_SetPicmip) follows
    // it: the old 256 MB gave half-resolution bump and specular maps.
    UINT    WINAPI GetAvailableTextureMem() override { KB_SPEC("GetAvailableTextureMem %s", KB_SpecThread()); return 1024u * 1024 * 1024; }
    HRESULT WINAPI GetDeviceCaps(D3DCAPS9 *pCaps) override;
    HRESULT WINAPI Reset(D3DPRESENT_PARAMETERS *pp) override;
    HRESULT WINAPI Present(const RECT *, const RECT *, HWND, const RGNDATA *) override;
    HRESULT WINAPI BeginScene() override { KB_SPEC("BeginScene %s inScene=%d", KB_SpecThread(), (int)inScene_); inScene_ = true;  return D3D_OK; }
    HRESULT WINAPI EndScene() override   { KB_SPEC("EndScene %s inScene=%d", KB_SpecThread(), (int)inScene_); inScene_ = false; return D3D_OK; }
    HRESULT WINAPI Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color,
                         float Z, DWORD Stencil) override;
    HRESULT WINAPI SetViewport(const D3DVIEWPORT9 *pViewport) override;

    // --- Geometry resources + draw (gl_d3d9_draw.cpp) ---
    HRESULT WINAPI CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                      IDirect3DVertexBuffer9 **ppVB, HANDLE *) override;
    HRESULT WINAPI CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                     IDirect3DIndexBuffer9 **ppIB, HANDLE *) override;
    HRESULT WINAPI CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pElements,
                                           IDirect3DVertexDeclaration9 **ppDecl) override;
    HRESULT WINAPI SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData,
                                   UINT OffsetInBytes, UINT Stride) override;
    HRESULT WINAPI SetIndices(IDirect3DIndexBuffer9 *pIndexData) override;
    HRESULT WINAPI SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) override;
    HRESULT WINAPI DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                                 UINT PrimitiveCount) override;
    HRESULT WINAPI DrawIndexedPrimitive(D3DPRIMITIVETYPE Type, INT BaseVertexIndex,
                                        UINT MinVertexIndex, UINT NumVertices,
                                        UINT startIndex, UINT primCount) override;
    HRESULT WINAPI DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT, const void *, UINT) override { KB_SPEC("DrawPrimitiveUP %s prim=%u", KB_SpecThread(), (unsigned)t); return D3D_OK; }

    // --- Render / sampler / texture state (gl_state.cpp) ---
    HRESULT WINAPI SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override;
    HRESULT WINAPI SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override;
    HRESULT WINAPI SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
    HRESULT WINAPI SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) override;
    HRESULT WINAPI SetScissorRect(const RECT *pRect) override;
    HRESULT WINAPI SetVertexShader(IDirect3DVertexShader9 *pShader) override;
    HRESULT WINAPI SetVertexShaderConstantF(UINT StartRegister, const float *pData, UINT Vec4Count) override;
    HRESULT WINAPI SetPixelShader(IDirect3DPixelShader9 *pShader) override;
    HRESULT WINAPI SetPixelShaderConstantF(UINT StartRegister, const float *pData, UINT Vec4Count) override;
    HRESULT WINAPI SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) override;
    HRESULT WINAPI SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) override;
    void    WINAPI SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *) override { KB_SPEC("SetGammaRamp %s", KB_SpecThread()); }

    // --- Not yet ported: textures / surfaces / shaders / queries — TODO(task #4/#5) ---
    HRESULT WINAPI GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **pp) override;
    HRESULT WINAPI GetSwapChain(UINT, IDirect3DSwapChain9 **pp) override;
    HRESULT WINAPI CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage,
                                 D3DFORMAT Format, D3DPOOL Pool,
                                 IDirect3DTexture9 **ppTexture, HANDLE *) override;
    HRESULT WINAPI CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
                                       D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *) override;
    HRESULT WINAPI CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format,
                                     D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *) override;
    HRESULT WINAPI CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE,
                                      DWORD, BOOL, IDirect3DSurface9 **ppSurface, HANDLE *) override;
    HRESULT WINAPI CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL,
                                               IDirect3DSurface9 **ppSurface, HANDLE *) override;
    HRESULT WINAPI GetRenderTargetData(IDirect3DSurface9 *pRenderTarget,
                                       IDirect3DSurface9 *pDestSurface) override;
    HRESULT WINAPI StretchRect(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect,
                               IDirect3DSurface9 *pDestSurface, const RECT *pDestRect,
                               D3DTEXTUREFILTERTYPE Filter) override;
    // Still stubbed (TODO): standalone depth-stencil surfaces, UpdateSurface.
    HRESULT WINAPI CreateDepthStencilSurface(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **pp, HANDLE *) override;
    HRESULT WINAPI UpdateSurface(IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *) override { KB_SPEC("UpdateSurface %s", KB_SpecThread()); return E_NOTIMPL; }
    HRESULT WINAPI CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) override;
    HRESULT WINAPI CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) override;
    HRESULT WINAPI CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) override;

    void retargetTexture(unsigned oldName, unsigned newName);   // see KB_GLRetargetTexture

    // KB_SPEC: the targets and texture kinds a draw uses.
    void specDraw(const char *kind, unsigned prim, int baseVertex);
    IDirect3DSurface9 *specRT_ = nullptr, *specDS_ = nullptr;
    const char        *specTexKind_[16] = {};
    unsigned           specTexFmt_[16] = {};
    bool               specVpFull_ = true;     // the viewport covers the render target

private:
    // ps_3_0 addresses 16 sampler stages (s0..s15). The lit world/material
    // shaders use the high samplers (s11 lightmap, s15 lookup, s12..s14) — with
    // fewer stages those bind to nothing and sample unit 0, turning lightmapped
    // surfaces magenta while low-sampler models render fine.
    static const int kMaxStages = 16;

    template <class T> static HRESULT ni(T **pp) { if (pp) *pp = nullptr; return E_NOTIMPL; }
    void ensureBuiltinProgram();  // lazily compile the built-in pre-transformed-vertex shader
    struct LinkedProgram;         // defined below
    void bindBuiltinForDraw();    // use built-in program + set frame/texture uniforms
    bool useDrawProgram();        // bind shader program (or built-in) + set uniforms; false = not linked yet, skip draw
    bool finalizeProgram(LinkedProgram &lp);  // poll link completion (non-blocking) + cache uniform locations
    void applyVertexState();      // set up VAO attribs from decl_ + streams_
private:
    bool applyTextures();         // bind stage-0 texture + sampler state; returns true if sampling
    void applyStageSampler(unsigned stage, unsigned target); // apply stage's filter/wrap to bound tex
    GLSurface *backBufferSurface(); // lazily create the back-buffer surface (bbFbo_ view)
    // Attach a correctly-sized auto depth-stencil renderbuffer to the live FBO and
    // GUARANTEE completeness (color-only last resort). Used wherever a DS attach left
    // the FBO incomplete — a stale or broken depth attachment otherwise no-ops every
    // draw of the pass = the scene goes (and stays) black.
    void kbRestoreAutoDepth(int w, int h);
    void resetViewportToTarget();   // D3D9 SetRenderTarget: full-target viewport and scissor
    // Guarantee the live custom FBO is COMPLETE by giving its colour texture renderable,
    // RT-sized storage (RGBA16F HDR, then RGBA8) — done once per (texture,size). Some RT
    // textures reach SetRenderTarget without renderable storage (wrong usage flag,
    // off-GL-thread creation, A16B16G16R16->non-renderable RGBA16), making the FBO
    // GL_FRAMEBUFFER_UNSUPPORTED so every draw no-ops = black scene. tex = colour texture.
    void kbEnsureRTComplete(unsigned tex, int w, int h);
    std::map<unsigned long long, int> rtFixed_;   // (tex<<32|wh) -> chosen internalformat

    GLContext *ctx_ = nullptr;
    int  fbWidth_   = 0;   // current render-target dimensions (back buffer or FBO)
    int  fbHeight_  = 0;
    int  bbWidth_   = 0;   // back-buffer dimensions (restored when RT is unset)
    int  bbHeight_  = 0;
    int  vpWidth_   = 1;   // current viewport size (the half-pixel offset is relative to it)
    int  vpHeight_  = 1;
    unsigned fbo_       = 0;  // reused FBO for render-to-texture
    unsigned fboDepth_  = 0;  // its depth-stencil renderbuffer
    int      fboDepthW_ = 0;
    int      fboDepthH_ = 0;
    // One auto depth-stencil renderbuffer per target size: reallocating a single one
    // for each size (the bloom chain alternates sizes) makes Apple's GL submit the
    // command buffer. selectAutoDepth points fboDepth_ at the one for w x h.
    std::map<unsigned, unsigned> autoDepthRbs_;
    void     selectAutoDepth(int w, int h);
    unsigned blitFbo_[2] = {};    // StretchRect's read and draw framebuffers
    bool inScene_   = false;

    // Back buffer / swap chain, exposed to the renderer through
    // GetBackBuffer()/GetSwapChain(). The back buffer is an offscreen FBO, not the
    // window: every target is rendered with D3D's row order (row 0 = top), so render
    // targets sample the right way up with D3D texture coordinates and blits between
    // them need no flip. Present copies it to the window upside down.
    // With D3D multisampling (the engine's r_aaSamples) the back buffer's
    // renderbuffers are multisampled; anything that reads it (Present, StretchRect,
    // screenshots) reads a single-sample copy, resolvedBackbufferFbo().
    GLSurface   *backBuffer_ = nullptr;  // owned
    GLSwapChain *swapChain_  = nullptr;  // owned
    unsigned bbFbo_     = 0;
    unsigned bbColorRb_ = 0;
    unsigned bbDepthRb_ = 0;
    int      bbFboW_    = 0;
    int      bbFboH_    = 0;
    int      bbSamples_ = 0;          // back-buffer samples (0 = single-sample)
    int      bbFboSamples_ = 0;       // ... the FBO's renderbuffers were allocated with
    unsigned bbResolveFbo_ = 0;       // single-sample copy of a multisampled back buffer
    unsigned bbResolveRb_  = 0;
    int      bbResolveW_   = 0;
    int      bbResolveH_   = 0;
    bool     fullscreen_   = false;   // D3DPRESENT_PARAMETERS::Windowed == FALSE
    void     ensureResolveFbo();      // size bbResolveFbo_ to the back buffer
public:
    unsigned backbufferFbo();     // create or resize the back-buffer FBO; returns its name
    unsigned curFbo() { return fboActive_ ? fbo_ : backbufferFbo(); }   // the live render target's FBO
    // The back buffer's contents in a single-sample FBO (resolving multisampling
    // first); leaves the live render target bound.
    unsigned resolvedBackbufferFbo();
    // Copy a colour rectangle of the bound read framebuffer into the back buffer,
    // which can't be the scaled target of a blit when multisampled.
    void     blitToBackbuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, unsigned filter);
private:

    unsigned vao_                 = 0;
    unsigned builtinProg_         = 0;
    int      builtinViewportLoc_  = -1;
    int      builtinTexLoc_       = -1;
    int      builtinUseTexLoc_    = -1;
    int      builtinColorOpLoc_   = -1;  // 0 = SELECTARG1 (tex), 1 = MODULATE (tex*diffuse)
    int      builtinAlphaFuncLoc_ = -1;  // GL-style compare func, or 0 = disabled
    int      builtinAlphaRefLoc_  = -1;  // [0,1] reference

    // Redundant-state elimination (each GL call costs driver CPU per draw). curProgram_
    // skips redundant glUseProgram; rsCache_/rsSet_
    // skip redundant SetRenderState GL calls (blend/depth/cull/colorwrite/etc.).
    unsigned      curProgram_ = 0;
    DWORD         rsCache_[256] = {};
    unsigned char rsSet_[256]   = {};

    struct Stream { GLVertexBuffer *vb = nullptr; UINT offset = 0; UINT stride = 0; };
    Stream               streams_[4];
    GLIndexBuffer       *ib_   = nullptr;
    GLVertexDeclaration *decl_ = nullptr;

    // VAO cache: one VAO per (decl, stream bindings) combination. applyVertexState
    // used to re-specify ~15-20 GL calls of attrib state on EVERY draw (~200k GL
    // calls/frame at 10k draws — measured 65ms/frame even on a local context);
    // a cache hit is a single glBindVertexArray. Entries also track the VAO's
    // captured GL_ELEMENT_ARRAY_BUFFER binding so unchanged index buffers skip
    // their per-draw rebind. Invalidated wholesale when any vertex/index buffer
    // or declaration is destroyed (g_kbVaoEpoch — GL object names get reused).
    struct VaoEntry { unsigned vao = 0; unsigned elem = 0; };
    std::map<std::array<unsigned, 13>, VaoEntry> vaoCache_;
    VaoEntry *curVaoEnt_   = nullptr;
    unsigned  curVao_      = 0;
    unsigned  vaoEpochSeen_ = 0;

    // Bound textures, resolved to a GL name + target in SetTexture so the bind
    // path is texture-type aware (2D / cube / volume) without a blind downcast.
    unsigned       boundTexName_[kMaxStages]   = {};   // GL texture object (0 = none)
    unsigned       boundTexTarget_[kMaxStages] = {};   // GL_TEXTURE_2D / _CUBE_MAP / _3D
    bool           boundTexIsDepth_[kMaxStages] = {};  // depth texture bound (shadow sampling)
    GLSurface     *curDS_     = nullptr;               // engine-set depth-stencil (shadow map)
    bool           fboActive_ = false;                 // rendering to fbo_ (not the backbuffer)
    bool           dsLive_    = false;                 // honored DS attached (shadow build pass)
    unsigned long long fboOkPairs_[8] = {};            // (colorTex<<32|dsTex) pairs verified COMPLETE
    int            fboOkN_ = 0;
    unsigned       curRTColorTex_ = 0;                 // colour tex of the live custom RT (for DS-pair keys)
    bool           scissorOn_ = false;                 // tracked GL_SCISSOR_TEST state (avoid per-Clear glIsEnabled sync round-trip)
    // FBO configs (colorTex,w,h) already verified GL_FRAMEBUFFER_COMPLETE. glCheckFramebufferStatus
    // can stall the driver; completeness cannot regress for immutable attachment storage, so
    // check once per config then skip forever.
    std::set<unsigned long long> fboComplete_;
    GLSamplerState samplers_[kMaxStages];
    GLTextureStageState texStage0_;     // stage-0 fixed-function combine (built-in program)
    GLAlphaTestState    alphaTest_;     // alpha-test emulation (built-in program)

    // Blend factors are set by two separate render states but applied together.
    DWORD blendSrc_  = D3DBLEND_ONE;
    DWORD blendDest_ = D3DBLEND_ZERO;
    // Lazy blend state: SRCBLEND/DESTBLEND/BLENDOP/ALPHABLENDENABLE only stage the shadow
    // values + set blendDirty_; commitBlendState() (called once per draw from useDrawProgram)
    // resolves them into at most one glEnable/glBlendFunc/glBlendEquation each, skipping any
    // whose already-applied value is unchanged. Kills the SRC-then-DEST double glBlendFunc and
    // the redundant blend toggles. appliedBlend* mirror the GL state actually emitted (so
    // commit is idempotent across draws).
    bool   blendEnabled_       = false;          // D3DRS_ALPHABLENDENABLE shadow
    DWORD  blendOp_            = D3DBLENDOP_ADD;  // D3DRS_BLENDOP shadow
    bool   blendDirty_         = true;           // a blend render-state changed since last commit
    int      appliedBlendEnabled_ = -1;          // -1 = unknown (force first emit)
    unsigned appliedBlendSrc_   = 0xFFFFFFFFu;   // GLenum; sentinel != any GL factor (GL_ZERO is 0!)
    unsigned appliedBlendDest_  = 0xFFFFFFFFu;
    unsigned appliedBlendOp_    = 0xFFFFFFFFu;
    void   commitBlendState();

    // Transparency anti-aliasing (the engine's r_aaAlpha, D3DRS_ADAPTIVETESS_Y): the
    // sample shading applied, 0 off, 1 half the samples, 2 all. Once per draw.
    int    sampleShading_ = 0;
    void   commitSampleShading();

    // Alpha test (func + ref are set separately but applied together via glAlphaFunc).
    DWORD alphaFunc_ = D3DCMP_ALWAYS;
    DWORD alphaRef_  = 0;
    // On the core profile (no fixed-function GL_ALPHA_TEST) the cutout is emulated with
    // discard in the translated fragment shaders, fed by uAlphaTestFunc/uAlphaRef.
    // alphaTestOn_ mirrors D3DRS_ALPHATESTENABLE so useDrawProgram can upload them.
    bool  alphaTestOn_ = false;

    // Programmable shader path: bound shaders, c# constant registers, and a cache
    // of linked (vs,ps) programs keyed by (vsShaderId<<32 | psShaderId).
    GLVertexShader *vs_ = nullptr;
    GLPixelShader  *ps_ = nullptr;
    float           vsConst_[256 * 4] = {};
    float           psConst_[256 * 4] = {};
    unsigned        vsVer_ = 1, psVer_ = 1;   // bumped when constants actually change
    // Dirty register span covering every change with version in (DirtyBaseVer, Ver].
    // A program whose upVer >= DirtyBaseVer only needs this span re-uploaded, not the
    // whole 256-vec4 array (the engine touches a few matrix registers per draw).
    unsigned        vsDirtyMin_ = 256, vsDirtyMax_ = 0, vsDirtyBaseVer_ = 1;
    unsigned        psDirtyMin_ = 256, psDirtyMax_ = 0, psDirtyBaseVer_ = 1;
    unsigned        unitTex_[kMaxStages] = {};        // per-GL-unit bound texture cache
    struct { unsigned tex = 0; unsigned char minF = 255, magF = 255, wS = 255, wT = 255; }
                    stageSamplerCache_[kMaxStages];   // last sampler params applied per stage
    struct LinkedProgram { unsigned prog = 0; int vscLoc = -1; int pscLoc = -1;
                           // Number of vec4s the shader's vsc[]/psc[] constant array
                           // actually declares. Uploading only these per draw (instead of
                           // a blanket 256+256) saves most of the per-draw upload.
                           int vscCount = 0; int pscCount = 0;
                           int alphaFuncLoc = -1; int alphaRefLoc = -1;
                           // Last alpha-test values uploaded — these change per material
                           // batch, not per draw; unconditional re-upload was 2 GL calls
                           // on every one of ~10k draws.
                           int upAlphaFunc = -999; float upAlphaRef = -999.0f;
                           // kbPosFixup (the D3D9 half-pixel offset): location + last value.
                           int posFixupLoc = -1; float upPosFixup[2] = { 0.0f, 0.0f };
                           // Versions of the constant arrays last uploaded to this
                           // program — constants change per material/pass, not per
                           // draw, so most of the per-draw glUniform4fv pairs skip.
                           unsigned upVsVer = 0; unsigned upPsVer = 0;
                           // Sampler uniform locations ("s0".."s15"), queried ONCE at
                           // link instead of 16 glGetUniformLocation calls per draw.
                           int samplerLoc[kMaxStages] = {};
                           // false until the async link completes and locs are cached;
                           // draws using it are skipped until then.
                           bool ready = false;
                           // Link-failure self-heal: a transiently-distressed GPU process
                           // fails links with empty logs; the program is deleted and
                           // re-linked later (bounded) instead of staying invalid forever.
                           int linkTries = 0; unsigned long lastFailPres = 0;
                           // COMPLETION_STATUS is polled once per PRESENT (not per draw):
                           // per-draw polling burns the whole budget in microseconds and
                           // then trusts getters that don't block on this ANGLE.
                           unsigned long lastPollPres = ~0ul;
                           // KHR_parallel_shader_compile completion polls so far. The
                           // async completion signal may need the worker's event loop
                           // (never pumped) — after a bounded number of polls the link is
                           // FORCED to finish with a blocking query (see finalizeProgram).
                           int pendPolls = 0;
                           // Verified-bind cache: a program whose glUseProgram fails
                           // (0x502) skips its draws (invisible) rather than running with
                           // the previously bound program (garbage). Set once a bind is
                           // clean.
                           bool bindOk = false; };
    std::map<uint64_t, LinkedProgram> progCache_;
};

// ---- IDirect3D9 -> OpenGL (the factory object) ----------------------------
class GLD3D9 final : public GLObject<IDirect3D9> {
public:
    // One adapter per SDL display (sdl_window.cpp's monitors).
    UINT    WINAPI GetAdapterCount() override;
    HRESULT WINAPI GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9 *pIdentifier) override;
    UINT    WINAPI GetAdapterModeCount(UINT Adapter, D3DFORMAT) override;
    HRESULT WINAPI EnumAdapterModes(UINT Adapter, D3DFORMAT, UINT Mode, D3DDISPLAYMODE *pMode) override;
    HMONITOR WINAPI GetAdapterMonitor(UINT Adapter) override { KB_SPEC("GetAdapterMonitor %s", KB_SpecThread()); return (HMONITOR)(intptr_t)(Adapter + 1); }
    HRESULT WINAPI GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) override;
    HRESULT WINAPI GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS9 *pCaps) override;
    HRESULT WINAPI CheckDeviceType(UINT, D3DDEVTYPE, D3DFORMAT a, D3DFORMAT b, BOOL w) override {
        KB_SPEC("CheckDeviceType %s %u %u %d", KB_SpecThread(), (unsigned)a, (unsigned)b, (int)w);
        return D3D_OK;
    }
    HRESULT WINAPI CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType,
                                     D3DFORMAT CheckFormat) override {
        // The engine PROBES formats in preference order — saying yes to everything makes
        // it pick vendor FOURCC hacks ('NULL', INTZ, ...) this layer cannot back. Reject
        // unknown FOURCCs (DXT1/3/5 excepted) so the probe falls through to a real format.
        HRESULT hr = checkDeviceFormat(CheckFormat);
        KB_SPEC("CheckDeviceFormat %s adapterfmt=%u usage=%x rtype=%u fmt=%u -> %s", KB_SpecThread(), (unsigned)AdapterFormat,
                (unsigned)Usage, (unsigned)RType, (unsigned)CheckFormat, hr == D3D_OK ? "ok" : "no");
        return hr;
    }
    static HRESULT checkDeviceFormat(D3DFORMAT CheckFormat) {
        unsigned f = (unsigned)CheckFormat;
        // NVIDIA's transparency supersampling, which the engine asks for with
        // r_aaAlpha: sample shading here (GLDevice::commitSampleShading).
        if (f == MAKEFOURCC('S', 'S', 'A', 'A'))
            return D3D_OK;
        if (KB_ShadowsEnabled() && f > 0x200 &&
            f != 827611204u /*DXT1*/ && f != 861165636u /*DXT3*/ && f != 894720068u /*DXT5*/)
            return D3DERR_NOTAVAILABLE;
        return D3D_OK;
    }
    HRESULT WINAPI CheckDeviceMultiSampleType(UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE Type, DWORD *pQ) override;
    HRESULT WINAPI CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT a, D3DFORMAT b, D3DFORMAT c) override {
        KB_SPEC("CheckDepthStencilMatch %s %u %u %u", KB_SpecThread(), (unsigned)a, (unsigned)b, (unsigned)c);
        return D3D_OK;
    }
    HRESULT WINAPI CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pp,
                                IDirect3DDevice9 **ppReturnedDeviceInterface) override;
};

#endif // KISAK_GL_D3D9_H
