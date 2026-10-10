// gl_surface_ops.cpp — standalone surface creation + resolve/readback.
//
// CreateRenderTarget / CreateOffscreenPlainSurface build standalone GLSurfaces;
// GetRenderTargetData reads a GPU render target back into a system-memory surface
// (the screenshot path); StretchRect blits between renderable surfaces (resolve /
// downsample) via glBlitFramebuffer.
#include "gl_d3d9.h"
#include "gl_resources.h"
#include "gl_format.h"
#include "gl_spec.h"

const char *KB_SpecSurfKind(IDirect3DSurface9 *p);

#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>


// --- Back buffer / swap chain ----------------------------------------------
//
// The back buffer is a single GLSurface tagged as such (binding it as a render
// target binds the device's back-buffer FBO), and the swap chain is a thin shim
// whose Present() copies that FBO to the window. Both are created lazily and
// owned by the device.
GLSurface *GLDevice::backBufferSurface() {
    if (!backBuffer_)
        backBuffer_ = new GLSurface(this, (UINT)bbWidth_, (UINT)bbHeight_,
                                    D3DFMT_A8R8G8B8, GLBackbufferTag{});
    return backBuffer_;
}


HRESULT WINAPI GLDevice::GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **pp) {
    KB_SPEC("GetBackBuffer %s", KB_SpecThread());
    if (!pp) return E_INVALIDARG;
    GLSurface *bb = backBufferSurface();
    bb->AddRef();
    *pp = bb;
    return D3D_OK;
}

HRESULT WINAPI GLDevice::GetSwapChain(UINT, IDirect3DSwapChain9 **pp) {
    KB_SPEC("GetSwapChain %s", KB_SpecThread());
    if (!pp) return E_INVALIDARG;
    if (!swapChain_)
        swapChain_ = new GLSwapChain(this, backBufferSurface());
    swapChain_->AddRef();
    *pp = swapChain_;
    return D3D_OK;
}

// Read the last presented frame (the back-buffer FBO keeps it after Present) into a
// system-memory surface as BGRA8 — the screenshot path: R_TakeScreenshot creates an
// offscreen A8R8G8B8 surface, calls this, then D3DXSaveSurfaceToFileA writes a TGA.
// The FBO holds D3D row order, so the rows arrive top-down like a D3D surface's.
HRESULT WINAPI GLSwapChain::GetFrontBufferData(IDirect3DSurface9 *pDestSurface) {
    GLSurface *dst = static_cast<GLSurface *>(pDestSurface);
    KB_SPEC("GetFrontBufferData %s %s fmt=%u", KB_SpecThread(), KB_SpecSurfKind(pDestSurface), dst ? (unsigned)dst->format() : 0u);
    if (!dst) return E_FAIL;
    UINT w = dst->width(), h = dst->height();
    std::vector<unsigned char> &shadow = dst->shadow();
    if (shadow.size() < (size_t)w * h * 4) shadow.assign((size_t)w * h * 4, 0);
    GLDevice *dev = static_cast<GLDevice *>(device_);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, dev->resolvedBackbufferFbo());
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA, GL_UNSIGNED_BYTE, shadow.data());
    glBindFramebuffer(GL_FRAMEBUFFER, dev->curFbo());
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format,
                                            D3DMULTISAMPLE_TYPE ms, DWORD, BOOL lockable,
                                            IDirect3DSurface9 **ppSurface, HANDLE *) {
    KB_SPEC("CreateRenderTarget %s fmt=%u ms=%u lockable=%d", KB_SpecThread(), (unsigned)Format, (unsigned)ms, (int)lockable);
    if (!ppSurface) return E_INVALIDARG;
    *ppSurface = new GLSurface(this, Width, Height, Format, /*sysmem=*/false);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                     D3DPOOL pool, IDirect3DSurface9 **ppSurface, HANDLE *) {
    KB_SPEC("CreateOffscreenPlainSurface %s fmt=%u pool=%u", KB_SpecThread(), (unsigned)Format, (unsigned)pool);
    if (!ppSurface) return E_INVALIDARG;
    *ppSurface = new GLSurface(this, Width, Height, Format, /*sysmem=*/true);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                   D3DMULTISAMPLE_TYPE ms, DWORD, BOOL discard,
                                                   IDirect3DSurface9 **ppSurface, HANDLE *) {
    KB_SPEC("CreateDepthStencilSurface %s fmt=%u ms=%u discard=%d", KB_SpecThread(), (unsigned)Format, (unsigned)ms, (int)discard);
    if (!ppSurface) return E_INVALIDARG;
    *ppSurface = new GLSurface(this, Width, Height, Format, GLDepthStencilTag{});
    return D3D_OK;
}

// Read a render target back into a system-memory surface for CPU access.
HRESULT WINAPI GLDevice::GetRenderTargetData(IDirect3DSurface9 *pRenderTarget,
                                             IDirect3DSurface9 *pDestSurface) {
    GLSurface *src = static_cast<GLSurface *>(pRenderTarget);
    GLSurface *dst = static_cast<GLSurface *>(pDestSurface);
    KB_SPEC("GetRenderTargetData %s %s fmt=%u -> %s fmt=%u", KB_SpecThread(), KB_SpecSurfKind(pRenderTarget),
            src ? (unsigned)src->format() : 0u, KB_SpecSurfKind(pDestSurface), dst ? (unsigned)dst->format() : 0u);
    if (!src || !dst || !src->texName()) return E_FAIL;

    unsigned internal, fmt, type; int bpp;
    if (!D3DToGLFormat(dst->format(), &internal, &fmt, &type, &bpp)) return E_FAIL;

    UINT w = src->width() < dst->width() ? src->width() : dst->width();
    UINT h = src->height() < dst->height() ? src->height() : dst->height();
    if (dst->shadow().size() < (size_t)dst->width() * dst->height() * bpp)
        dst->shadow().assign((size_t)dst->width() * dst->height() * bpp, 0);

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           src->texName(), src->level());
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, fmt, type, dst->shadow().data());
    glBindFramebuffer(GL_FRAMEBUFFER, curFbo());   // restore the active RT
    glDeleteFramebuffers(1, &fbo);
    return D3D_OK;
}

