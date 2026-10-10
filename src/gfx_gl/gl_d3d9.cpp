// gl_d3d9.cpp — OpenGL implementations of IDirect3D9 / IDirect3DDevice9.
#include "gl_d3d9.h"
#include "glcontext.h"
#include "gl_resources.h"

#include <GL/glew.h>

#include <SDL2/SDL.h>   // adapter display-mode queries (EnumAdapterModes etc.)
#include "../platform/sdl/sdl_display.h"
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <vector>
extern int g_kbTrace;   // KB_TRACEFRAME (defined below, before Present)


// glClearDepth/glDepthRange (the desktop double variants).
static inline void KB_glClearDepth(double z) {
    glClearDepth(z);
}
static inline void KB_glDepthRange(double n, double f) {
    glDepthRange(n, f);
}

// Default device caps, shared by GLDevice::GetDeviceCaps and GLD3D9::GetDeviceCaps.
// These advertise an SM3.0-class GPU, which is what the Black Ops renderer expects.
static void FillDefaultCaps(D3DCAPS9 *c) {
    *c = D3DCAPS9{};
    c->DeviceType              = D3DDEVTYPE_HAL;
    c->MaxTextureWidth         = 8192;
    c->MaxTextureHeight        = 8192;
    c->MaxAnisotropy           = 16;
    c->MaxSimultaneousTextures = 8;
    c->MaxTextureBlendStages   = 8;
    c->NumSimultaneousRTs      = 4;
    c->VertexShaderVersion     = 0xFFFE0300;  // vs_3_0
    c->PixelShaderVersion      = 0xFFFF0300;  // ps_3_0
    c->MaxVertexShaderConst    = 256;
    c->MaxPrimitiveCount       = 0x00FFFFFF;
    c->MaxVertexIndex          = 0x00FFFFFF;
    c->MaxStreams              = 16;
    // Slope-scaled depth bias: every D3D9-era GPU (incl. the 8600GT this game shipped on) had
    // it. Without advertising it, R_HW_SetPolygonOffset (r_state.cpp) drops the polygon-offset
    // SLOPE term entirely and only sends the constant D3DRS_DEPTHBIAS — so decals on
    // grazing-angle walls/fences are under-biased and render THROUGH the geometry. Advertising
    // it restores the engine's intended slope+constant bias for both decals and shadowmaps.
    c->RasterCaps |= 0x02000000;  // D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS
}

// The most samples a multisampled renderbuffer may have (GL_MAX_SAMPLES; Apple
// silicon: 4). Known once a context has been current; until the device exists
// (the engine checks its anti-aliasing setting before creating it) any count up to
// D3D's 16 is accepted, and the back buffer gets as many as the GPU allows.
static int g_kbMaxSamples;

static int KB_MaxSamples() {
    if (!g_kbMaxSamples && SDL_GL_GetCurrentContext())
    {
        GLint n = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &n);
        g_kbMaxSamples = n > 0 ? (int)n : 1;
    }
    return g_kbMaxSamples ? g_kbMaxSamples : 16;
}

// The back buffer's GL sample count for a D3D multisample type: 0 (single-sample)
// for none and for D3DMULTISAMPLE_NONMASKABLE, which counts in quality levels
// (the engine asks for it only to build reflection probes).
static int KB_BackbufferSamples(D3DMULTISAMPLE_TYPE type) {
    if ((int)type < 2) return 0;
    const int maxSamples = KB_MaxSamples();
    const int n = (int)type < maxSamples ? (int)type : maxSamples;
    return n >= 2 ? n : 0;
}

// ---------------------------------------------------------------------------
// GLDevice
// ---------------------------------------------------------------------------
GLDevice::GLDevice(GLContext *ctx, int width, int height, int samples, bool fullscreen)
    : ctx_(ctx), fbWidth_(width), fbHeight_(height), bbWidth_(width), bbHeight_(height),
      bbSamples_(samples), fullscreen_(fullscreen) {
    // The context is current here (GLD3D9::CreateDevice): start on the back buffer,
    // as a D3D device does.
    glBindFramebuffer(GL_FRAMEBUFFER, backbufferFbo());
    glViewport(0, 0, width, height);
}

GLDevice::~GLDevice() {
    if (swapChain_)   swapChain_->Release();   // swap chain references the back buffer; drop it first
    if (backBuffer_)  backBuffer_->Release();
    if (builtinProg_) glDeleteProgram(builtinProg_);
    if (vao_)         glDeleteVertexArrays(1, &vao_);
    if (fbo_)         glDeleteFramebuffers(1, &fbo_);
    for (auto &rb : autoDepthRbs_) glDeleteRenderbuffers(1, &rb.second);
    if (blitFbo_[0])  glDeleteFramebuffers(2, blitFbo_);
    if (bbFbo_)       glDeleteFramebuffers(1, &bbFbo_);
    if (bbColorRb_)   glDeleteRenderbuffers(1, &bbColorRb_);
    if (bbDepthRb_)   glDeleteRenderbuffers(1, &bbDepthRb_);
    if (bbResolveFbo_) glDeleteFramebuffers(1, &bbResolveFbo_);
    if (bbResolveRb_)  glDeleteRenderbuffers(1, &bbResolveRb_);
    delete ctx_;
}

unsigned GLDevice::backbufferFbo() {
    if (bbFbo_ && bbFboW_ == bbWidth_ && bbFboH_ == bbHeight_ && bbFboSamples_ == bbSamples_)
        return bbFbo_;
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    if (!bbFbo_) {
        glGenFramebuffers(1, &bbFbo_);
        glGenRenderbuffers(1, &bbColorRb_);
        glGenRenderbuffers(1, &bbDepthRb_);
    }
    // Multisampled: the engine draws the scene straight into the back buffer
    // (R_RENDERTARGET_SCENE shares R_RENDERTARGET_FRAME_BUFFER), as on D3D.
    glBindRenderbuffer(GL_RENDERBUFFER, bbColorRb_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, bbSamples_, GL_RGBA8, bbWidth_, bbHeight_);
    glBindRenderbuffer(GL_RENDERBUFFER, bbDepthRb_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, bbSamples_, GL_DEPTH24_STENCIL8, bbWidth_, bbHeight_);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, bbFbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, bbColorRb_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, bbDepthRb_);
    unsigned st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    if (st != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "[gl] back-buffer FBO %dx%d (%d samples) incomplete: 0x%x\n", bbWidth_, bbHeight_, bbSamples_, st);
    else
        fprintf(stderr, "[gl] back buffer %dx%d, %s%d samples\n", bbWidth_, bbHeight_,
                samples > 1 ? "multisampled, " : "", samples > 1 ? (int)samples : 1);
    bbFboW_ = bbWidth_; bbFboH_ = bbHeight_; bbFboSamples_ = bbSamples_;
    // A resize while the back buffer is bound must leave it bound.
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned)prevFbo == bbFbo_ || !fboActive_ ? bbFbo_ : (unsigned)prevFbo);
    return bbFbo_;
}

void GLDevice::ensureResolveFbo() {
    if (bbResolveFbo_ && bbResolveW_ == bbWidth_ && bbResolveH_ == bbHeight_)
        return;
    if (!bbResolveFbo_) {
        glGenFramebuffers(1, &bbResolveFbo_);
        glGenRenderbuffers(1, &bbResolveRb_);
    }
    glBindRenderbuffer(GL_RENDERBUFFER, bbResolveRb_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, bbWidth_, bbHeight_);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bbResolveFbo_);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, bbResolveRb_);
    bbResolveW_ = bbWidth_; bbResolveH_ = bbHeight_;
}

// A multisampled read framebuffer can only be blitted 1:1 into a single-sample one
// of the same format (no scaling, no format conversion; glReadPixels refuses it), so
// readers of the back buffer take this copy.
unsigned GLDevice::resolvedBackbufferFbo() {
    unsigned bb = backbufferFbo();
    if (!bbSamples_)
        return bb;
    ensureResolveFbo();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, bb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bbResolveFbo_);
    if (scissorOn_) glDisable(GL_SCISSOR_TEST);   // blits are scissored
    glBlitFramebuffer(0, 0, bbWidth_, bbHeight_, 0, 0, bbWidth_, bbHeight_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (scissorOn_) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, curFbo());
    return bbResolveFbo_;
}

// The caller has bound the source as the read framebuffer (and disabled the scissor).
// A multisampled back buffer only takes 1:1 blits of its own format: go through the
// single-sample copy, scaling into it, then copy the same rectangle across.
void GLDevice::blitToBackbuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, unsigned filter) {
    unsigned bb = backbufferFbo();
    if (!bbSamples_) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bb);
        glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT, filter);
        return;
    }
    ensureResolveFbo();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bbResolveFbo_);
    glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT, filter);
    const int x0 = dx0 < dx1 ? dx0 : dx1, x1 = dx0 < dx1 ? dx1 : dx0;
    const int y0 = dy0 < dy1 ? dy0 : dy1, y1 = dy0 < dy1 ? dy1 : dy0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, bbResolveFbo_);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bb);
    glBlitFramebuffer(x0, y0, x1, y1, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

void GLDevice::kbEnsureRTComplete(unsigned tex, int w, int h) {
    if (!tex || w <= 0 || h <= 0) return;
    unsigned long long key = ((unsigned long long)tex << 32) | ((unsigned)w << 16) | (unsigned)(h & 0xFFFF);
    if (rtFixed_.count(key)) return;   // already given renderable storage at this size
    // The live FBO (fbo_) has this tex as COLOR0 and an auto depth attachment. Re-allocate
    // the colour storage to a renderable format sized to the RT, then re-verify the FULL
    // attachment combo (colour + depth). RGBA16F preserves HDR; RGBA8 is the last resort.
    unsigned cands[2] = { GL_RGBA16F, GL_RGBA8 };
    unsigned fmts[2]  = { GL_RGBA,    GL_RGBA   };
    unsigned types[2] = { GL_HALF_FLOAT, GL_UNSIGNED_BYTE };
    int chosen = -1;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    unsigned dbgErr1 = 0, dbgStColorDepth = 0, dbgStColorOnly = 0, dbgErr2 = 0;
    GLint dbgBound = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &dbgBound);
    for (int i = 0; i < 2; ++i) {
        while (glGetError() != GL_NO_ERROR) {}
        glTexImage2D(GL_TEXTURE_2D, 0, cands[i], w, h, 0, fmts[i], types[i], nullptr);
        dbgErr1 = glGetError();   // did the storage alloc itself fail (e.g. immutable tex)?
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        // Match the auto depth renderbuffer to this size (a stale-sized one also fails).
        kbRestoreAutoDepth(w, h);
        dbgStColorDepth = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (glGetError() == GL_NO_ERROR && dbgStColorDepth == GL_FRAMEBUFFER_COMPLETE) { chosen = i; break; }
        // Color-ONLY (no depth at all): isolates whether the depth attach is the problem.
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        dbgStColorOnly = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        dbgErr2 = glGetError();
    }
    rtFixed_[key] = chosen >= 0 ? (int)cands[chosen] : 0;
    static int fixN = 0;
    if (++fixN <= 12)
        fprintf(stderr, "[gl] kbEnsureRTComplete tex=%u %dx%d -> %s | fbo=%d texErr=0x%x stCD=0x%x stColorOnly=0x%x err2=0x%x\n",
                tex, w, h, chosen == 0 ? "RGBA16F" : chosen == 1 ? "RGBA8" : "STILL-INCOMPLETE",
                (int)dbgBound, dbgErr1, dbgStColorDepth, dbgStColorOnly, dbgErr2);
}