// Blit between two renderable surfaces (resolve / scale). Either side may be the
// back buffer (FBO 0) — e.g. capturing the frame-buffer render target into an image
// for refraction/feedback — so the back buffer binds framebuffer 0 directly instead
// of a texture-attachment FBO.
HRESULT WINAPI GLDevice::StretchRect(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect,
                                     IDirect3DSurface9 *pDestSurface, const RECT *pDestRect,
                                     D3DTEXTUREFILTERTYPE Filter) {
    extern unsigned long g_kbBlits; ++g_kbBlits;
    GLSurface *src = static_cast<GLSurface *>(pSourceSurface);
    GLSurface *dst = static_cast<GLSurface *>(pDestSurface);
    if (g_kbSpec && src && dst) {
        const int sw = pSourceRect ? pSourceRect->right - pSourceRect->left : (int)src->width();
        const int sh = pSourceRect ? pSourceRect->bottom - pSourceRect->top : (int)src->height();
        const int dw = pDestRect ? pDestRect->right - pDestRect->left : (int)dst->width();
        const int dh = pDestRect ? pDestRect->bottom - pDestRect->top : (int)dst->height();
        KB_SpecNote("StretchRect %s %s fmt=%u -> %s fmt=%u filter=%u srcrect=%d dstrect=%d scaled=%d", KB_SpecThread(),
                    KB_SpecSurfKind(pSourceSurface), (unsigned)src->format(), KB_SpecSurfKind(pDestSurface), (unsigned)dst->format(),
                    (unsigned)Filter, pSourceRect != nullptr, pDestRect != nullptr, (int)(sw != dw || sh != dh));
    }
    if (!src || !dst) return E_FAIL;
    if ((!src->isBackbuffer() && !src->texName()) ||
        (!dst->isBackbuffer() && !dst->texName())) return E_FAIL;

    int sx0 = 0, sy0 = 0, sx1 = (int)src->width(), sy1 = (int)src->height();
    int dx0 = 0, dy0 = 0, dx1 = (int)dst->width(), dy1 = (int)dst->height();
    if (pSourceRect) { sx0 = pSourceRect->left; sy0 = pSourceRect->top; sx1 = pSourceRect->right; sy1 = pSourceRect->bottom; }
    if (pDestRect)   { dx0 = pDestRect->left;   dy0 = pDestRect->top;   dx1 = pDestRect->right;   dy1 = pDestRect->bottom; }

    {
        extern int g_kbTrace;
        if (g_kbTrace) {
            fprintf(stderr, "[trace] StretchRect src=%p bb=%d tex=%u %dx%d -> dst=%p bb=%d tex=%u %dx%d\n",
                    (void *)src, (int)src->isBackbuffer(), src->texName(), sx1 - sx0, sy1 - sy0,
                    (void *)dst, (int)dst->isBackbuffer(), dst->texName(), dx1 - dx0, dy1 - dy0);
            static int dumped;
            const char *dir = getenv("KB_SCREENSHOT");
            if (src->isBackbuffer() && dir && dumped < 16) {
                extern void KB_WriteBackbufferTGA(unsigned fbo, const char *path, int w, int h);
                char path[1024];
                snprintf(path, sizeof(path), "%s/resolve_%d.tga", dir, dumped++);
                KB_WriteBackbufferTGA(resolvedBackbufferFbo(), path, (int)src->width(), (int)src->height());
            }
        }
    }
    // Both sides hold D3D row order, so D3D rects map straight onto GL rows.
    if (!blitFbo_[0]) glGenFramebuffers(2, blitFbo_);
    if (src->isBackbuffer()) {
        // A multisampled back buffer resolves straight into an RGBA8 target of a 1:1
        // rectangle (the scene resolves, several a frame); anything else reads the
        // single-sample copy.
        const bool direct = !dst->isBackbuffer() && sx0 == dx0 && sy0 == dy0 && sx1 == dx1 && sy1 == dy1 &&
                            (dst->format() == D3DFMT_A8R8G8B8 || dst->format() == D3DFMT_A8B8G8R8);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, direct ? backbufferFbo() : resolvedBackbufferFbo());
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, blitFbo_[0]);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src->texName(), src->level());
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }
    if (scissorOn_) glDisable(GL_SCISSOR_TEST);   // D3D's StretchRect ignores the scissor; GL blits don't
    if (dst->isBackbuffer()) {
        blitToBackbuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, Filter == D3DTEXF_NONE ? GL_NEAREST : GL_LINEAR);
    } else {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blitFbo_[1]);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst->texName(), dst->level());
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT,
                          Filter == D3DTEXF_NONE ? GL_NEAREST : GL_LINEAR);
    }
    if (scissorOn_) glEnable(GL_SCISSOR_TEST);
    // RESTORE the device's ACTIVE render target (binds both READ and DRAW): leaving the
    // draw framebuffer at 0 sent every draw between a mid-pass resolve and the next
    // SetRenderTarget to the WRONG framebuffer — invisible geometry for the rest of the
    // pass, varying with FX/command timing (the world flicker candidate).
    glBindFramebuffer(GL_FRAMEBUFFER, curFbo());
    return D3D_OK;
}