void GLDevice::selectAutoDepth(int w, int h) {
    if (fboDepth_ && fboDepthW_ == w && fboDepthH_ == h) return;
    unsigned &rb = autoDepthRbs_[((unsigned)w << 16) | (unsigned)(h & 0xFFFF)];
    if (!rb) {
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }
    fboDepth_ = rb; fboDepthW_ = w; fboDepthH_ = h;
}

void GLDevice::kbRestoreAutoDepth(int w, int h) {
    // Detach ALL depth-ish attachments (a leftover DEPTH-only or stencil attachment
    // from the broken DS attach would conflict), then attach a fresh DEPTH24_STENCIL8
    // renderbuffer sized to the live color target (this layer keeps depth and color the
    // same size, a rule its WebGL target needed; a stale-sized fboDepth_ breaks that).
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    if (w > 0 && h > 0) {
        selectAutoDepth(w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fboDepth_);
    } else {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    }
    // Last resort: color-only beats a permanently-black scene.
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
}

// D3D9's SetRenderTarget(0, ..) sets the viewport to the whole new target (depth 0..1)
// and resets the scissor rect to it; the renderer relies on that and does not
// re-issue SetViewport for full-target post passes.
void GLDevice::resetViewportToTarget() {
    glViewport(0, 0, fbWidth_, fbHeight_);
    vpWidth_ = fbWidth_; vpHeight_ = fbHeight_;
    KB_glDepthRange(0.0, 1.0);
    glScissor(0, 0, fbWidth_, fbHeight_);
}

HRESULT WINAPI GLDevice::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) {
    if (RenderTargetIndex != 0) return D3D_OK;  // single render target for now (MRT: TODO)
    GLSurface *s = static_cast<GLSurface *>(pRenderTarget);
    if (g_kbTrace) fprintf(stderr, "[trace] SetRT %p bb=%d %ux%u tex=%u ds=%p\n", (void *)s, s ? (int)s->isBackbuffer() : -1,
                           s ? s->width() : 0, s ? s->height() : 0, s ? s->texName() : 0, (void *)curDS_);
    // A null target, or the back-buffer surface itself, means the back buffer.
    if (!s || s->isBackbuffer()) {
        fboActive_ = false;
        glBindFramebuffer(GL_FRAMEBUFFER, backbufferFbo());
        dsLive_ = false;
        fbWidth_ = bbWidth_; fbHeight_ = bbHeight_;
        resetViewportToTarget();
        return D3D_OK;
    }
    if (!fbo_) glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    fboActive_ = true;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->texName(), s->level());
    curRTColorTex_ = s->texName();

    int w = (int)s->width(), h = (int)s->height();
    // Honor an engine-set depth-stencil surface backed by a DEPTH TEXTURE (the shadow
    // map): attach it so the shadow pass renders depth into sampleable storage. The
    // attach point depends on whether the format carries stencil.
    if (curDS_ && curDS_->texName() &&
        curDS_->width() == (UINT)w && curDS_->height() == (UINT)h) {
        // The DS texture's storage decides the attach point. Depth-tag surfaces always
        // carry DEPTH24_STENCIL8 (the engine's metrics format arrives as garbage, see
        // gl_resources.cpp); texture-backed depth surfaces keep the format-derived choice.
        unsigned attach = curDS_->texIsDepthStencil() ? GL_DEPTH_STENCIL_ATTACHMENT
                          : (curDS_->format() == (D3DFORMAT)80 /*D16*/ ||
                             curDS_->format() == (D3DFORMAT)70 /*D16_LOCKABLE*/ ||
                             curDS_->format() == (D3DFORMAT)71 /*D32*/)
                                ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT;
        // Clear the OTHER attach point first (a stale renderbuffer on DEPTH_STENCIL
        // while we attach DEPTH leaves the FBO incomplete).
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, attach, GL_TEXTURE_2D, curDS_->texName(), 0);
        // An incomplete FBO silently no-ops EVERY draw of the pass (the B59 black-region
        // regression: a color texture attached as depth no-op'd the MAIN scene). Never
        // leave a broken FBO live — detach and fall back to the auto renderbuffer.
        // glCheckFramebufferStatus can stall the driver; with ~9 DS binds a frame, skip
        // it for pairs already known good (attachments are immutable storage;
        // completeness cannot regress).
        unsigned long long pairKey = ((unsigned long long)s->texName() << 32) | curDS_->texName();
        bool known = false;
        for (int pi = 0; pi < fboOkN_; ++pi) if (fboOkPairs_[pi] == pairKey) { known = true; break; }
        unsigned st = known ? GL_FRAMEBUFFER_COMPLETE : glCheckFramebufferStatus(GL_FRAMEBUFFER);
        bool dsAttached = (st == GL_FRAMEBUFFER_COMPLETE);
        if (dsAttached && !known && fboOkN_ < 8) fboOkPairs_[fboOkN_++] = pairKey;
        if (!dsAttached)
            glFramebufferTexture2D(GL_FRAMEBUFFER, attach, GL_TEXTURE_2D, 0, 0);
        {
            static unsigned lastStatus = 0xFFFFFFFFu;
            if (st != lastStatus) {
                lastStatus = st;
                fprintf(stderr, "[gl] DS-attach FBO status=0x%x (%s) ds=%ux%u rt=%dx%d\n",
                        st, dsAttached ? "COMPLETE" : "INCOMPLETE->auto-renderbuffer for this pass",
                        curDS_->width(), curDS_->height(), w, h);
            }
        }
        if (dsAttached) {
            dsLive_ = true; fbWidth_ = w; fbHeight_ = h;
            resetViewportToTarget();
            return D3D_OK;
        }
    }
    dsLive_ = false;
    {
        // Auto depth-stencil renderbuffer sized to the colour target (the default path).
        selectAutoDepth(w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fboDepth_);
    }

    fbWidth_ = w; fbHeight_ = h;
    resetViewportToTarget();
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) {
    GLSurface *ds = static_cast<GLSurface *>(pNewZStencil);
    // Only depth-TEXTURE-backed surfaces are honored (texName != 0, i.e. a view onto a
    // depth-format GLTexture). Plain metadata depth-stencil handles keep the auto
    // renderbuffer behavior. NULL restores the auto path.
    curDS_ = (KB_ShadowsEnabled() && ds && ds->texName()) ? ds : nullptr;
    // The shadowmap image's CreateTexture flags don't reliably mark it depth (engine
    // flag soup + garbage formats from the decompiled metrics). The point of truth is
    // HERE: anything used as a depth-stencil surface gets real depth storage.
    if (curDS_ && curDS_->ownerTex() && !curDS_->ownerTex()->isDepth())
        curDS_->ownerTex()->ensureDepthStorage();
    {
        static bool once = false;
        if (curDS_ && !once) {
            once = true;
            fprintf(stderr, "[gl] SetDepthStencilSurface: depth texture honored (%ux%u fmt=%u)\n",
                    curDS_->width(), curDS_->height(), (unsigned)curDS_->format());
        }
    }
    if (fboActive_) {
        // Re-apply on the live FBO immediately (engine may set DS after the RT) — but
        // only when the DS size matches the live RT (the equal-size rule above; D3D9
        // allowed DS >= RT, e.g. the 1080p main DS during a 256x256 UI3D pass).
        if (curDS_ && curDS_->width() == (UINT)fbWidth_ && curDS_->height() == (UINT)fbHeight_) {
            unsigned attach = curDS_->texIsDepthStencil() ? GL_DEPTH_STENCIL_ATTACHMENT
                              : (curDS_->format() == (D3DFORMAT)80 || curDS_->format() == (D3DFORMAT)70 ||
                                 curDS_->format() == (D3DFORMAT)71)
                                    ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT;
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, attach, GL_TEXTURE_2D, curDS_->texName(), 0);
            dsLive_ = true;
            // Never leave a broken FBO live: incomplete -> a correctly-sized auto
            // renderbuffer for THIS pass (keep curDS_; it may match a later RT). A
            // stale-sized fboDepth_ was itself incomplete = the scene went and STAYED
            // black after a decal/DS-switch mid-game.
            // Cache verified (colorTex,dsTex) pairs — glCheckFramebufferStatus is a SYNC
            // round-trip (the #1 DOM-thread cost) and this DS re-apply runs ~9x/frame.
            unsigned long long dsKey = ((unsigned long long)curRTColorTex_ << 32) | curDS_->texName();
            bool dsKnown = false;
            for (int pi = 0; pi < fboOkN_; ++pi) if (fboOkPairs_[pi] == dsKey) { dsKnown = true; break; }
            unsigned dsStatus = dsKnown ? GL_FRAMEBUFFER_COMPLETE : glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (dsStatus == GL_FRAMEBUFFER_COMPLETE && !dsKnown && fboOkN_ < 8) fboOkPairs_[fboOkN_++] = dsKey;
            if (dsStatus != GL_FRAMEBUFFER_COMPLETE) {
                static int dsIncN = 0;
                if (++dsIncN <= 6)
                    fprintf(stderr, "[gl] DS re-apply INCOMPLETE status=0x%x ds=%ux%u(fmt=%u dsTex=%u attach=0x%x) rt=%dx%d -> auto rb\n",
                            dsStatus, curDS_->width(), curDS_->height(), (unsigned)curDS_->format(),
                            curDS_->texName(), attach, fbWidth_, fbHeight_);
                dsLive_ = false;
                kbRestoreAutoDepth(fbWidth_, fbHeight_);
            }
        } else if (fboDepth_ && fboDepthW_ == fbWidth_ && fboDepthH_ == fbHeight_) {
            // DS size != live RT (the equal-size rule), auto rb already correct:
            // cheap re-attach, no completeness check (the hot path, ~9 DS sets/frame).
            dsLive_ = false;
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fboDepth_);
        } else {
            // Auto rb missing or wrong-sized: size + attach + verify.
            dsLive_ = false;
            kbRestoreAutoDepth(fbWidth_, fbHeight_);
        }
    }
    return D3D_OK;
}

HRESULT WINAPI GLDevice::Reset(D3DPRESENT_PARAMETERS *pp) {
    if (pp && pp->BackBufferWidth && pp->BackBufferHeight) {
        bbWidth_  = (int)pp->BackBufferWidth;
        bbHeight_ = (int)pp->BackBufferHeight;
        bbSamples_ = KB_BackbufferSamples(pp->MultiSampleType);
        fullscreen_ = !pp->Windowed;
        if (backBuffer_) backBuffer_->setSize((UINT)bbWidth_, (UINT)bbHeight_);
        fboActive_ = false;
        glBindFramebuffer(GL_FRAMEBUFFER, backbufferFbo());
        fbWidth_ = bbWidth_; fbHeight_ = bbHeight_;
        resetViewportToTarget();
        if (ctx_) {
            ctx_->Resize(fbWidth_, fbHeight_);
            ctx_->SetFullscreen(!pp->Windowed);
            ctx_->SetVSync(pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE);
        }
    }
    return D3D_OK;
}

// Diagnostic: KB_TRACEFRAME=<n> logs the render-target, viewport, clear and draw calls
// of the frame after present n (native only).
int g_kbTrace = 0;

// Diagnostic: KB_SCREENSHOT=<dir> writes the back buffer to <dir>/present_<n>.tga
// every KB_SCREENSHOT_EVERY presents (default 300). The console `screenshot` command
// needs the console, and `+wait` on the command line runs out during loading.
// Write the default framebuffer's back buffer to <path> as a 32-bit TGA (alpha forced
// opaque). Restores the read framebuffer and read buffer.
// Framebuffer 0 is the window, whose rows are in GL order (bottom first).
void KB_WriteBackbufferTGA(unsigned fbo, const char *path, int w, int h) {
    std::vector<unsigned char> px((size_t)w * h * 4);
    GLint oldFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadBuffer(fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, oldFbo);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 0xFF;

    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned char hdr[18] = {0};   // uncompressed true-colour, 32bpp BGRA
    hdr[2]  = 2;
    hdr[12] = (unsigned char)(w & 0xFF); hdr[13] = (unsigned char)((w >> 8) & 0xFF);
    hdr[14] = (unsigned char)(h & 0xFF); hdr[15] = (unsigned char)((h >> 8) & 0xFF);
    hdr[16] = 32; hdr[17] = fbo ? 8 | 0x20 : 8;   // 0x20: rows top-down
    fwrite(hdr, 1, sizeof(hdr), f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

// Returns the present's number when it wrote a screenshot, else 0.
static unsigned long KB_MaybeScreenshot(unsigned fbo, int w, int h) {
    static const char *dir = getenv("KB_SCREENSHOT");
    if (!dir || w <= 0 || h <= 0) return 0;
    static int every = [] { const char *e = getenv("KB_SCREENSHOT_EVERY"); int n = e ? atoi(e) : 0; return n > 0 ? n : 300; }();
    static unsigned long presents;
    if (++presents % every) return 0;

    char path[1024];
    snprintf(path, sizeof(path), "%s/present_%06lu.tga", dir, presents);
    KB_WriteBackbufferTGA(fbo, path, w, h);
    extern unsigned long g_kbDraws, g_kbBuiltinFall, g_kbSkipPending, g_kbProgLinks, g_kbBlits;
    static unsigned long lastDraws, lastFall, lastSkip;
    fprintf(stderr, "[gl] wrote %s (since last: draws=%lu builtinFall=%lu skipPending=%lu; links=%lu blits=%lu)\n", path,
            g_kbDraws - lastDraws, g_kbBuiltinFall - lastFall, g_kbSkipPending - lastSkip, g_kbProgLinks, g_kbBlits);
    lastDraws = g_kbDraws; lastFall = g_kbBuiltinFall; lastSkip = g_kbSkipPending;
    return presents;
}

// KB_SCREENSHOT_WINDOW=1 with KB_SCREENSHOT: also write what the window shows
// (window_N.tga, the window's pixel size) at the same presents, after scaling.
static void KB_MaybeScreenshotWindow(unsigned long shot, int w, int h) {
    static const char *dir = getenv("KB_SCREENSHOT");
    static const char *window = getenv("KB_SCREENSHOT_WINDOW");
    if (!shot || !window || *window == '0' || w <= 0 || h <= 0) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/window_%06lu.tga", dir, shot);
    KB_WriteBackbufferTGA(0, path, w, h);
}

// The back buffer's rectangle in the window at the last Present (x, y from the top
// left, width, height) and the back buffer's size: the window's cursor position maps
// back through it (IN_Frame), as the window can be larger than the back buffer.
static std::atomic<int> g_kbPresentRect[6];

static bool KB_PresentRect(int r[6]) {
    for (int i = 0; i < 6; ++i) r[i] = g_kbPresentRect[i];
    return r[2] > 0 && r[3] > 0 && r[4] > 0 && r[5] > 0;
}

void KB_WindowToBackbuffer(int *x, int *y) {
    int r[6];
    if (!KB_PresentRect(r)) return;
    *x = (int)((long long)(*x - r[0]) * r[4] / r[2]);
    *y = (int)((long long)(*y - r[1]) * r[5] / r[3]);
    *x = *x < 0 ? 0 : (*x >= r[4] ? r[4] - 1 : *x);
    *y = *y < 0 ? 0 : (*y >= r[5] ? r[5] - 1 : *y);
}

void KB_BackbufferToWindow(int *x, int *y) {
    int r[6];
    if (!KB_PresentRect(r)) return;
    *x = r[0] + (int)((long long)*x * r[2] / r[4]);
    *y = r[1] + (int)((long long)*y * r[3] / r[5]);
}

HRESULT WINAPI GLDevice::Present(const RECT *, const RECT *, HWND, const RGNDATA *) {
    unsigned bb = resolvedBackbufferFbo();
    const unsigned long shot = KB_MaybeScreenshot(bb, bbWidth_, bbHeight_);
    {
        static const char *traceList = getenv("KB_TRACEFRAME");   // comma-separated present numbers
        static long presentNo;
        ++presentNo;
        if (g_kbTrace) { g_kbTrace = 0; fprintf(stderr, "[trace] ---- present\n"); }
        for (const char *t = traceList; t && *t; ) {
            char *end;
            long n = strtol(t, &end, 10);
            if (end == t) break;
            if (n == presentNo) { g_kbTrace = 1; fprintf(stderr, "[trace] ---- frame after present %ld\n", presentNo); break; }
            t = *end == ',' ? end + 1 : end;
        }
    }
    // The back buffer holds D3D row order (top row first); the window wants GL's.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, bb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDrawBuffer(GL_BACK);
    if (scissorOn_) glDisable(GL_SCISSOR_TEST);   // blits are scissored
    // The window can differ from the back buffer: a fullscreen window covers the
    // display whatever the back buffer's size, and a window dragged to a display of
    // another pixel density (Retina or not) keeps its size in points. Fullscreen
    // fills the display, as D3D's fullscreen mode did: the engine's automatic aspect
    // ratio is the monitor's in fullscreen (R_StoreWindowSettings), so a 4:3 back
    // buffer holds a picture squeezed to be stretched. A window keeps the back
    // buffer's shape, black around it. KB_WindowToBackbuffer maps the cursor back
    // through the same rectangle.
    int winW = 0, winH = 0;
    if (ctx_) ctx_->GetDrawableSize(&winW, &winH);
    int x0 = 0, y0 = 0, x1 = bbWidth_, y1 = bbHeight_;
    const bool scaled = winW > 0 && winH > 0 && (winW != bbWidth_ || winH != bbHeight_);
    if (scaled && fullscreen_) {
        x1 = winW; y1 = winH;
    } else if (scaled) {
        if ((long long)winW * bbHeight_ > (long long)winH * bbWidth_) {
            const int w = (int)((long long)winH * bbWidth_ / bbHeight_);
            x0 = (winW - w) / 2; x1 = x0 + w; y1 = winH;
        } else {
            const int h = (int)((long long)winW * bbHeight_ / bbWidth_);
            y0 = (winH - h) / 2; y1 = y0 + h; x1 = winW;
        }
        GLboolean mask[4];
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
    }
    g_kbPresentRect[0] = x0; g_kbPresentRect[1] = winH > 0 ? winH - y1 : 0;   // top-left origin
    g_kbPresentRect[2] = x1 - x0; g_kbPresentRect[3] = y1 - y0;
    g_kbPresentRect[4] = bbWidth_; g_kbPresentRect[5] = bbHeight_;
    glBlitFramebuffer(0, 0, bbWidth_, bbHeight_, x0, y1, x1, y0, GL_COLOR_BUFFER_BIT, scaled ? GL_LINEAR : GL_NEAREST);
    if (scissorOn_) glEnable(GL_SCISSOR_TEST);
    KB_MaybeScreenshotWindow(shot, winW > 0 ? winW : bbWidth_, winH > 0 ? winH : bbHeight_);
    if (ctx_) ctx_->SwapBuffers();
    glBindFramebuffer(GL_FRAMEBUFFER, curFbo());
    return D3D_OK;
}

HRESULT WINAPI GLDevice::Clear(DWORD /*Count*/, const D3DRECT * /*pRects*/, DWORD Flags,
                               D3DCOLOR Color, float Z, DWORD Stencil) {
    if (g_kbTrace) fprintf(stderr, "[trace] Clear flags=%x color=%08x z=%g fbo=%d\n", (unsigned)Flags, (unsigned)Color, Z, (int)fboActive_);
    GLbitfield mask = 0;
    if (Flags & D3DCLEAR_TARGET) {
        const float inv = 1.0f / 255.0f;
        glClearColor(((Color >> 16) & 0xff) * inv,   // R
                     ((Color >>  8) & 0xff) * inv,   // G
                     ((Color      ) & 0xff) * inv,   // B
                     ((Color >> 24) & 0xff) * inv);  // A
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (Flags & D3DCLEAR_ZBUFFER)  { KB_glClearDepth(Z);       mask |= GL_DEPTH_BUFFER_BIT; }
    if (Flags & D3DCLEAR_STENCIL)  { glClearStencil((GLint)Stencil); mask |= GL_STENCIL_BUFFER_BIT; }

    // D3D's Clear ignores scissor (when no rects) and the write masks; GL's does
    // not. Force the affected state for the clear, then restore.
    // Use the shadowed scissor state, not glIsEnabled (a driver round-trip; Clear runs
    // several times per frame).
    bool scissor = scissorOn_;
    if (scissor) glDisable(GL_SCISSOR_TEST);
    if (mask & GL_COLOR_BUFFER_BIT) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (mask & GL_DEPTH_BUFFER_BIT) glDepthMask(GL_TRUE);

    glClear(mask);

    if (scissor) glEnable(GL_SCISSOR_TEST);
    // Clear forced colorMask/depthMask open and did NOT restore D3D's values, so the GL
    // state now diverges from the SetRenderState cache — invalidate those entries so the
    // next SetRenderState re-applies them (otherwise the masks stay stuck full-open).
    if (mask & GL_COLOR_BUFFER_BIT) rsSet_[D3DRS_COLORWRITEENABLE] = 0;
    if (mask & GL_DEPTH_BUFFER_BIT) rsSet_[D3DRS_ZWRITEENABLE]     = 0;
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetViewport(const D3DVIEWPORT9 *vp) {
    if (!vp) return E_INVALIDARG;
    if (g_kbTrace) fprintf(stderr, "[trace] SetViewport %u %u %u %u z %g..%g fbo=%d\n", (unsigned)vp->X, (unsigned)vp->Y,
                           (unsigned)vp->Width, (unsigned)vp->Height, vp->MinZ, vp->MaxZ, (int)fboActive_);
    // Every target, the back buffer included, is stored with D3D's row order (the
    // vertex shaders negate clip-space Y), so GL window y is D3D's y: no flip.
    glViewport((GLint)vp->X, (GLint)vp->Y, (GLsizei)vp->Width, (GLsizei)vp->Height);
    vpWidth_ = vp->Width ? (int)vp->Width : 1; vpHeight_ = vp->Height ? (int)vp->Height : 1;
    KB_glDepthRange(vp->MinZ, vp->MaxZ);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::GetDeviceCaps(D3DCAPS9 *pCaps) {
    if (!pCaps) return E_INVALIDARG;
    FillDefaultCaps(pCaps);
    return D3D_OK;
}

// ---------------------------------------------------------------------------
// GLD3D9 (factory)
// ---------------------------------------------------------------------------
HRESULT WINAPI GLD3D9::GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9 *pIdentifier) {
    if (!pIdentifier) return E_INVALIDARG;
    *pIdentifier = D3DADAPTER_IDENTIFIER9{};
    // GL_VENDOR/GL_RENDERER need a current context. R_ChooseAdapter() queries this
    // BEFORE R_CreateGameWindow() creates the context, so there may be none yet.
    const GLubyte *renderer = nullptr;
    const GLubyte *vendor   = nullptr;
    // macOS's glGetString faults rather than returning null without a context.
    if (SDL_GL_GetCurrentContext())
    {
        renderer = glGetString(GL_RENDERER);
        vendor   = glGetString(GL_VENDOR);
    }
    snprintf(pIdentifier->Description, sizeof(pIdentifier->Description), "%s",
             renderer ? (const char *)renderer : "OpenGL Renderer");
    snprintf(pIdentifier->Driver, sizeof(pIdentifier->Driver), "%s",
             vendor ? (const char *)vendor : "OpenGL");
    return D3D_OK;
}

// The renderer lists modes and monitors before it creates the window, so SDL video
// is started here if nothing has started it yet.
static bool KB_VideoReady() {
    Sys_EnsureSDLVideo();
    return SDL_WasInit(SDL_INIT_VIDEO) != 0;
}

static int KB_AdapterDensity(UINT adapter) {
    return Sys_DisplayPixelDensity((int)adapter);
}

UINT WINAPI GLD3D9::GetAdapterCount() {
    const int n = KB_VideoReady() ? SDL_GetNumVideoDisplays() : 0;
    return n > 0 ? (UINT)n : 1;
}

// The desktop's mode, its size in pixels (a Retina display's are 2x its points).
static void KB_DesktopMode(UINT adapter, D3DDISPLAYMODE *mode) {
    SDL_DisplayMode dm;
    if (KB_VideoReady() && SDL_GetDesktopDisplayMode((int)adapter, &dm) == 0) {
        const int density = KB_AdapterDensity(adapter);
        mode->Width = (UINT)(dm.w * density); mode->Height = (UINT)(dm.h * density);
        mode->RefreshRate = (UINT)dm.refresh_rate;
    } else {
        mode->Width = 1920; mode->Height = 1080; mode->RefreshRate = 60;
    }
    mode->Format = D3DFMT_X8R8G8B8;
}

HRESULT WINAPI GLD3D9::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) {
    if (!pMode) return E_INVALIDARG;
    KB_DesktopMode(Adapter, pMode);
    return D3D_OK;
}

// The resolutions offered (r_mode): the sizes of the display's modes, and its own
// size in pixels, which a Retina display doesn't list. Fullscreen covers the display
// without switching its mode (Present scales), so each comes at the desktop's
// refresh rate: that is the rate the game runs at.
static std::vector<D3DDISPLAYMODE> g_kbAdapterModes;
static UINT g_kbAdapterModesOf = ~0u;

static void KB_ListAdapterModes(UINT adapter) {
    g_kbAdapterModes.clear();
    g_kbAdapterModesOf = adapter;
    D3DDISPLAYMODE desktop;
    KB_DesktopMode(adapter, &desktop);
    auto add = [](UINT w, UINT h, UINT hz) {
        for (const D3DDISPLAYMODE &m : g_kbAdapterModes)
            if (m.Width == w && m.Height == h) return;
        D3DDISPLAYMODE m = {};
        m.Width = w; m.Height = h; m.RefreshRate = hz; m.Format = D3DFMT_X8R8G8B8;
        g_kbAdapterModes.push_back(m);
    };
    add(desktop.Width, desktop.Height, desktop.RefreshRate);
    const int count = KB_VideoReady() ? SDL_GetNumDisplayModes((int)adapter) : 0;
    for (int i = 0; i < count; ++i) {
        SDL_DisplayMode dm;
        if (SDL_GetDisplayMode((int)adapter, i, &dm) == 0 && dm.w > 0 && dm.h > 0)
            add((UINT)dm.w, (UINT)dm.h, desktop.RefreshRate);
    }
}

UINT WINAPI GLD3D9::GetAdapterModeCount(UINT Adapter, D3DFORMAT) {
    KB_ListAdapterModes(Adapter);
    return (UINT)g_kbAdapterModes.size();
}

HRESULT WINAPI GLD3D9::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) {
    if (!pMode) return E_INVALIDARG;
    if (Adapter != g_kbAdapterModesOf || g_kbAdapterModes.empty())
        KB_ListAdapterModes(Adapter);
    if (Mode >= g_kbAdapterModes.size()) return D3DERR_INVALIDCALL;
    *pMode = g_kbAdapterModes[Mode];
    pMode->Format = Format;
    return D3D_OK;
}

HRESULT WINAPI GLD3D9::GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS9 *pCaps) {
    if (!pCaps) return E_INVALIDARG;
    FillDefaultCaps(pCaps);  // same caps the device reports
    return D3D_OK;
}

HRESULT WINAPI GLD3D9::CreateDevice(UINT Adapter, D3DDEVTYPE /*DeviceType*/,
                                    HWND /*hFocusWindow*/, DWORD /*BehaviorFlags*/,
                                    D3DPRESENT_PARAMETERS *pp,
                                    IDirect3DDevice9 **ppReturnedDeviceInterface) {
    if (!pp || !ppReturnedDeviceInterface) return E_INVALIDARG;
    *ppReturnedDeviceInterface = nullptr;

    GLContextDesc desc;
    desc.width        = pp->BackBufferWidth  ? (int)pp->BackBufferWidth  : 640;
    desc.height       = pp->BackBufferHeight ? (int)pp->BackBufferHeight : 480;
    desc.doubleBuffer = true;
    desc.depthStencil = pp->EnableAutoDepthStencil ? true : true;
    desc.visible      = pp->Windowed ? true : true;
    desc.fullscreen   = !pp->Windowed;
    desc.vsync        = pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE;
    desc.display      = (int)Adapter;

    GLContext *ctx = GLContext::Create(desc);
    if (!ctx) { fprintf(stderr, "[gl] CreateDevice: GL context creation failed\n"); return E_FAIL; }

    // The context is current: the sample count can be checked against the GPU now.
    const int samples = KB_BackbufferSamples(pp->MultiSampleType);
    if ((int)pp->MultiSampleType >= 2 && samples != (int)pp->MultiSampleType)
        fprintf(stderr, "[gl] %d samples asked, the GPU allows %d\n", (int)pp->MultiSampleType, KB_MaxSamples());
    *ppReturnedDeviceInterface = new GLDevice(ctx, desc.width, desc.height, samples, desc.fullscreen);
    return D3D_OK;
}

// The engine steps its sample count down until this accepts one
// (R_SetupAntiAliasing). Counts above GL_MAX_SAMPLES are refused once a context
// has told us the maximum; the first device creation clamps instead.
HRESULT WINAPI GLD3D9::CheckDeviceMultiSampleType(UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE Type, DWORD *pQ) {
    if (pQ) *pQ = 1;
    if ((int)Type >= 2 && (int)Type > KB_MaxSamples()) return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}

// ---------------------------------------------------------------------------
// Library entry point (replaces d3d9.dll's Direct3DCreate9 on non-Windows).
// ---------------------------------------------------------------------------
extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT /*SDKVersion*/) {
    return new GLD3D9();
}
