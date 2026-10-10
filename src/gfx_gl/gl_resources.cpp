// gl_resources.cpp — GL-backed vertex/index buffers + vertex declaration.
#include "gl_resources.h"
#include "gl_format.h"
#include "gl_spec.h"

#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Shadow stack: default ON (KB_SHADOWS=0 disables). Gates depth-texture
// creation, SetDepthStencilSurface honoring and the sampler2DShadow variants in one
// switch. OFF is broken-by-design now: the engine still renders+samples the shadowmap,
// and without real depth storage the sampled garbage VARIES with worker timing per
// frame = flickering sun-shadow factors on world surfaces.
extern "C" int KB_ShadowsEnabled() {
    static int en = -1;
    if (en < 0) { const char *v = getenv("KB_SHADOWS"); en = (v && *v == '0') ? 0 : 1; }
    return en;
}

// ---- GLVertexBuffer -------------------------------------------------------
GLVertexBuffer::GLVertexBuffer(IDirect3DDevice9 *device, UINT length, DWORD usage,
                               DWORD fvf, D3DPOOL pool)
    : device_(device), length_(length), usage_(usage), fvf_(fvf), pool_(pool),
      shadow_(length, 0) {
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, length_,
                 nullptr, (usage_ & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// Apple's GL on Metal turns a glBufferSubData into a buffer the GPU may still read
// into a blit on a fresh Metal command buffer, and blocks once those run out; one per
// dynamic index upload held a match to ~6 fps. NOOVERWRITE and DISCARD locks promise
// the range is not in use (DISCARD has just orphaned the storage), so write those
// through an unsynchronized mapping.
extern unsigned long g_kbUploads;
extern unsigned long long g_kbUploadBytes, g_kbOrphanBytes;
static void KB_BufferUpload(GLenum target, GLintptr off, GLsizeiptr size, const void *src, bool synced) {
    ++g_kbUploads;
    g_kbUploadBytes += size;
    if (!synced) {
        void *dst = glMapBufferRange(target, off, size,
                                     GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
        if (dst) {
            memcpy(dst, src, size);
            if (glUnmapBuffer(target)) return;
        }
    }
    glBufferSubData(target, off, size, src);
}

void GLVertexBuffer::sync() {
    UINT upMin, upMax;
    bool upDiscard, upSync;
    {
        std::lock_guard<std::mutex> g(lockMu_);
        upMin = pendMin_; upMax = pendMax_; upDiscard = pendDiscard_; upSync = pendSync_;
        pendMin_ = ~0u; pendMax_ = 0; pendDiscard_ = false; pendSync_ = false;
    }
    if (!vbo_) {
        // First touch on the GL thread: create and upload the whole shadow (covers any
        // pending ranges in one go).
        glGenBuffers(1, &vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, length_, shadow_.data(),
                     (usage_ & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else if (upMax > upMin) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        if (upDiscard)
            glBufferData(GL_ARRAY_BUFFER, length_, nullptr, GL_DYNAMIC_DRAW);
        KB_BufferUpload(GL_ARRAY_BUFFER, upMin, upMax - upMin, shadow_.data() + upMin, upSync);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
}

// Selective VAO-cache invalidation: note the dead buffer/decl so only the cache entries that
// reference it are dropped (the wholesale clear on every death was the periodic stutter).
extern "C" void KB_VaoNoteDeadBuf(unsigned name);
extern "C" void KB_VaoNoteDeadDecl(const void *decl);
GLVertexBuffer::~GLVertexBuffer() {
    if (vbo_) { KB_VaoNoteDeadBuf(vbo_); glDeleteBuffers(1, &vbo_); }
}

HRESULT WINAPI GLVertexBuffer::GetDevice(IDirect3DDevice9 **ppDevice) {
    KB_SPEC("GetDevice VertexBuffer %s", KB_SpecThread());
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_;
    if (device_) device_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI GLVertexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
    if (!ppbData) return E_INVALIDARG;
    KB_SPEC("LockVB %s usage=%x flags=%x off0=%d size0=%d used=%d", KB_SpecThread(), (unsigned)usage_, (unsigned)Flags,
            OffsetToLock == 0, SizeToLock == 0, (int)specUsed);
    if (g_kbSpec) {
        extern unsigned long g_kbPresentEnter;
        if (Flags & D3DLOCK_DISCARD) specDiscardPresent = g_kbPresentEnter;
        else if ((Flags & D3DLOCK_NOOVERWRITE) && specDiscardPresent != g_kbPresentEnter)
            KB_SpecNote("LockVB NOOVERWRITE after a DISCARD in an earlier frame %s", specDiscardPresent == ~0ul ? "(never)" : "");
    }
    if (SizeToLock == 0) SizeToLock = length_ - OffsetToLock;  // 0 means "to end" in D3D
    {
        // Locks legitimately overlap across threads (frontend appends frame N+1 into the
        // shared dynamic ring while the backend tess-fills frame N) — accumulate a UNION
        // of outstanding ranges instead of single-slot fields a second Lock would clobber.
        std::lock_guard<std::mutex> g(lockMu_);
        ++lockDepth_;
        if (!(Flags & D3DLOCK_READONLY)) {
            if (OffsetToLock < outMin_) outMin_ = OffsetToLock;
            if (OffsetToLock + SizeToLock > outMax_) outMax_ = OffsetToLock + SizeToLock;
            if (Flags & D3DLOCK_DISCARD) outDiscard_ = true;
            if (!(Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) outSync_ = true;
        }
    }
    *ppbData = shadow_.data() + OffsetToLock;
    return D3D_OK;
}

HRESULT WINAPI GLVertexBuffer::Unlock() {
    KB_SPEC("UnlockVB %s", KB_SpecThread());
    // FLUSH ONLY ON DISCARD: a DISCARD lock orphans the buffer (new storage), so any
    // pending batched draws that reference the OLD storage by offset would render
    // garbage — flush them first. A NOOVERWRITE lock appends to a fresh region and
    // leaves prior data intact, so batched draws stay valid; NOT flushing there lets
    // same-material dynamic geometry collapse into one merged draw (the big GL-call cut
    // — dynamic-buffer unlocks were the #1 batch-breaker, ~1900 flushes/frame).
    UINT upMin = ~0u, upMax = 0;
    bool upDiscard = false, upSync = false, uploadNow = false;
    {
        std::lock_guard<std::mutex> g(lockMu_);
        if (lockDepth_) --lockDepth_;
        // Fold the whole outstanding union into the pending range at EVERY unlock: the
        // other thread's still-open range may upload half-written, but its own Unlock
        // re-folds it, so the final upload always carries complete data.
        if (outDiscard_) pendDiscard_ = true;
        if (outSync_) pendSync_ = true;
        if (outMin_ < pendMin_) pendMin_ = outMin_;
        if (outMax_ > pendMax_) pendMax_ = outMax_;
        if (!lockDepth_) { outMin_ = ~0u; outMax_ = 0; outDiscard_ = false; outSync_ = false; }
        // DYNAMIC buffers defer even on the GL thread when coalescing: the engine does
        // many small Lock/Unlock cycles per frame (FX quads, skinned chunks); folding
        // them into the pending range and uploading ONCE at next bind replaces N
        // bind+upload call pairs with one (the in-game bufKB/f was ~11MB across
        // hundreds of calls). Off the GL thread sync() replays it at next bind.
        uploadNow = vbo_
                    ;
        if (uploadNow) {
            upMin = pendMin_; upMax = pendMax_; upDiscard = pendDiscard_; upSync = pendSync_;
            pendMin_ = ~0u; pendMax_ = 0; pendDiscard_ = false; pendSync_ = false;
        }
    }
    if (uploadNow && upMax > upMin) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        // D3DLOCK_DISCARD = "I'm overwriting the whole buffer, give me fresh storage."
        // Orphan first (glBufferData NULL) so the upload does NOT block on the GPU still
        // reading last frame's geometry from this VBO — without it glBufferSubData forces
        // an implicit sync (the main per-frame dynamic-mesh stall). The engine only
        // DISCARDs at offset 0 (see rb_backend/r_shade), so the orphaned tail is never drawn.
        if (upDiscard) {
            glBufferData(GL_ARRAY_BUFFER, length_, nullptr, GL_DYNAMIC_DRAW);
            g_kbOrphanBytes += length_;
        }
        KB_BufferUpload(GL_ARRAY_BUFFER, upMin, upMax - upMin, shadow_.data() + upMin, upSync);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    return D3D_OK;
}

HRESULT WINAPI GLVertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *pDesc) {
    KB_SPEC("VertexBuffer GetDesc %s", KB_SpecThread());
    if (!pDesc) return E_INVALIDARG;
    *pDesc = D3DVERTEXBUFFER_DESC{};
    pDesc->Format = D3DFMT_UNKNOWN; pDesc->Type = D3DRTYPE_VERTEXBUFFER;
    pDesc->Usage = usage_; pDesc->Pool = pool_; pDesc->Size = length_; pDesc->FVF = fvf_;
    return D3D_OK;
}

// ---- GLIndexBuffer --------------------------------------------------------
GLIndexBuffer::GLIndexBuffer(IDirect3DDevice9 *device, UINT length, DWORD usage,
                             D3DFORMAT format, D3DPOOL pool)
    : device_(device), length_(length), usage_(usage), format_(format), pool_(pool),
      shadow_(length, 0) {
    glGenBuffers(1, &ibo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, length_,
                 nullptr, (usage_ & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void GLIndexBuffer::sync() {
    UINT upMin, upMax;
    bool upDiscard, upSync;
    {
        std::lock_guard<std::mutex> g(lockMu_);
        upMin = pendMin_; upMax = pendMax_; upDiscard = pendDiscard_; upSync = pendSync_;
        pendMin_ = ~0u; pendMax_ = 0; pendDiscard_ = false; pendSync_ = false;
    }
    // Uploads go through GL_COPY_WRITE_BUFFER: the ELEMENT_ARRAY binding is VAO
    // STATE, so binding here silently rewrote (then zeroed) the element binding of
    // whatever VAO was current — the per-VAO bind-skip cache then drew with the
    // WRONG index buffer (the spazzing-triangle corruption under the VAO cache).
    if (!ibo_) {
        glGenBuffers(1, &ibo_);
        glBindBuffer(GL_COPY_WRITE_BUFFER, ibo_);
        glBufferData(GL_COPY_WRITE_BUFFER, length_, shadow_.data(),
                     (usage_ & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    } else if (upMax > upMin) {
        glBindBuffer(GL_COPY_WRITE_BUFFER, ibo_);
        if (upDiscard)
            glBufferData(GL_COPY_WRITE_BUFFER, length_, nullptr, GL_DYNAMIC_DRAW);
        KB_BufferUpload(GL_COPY_WRITE_BUFFER, upMin, upMax - upMin, shadow_.data() + upMin, upSync);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }
}

GLIndexBuffer::~GLIndexBuffer() {
    if (ibo_) { KB_VaoNoteDeadBuf(ibo_); glDeleteBuffers(1, &ibo_); }
}

HRESULT WINAPI GLIndexBuffer::GetDevice(IDirect3DDevice9 **ppDevice) {
    KB_SPEC("GetDevice IndexBuffer %s", KB_SpecThread());
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_;
    if (device_) device_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI GLIndexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
    if (!ppbData) return E_INVALIDARG;
    KB_SPEC("LockIB %s usage=%x flags=%x off0=%d size0=%d used=%d", KB_SpecThread(), (unsigned)usage_, (unsigned)Flags,
            OffsetToLock == 0, SizeToLock == 0, (int)specUsed);
    if (g_kbSpec) {
        extern unsigned long g_kbPresentEnter;
        if (Flags & D3DLOCK_DISCARD) specDiscardPresent = g_kbPresentEnter;
        else if ((Flags & D3DLOCK_NOOVERWRITE) && specDiscardPresent != g_kbPresentEnter)
            KB_SpecNote("LockIB NOOVERWRITE after a DISCARD in an earlier frame %s", specDiscardPresent == ~0ul ? "(never)" : "");
    }
    if (SizeToLock == 0) SizeToLock = length_ - OffsetToLock;
    {
        // See GLVertexBuffer::Lock — outstanding-range UNION; concurrent FE/BE locks.
        std::lock_guard<std::mutex> g(lockMu_);
        ++lockDepth_;
        if (!(Flags & D3DLOCK_READONLY)) {
            if (OffsetToLock < outMin_) outMin_ = OffsetToLock;
            if (OffsetToLock + SizeToLock > outMax_) outMax_ = OffsetToLock + SizeToLock;
            if (Flags & D3DLOCK_DISCARD) outDiscard_ = true;
            if (!(Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) outSync_ = true;
        }
    }
    *ppbData = shadow_.data() + OffsetToLock;
    return D3D_OK;
}

HRESULT WINAPI GLIndexBuffer::Unlock() {
    KB_SPEC("UnlockIB %s", KB_SpecThread());
    // Flush pending batched draws only on DISCARD (orphan) — see GLVertexBuffer::Unlock.
    UINT upMin = ~0u, upMax = 0;
    bool upDiscard = false, upSync = false, uploadNow = false;
    {
        std::lock_guard<std::mutex> g(lockMu_);
        if (lockDepth_) --lockDepth_;
        if (outDiscard_) pendDiscard_ = true;
        if (outSync_) pendSync_ = true;
        if (outMin_ < pendMin_) pendMin_ = outMin_;
        if (outMax_ > pendMax_) pendMax_ = outMax_;
        if (!lockDepth_) { outMin_ = ~0u; outMax_ = 0; outDiscard_ = false; outSync_ = false; }
        uploadNow = ibo_
                    ;
        if (uploadNow) {
            upMin = pendMin_; upMax = pendMax_; upDiscard = pendDiscard_; upSync = pendSync_;
            pendMin_ = ~0u; pendMax_ = 0; pendDiscard_ = false; pendSync_ = false;
        }
    }
    if (uploadNow && upMax > upMin) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
        // See GLVertexBuffer::Unlock — orphan on DISCARD to avoid the GPU sync stall.
        if (upDiscard) {
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, length_, nullptr, GL_DYNAMIC_DRAW);
            g_kbOrphanBytes += length_;
        }
        KB_BufferUpload(GL_ELEMENT_ARRAY_BUFFER, upMin, upMax - upMin, shadow_.data() + upMin, upSync);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
    return D3D_OK;
}

HRESULT WINAPI GLIndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc) {
    KB_SPEC("IndexBuffer GetDesc %s", KB_SpecThread());
    if (!pDesc) return E_INVALIDARG;
    *pDesc = D3DINDEXBUFFER_DESC{};
    pDesc->Format = format_; pDesc->Type = D3DRTYPE_INDEXBUFFER;
    pDesc->Usage = usage_; pDesc->Pool = pool_; pDesc->Size = length_;
    return D3D_OK;
}

// ---- GLTexture ------------------------------------------------------------
// D3D's CreateTexture(Levels=0) means "full mip chain"; compute it from the size.
static UINT FullMipCount(UINT w, UINT h) {
    UINT m = (w > h) ? w : h, n = 1;
    while (m > 1) { m >>= 1; ++n; }
    return n;
}

GLTexture::GLTexture(IDirect3DDevice9 *device, UINT width, UINT height, UINT levels,
                     DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : device_(device), width_(width), height_(height),
      levels_(levels ? levels : FullMipCount(width, height)), usage_(usage), format_(format), pool_(pool) {
    levelShadow_.resize(levels_);
    createGL();
}

// Allocate RENDERABLE, completeness-verified level-0 storage for a render-target
// texture. D3DToGLFormat maps the accurate SAMPLING format, but several D3D RT formats
// (notably A16B16G16R16 -> GL_RGBA16 unorm) were not color-renderable on the WebGL
// target this layer started on: such an FBO is GL_FRAMEBUFFER_UNSUPPORTED (0x8cdd) and
// every draw to it no-ops (a black HDR scene and post-fx pyramid). Try renderable
// candidates (HDR float, then RGBA8) and keep the first that yields a COMPLETE FBO.
void KB_AllocRenderableRT(unsigned tex, UINT w, UINT h, D3DFORMAT d3dfmt) {
    unsigned internal = 0, fmt = 0, type = 0; int bpp = 0;
    bool mapped = D3DToGLFormat(d3dfmt, &internal, &fmt, &type, &bpp);
    struct Cand { unsigned internal, fmt, type; };
    Cand cands[3]; int nc = 0;
    if (mapped && internal != GL_RGBA16)                     // accurate map, if renderable
        cands[nc++] = { internal, fmt, type };
    cands[nc++] = { GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT };    // HDR (renderable via EXT_color_buffer_float)
    cands[nc++] = { GL_RGBA8,   GL_RGBA, GL_UNSIGNED_BYTE }; // always-renderable last resort

    glBindTexture(GL_TEXTURE_2D, tex);
    GLuint probe = 0; glGenFramebuffers(1, &probe);
    GLint prevFbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, probe);
    int chosen = nc - 1;
    for (int i = 0; i < nc; ++i) {
        while (glGetError() != GL_NO_ERROR) {}
        glTexImage2D(GL_TEXTURE_2D, 0, cands[i].internal, w, h, 0, cands[i].fmt, cands[i].type, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glGetError() == GL_NO_ERROR && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) { chosen = i; break; }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glDeleteFramebuffers(1, &probe);
    static int rtN = 0;
    if (++rtN <= 12)
        fprintf(stderr, "[gl] RT %ux%u d3dfmt=%u -> internal=0x%x\n", w, h, (unsigned)d3dfmt, cands[chosen].internal);
}

void GLTexture::createGL() {
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels_ - 1);
    // DEPTH textures (the hardware-shadow path): the engine creates the shadow map as a
    // depth-format texture, renders into it via SetDepthStencilSurface, then SAMPLES it
    // with depth comparison (texldp). Allocate real depth storage and enable compare mode
    // so a sampler2DShadow lookup returns the PCF comparison result like D3D9 hardware
    // shadows do. (The old renderbuffer-backed path made every shadow read garbage ->
    // the long-standing black gun/surfaces bug.)
    {
        unsigned internal = 0, fmt = 0, type = 0, attach = 0;
        switch ((unsigned)format_) {
            case 75: /*D3DFMT_D24S8*/ case 77: /*D3DFMT_D24X8*/ case 79: /*D3DFMT_D24FS8*/
                internal = GL_DEPTH24_STENCIL8; fmt = GL_DEPTH_STENCIL;
                type = GL_UNSIGNED_INT_24_8; attach = GL_DEPTH_STENCIL_ATTACHMENT; break;
            case 80: /*D3DFMT_D16*/ case 70: /*D3DFMT_D16_LOCKABLE*/
                internal = GL_DEPTH_COMPONENT16; fmt = GL_DEPTH_COMPONENT;
                type = GL_UNSIGNED_SHORT; attach = GL_DEPTH_ATTACHMENT; break;
            case 71: /*D3DFMT_D32*/ case 73: /*D3DFMT_D24X4S4*/
                internal = GL_DEPTH_COMPONENT24; fmt = GL_DEPTH_COMPONENT;
                type = GL_UNSIGNED_INT; attach = GL_DEPTH_ATTACHMENT; break;
        }
        // D3DUSAGE_DEPTHSTENCIL is the authoritative signal: the engine's shadowmap
        // texture arrives with a GARBAGE format (gfxMetrics.shadowmapFormat* reads
        // misaligned in the decompile — observed 374936), which fell through to color
        // storage -> attached as depth -> FBO incomplete -> black passes (B59 run).
        if (!internal && (usage_ & D3DUSAGE_DEPTHSTENCIL)) {
            internal = GL_DEPTH24_STENCIL8; fmt = GL_DEPTH_STENCIL;
            type = GL_UNSIGNED_INT_24_8; attach = GL_DEPTH_STENCIL_ATTACHMENT;
        }
        if (internal && KB_ShadowsEnabled()) {
            isDepth_ = true;
            glTexImage2D(GL_TEXTURE_2D, 0, internal, width_, height_, 0, fmt, type, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE /* manual-PCF engine: raw depth reads */);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
            fprintf(stderr, "[gl] depth texture %ux%u fmt=%u (shadow map)\n", width_, height_, (unsigned)format_);
            return;
        }
    }
    // A render-target texture is never Lock/Unlocked, so allocate level-0 storage now
    // to make it complete for FBO colour attachment. CRITICAL: the storage must be a
    // RENDERABLE format. D3DToGLFormat maps A16B16G16R16 -> GL_RGBA16 (the accurate
    // sampling format), which the WebGL target couldn't render to (only RGBA16F), so
    // the HDR/bloom RTs went black. Pick a renderable internalformat and VERIFY
    // completeness, falling back until one works (KB_AllocRenderableRT).
    if (usage_ & D3DUSAGE_RENDERTARGET) {
        KB_AllocRenderableRT(tex_, width_, height_, format_);
        glBindTexture(GL_TEXTURE_2D, tex_);
    } else {
        // Give every texture a neutral 1x1 level-0 immediately so it is COMPLETE even
        // before (or if) the engine uploads its pixels — otherwise a not-yet-uploaded
        // (e.g. streamed) texture is incomplete and samples as a debug colour (magenta
        // on Mesa). The real LockRect/UnlockRect redefines level 0 with the true data.
        static const unsigned char gray[4] = { 128, 128, 128, 255 };
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, gray);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GLTexture::ensureDepthStorage() {
    if (isDepth_) return;
    if (!tex_) createGL();
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width_, height_, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE /* manual-PCF engine: raw depth reads */);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    isDepth_ = true;
    fprintf(stderr, "[gl] retrofitted %ux%u texture to depth storage (shadow DS)\n", width_, height_);
}

GLTexture::~GLTexture() { if (tex_) glDeleteTextures(1, &tex_); }

HRESULT WINAPI GLTexture::GetDevice(IDirect3DDevice9 **ppDevice) {
    KB_SPEC("GetDevice Texture %s", KB_SpecThread());
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_;
    if (device_) device_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI GLTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
    KB_SPEC("Texture GetLevelDesc %s", KB_SpecThread());
    if (!pDesc || Level >= levels_) return E_INVALIDARG;
    *pDesc = D3DSURFACE_DESC{};
    pDesc->Format = format_; pDesc->Type = D3DRTYPE_SURFACE; pDesc->Usage = usage_; pDesc->Pool = pool_;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    pDesc->Width  = width_  >> Level ? width_  >> Level : 1;
    pDesc->Height = height_ >> Level ? height_ >> Level : 1;
    return D3D_OK;
}

HRESULT WINAPI GLTexture::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) {
    KB_SPEC("GetSurfaceLevel %s level=%s usage=%x fmt=%u", KB_SpecThread(), Level ? "n" : "0", (unsigned)usage_, (unsigned)format_);
    if (!ppSurfaceLevel || Level >= levels_) return E_INVALIDARG;
    *ppSurfaceLevel = new GLSurface(this, Level);
    return D3D_OK;
}

HRESULT WINAPI GLTexture::LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
    if (!pLockedRect || Level >= levels_) return E_INVALIDARG;
    KB_SPEC("LockRect %s fmt=%u usage=%x pool=%u level=%s flags=%x rect=%d used=%d", KB_SpecThread(), (unsigned)format_,
            (unsigned)usage_, (unsigned)pool_, Level ? "n" : "0", (unsigned)Flags, pRect != nullptr, (int)specUsed);
    UINT w = width_  >> Level ? width_  >> Level : 1;
    UINT h = height_ >> Level ? height_ >> Level : 1;
    int blockBytes = 0;
    if (D3DCompressedGLFormat(format_, &blockBytes)) {
        // DXT/BC: the lock surface is a grid of 4x4 blocks; Pitch is bytes per block row.
        UINT bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (levelShadow_[Level].size() != (size_t)bw * bh * blockBytes) levelShadow_[Level].assign((size_t)bw * bh * blockBytes, 0);  // preserve: D3D9 Lock keeps contents
        pLockedRect->Pitch = (int)(bw * blockBytes);
    } else {
        int bpp = D3DFormatBpp(format_);
        if (levelShadow_[Level].size() != (size_t)w * h * bpp) levelShadow_[Level].assign((size_t)w * h * bpp, 0);  // preserve
        pLockedRect->Pitch = (int)(w * bpp);
    }
    lockLevel_ = Level;
    dirty_     = true;
    pLockedRect->pBits = levelShadow_[Level].data();
    return D3D_OK;
}

HRESULT WINAPI GLTexture::UnlockRect(UINT Level) {
    KB_SPEC("UnlockRect %s", KB_SpecThread());
    if (Level >= levels_ || !dirty_) return D3D_OK;
    dirty_ = false;
    if (!tex_) {
        pendLevels_ |= 1u << Level;  // replayed by sync() at next bind on the GL thread
        return D3D_OK;
    }
    uploadLevel(Level);
    return D3D_OK;
}

void GLTexture::sync() {
    if (!tex_) createGL();
    if (pendLevels_) {
        unsigned p = pendLevels_;
        pendLevels_ = 0;
        for (UINT L = 0; L < levels_ && p; ++L, p >>= 1)
            if (p & 1) uploadLevel(L);
    }
}

void GLTexture::uploadLevel(UINT Level) {
    UINT w = width_  >> Level ? width_  >> Level : 1;
    UINT h = height_ >> Level ? height_ >> Level : 1;
    // One-time GPU capability report — tells us if S3TC (DXT) uploads can work.
    static bool reported = false;
    if (!reported) {
        reported = true;
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        bool s3tc = ext && (strstr(ext, "texture_compression_s3tc") ||
                            strstr(ext, "compressed_texture_s3tc"));
        // A core profile (macOS) has no GL_EXTENSIONS string; list them one by one.
        if (!ext) {
            while (glGetError() != GL_NO_ERROR) {}
            GLint count = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &count);
            for (GLint i = 0; i < count && !s3tc; ++i) {
                const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
                s3tc = e && strstr(e, "texture_compression_s3tc");
            }
        }
        fprintf(stderr, "[gl] renderer=%s | GL=%s | S3TC=%s\n",
                glGetString(GL_RENDERER), glGetString(GL_VERSION), s3tc ? "YES" : "NO");
    }
    glBindTexture(GL_TEXTURE_2D, tex_);
    while (glGetError() != GL_NO_ERROR) {}  // drain prior errors
    int blockBytes = 0; unsigned cfmt = D3DCompressedGLFormat(format_, &blockBytes);
    if (cfmt) {
        glCompressedTexImage2D(GL_TEXTURE_2D, Level, cfmt, w, h, 0,
                               (GLsizei)levelShadow_[Level].size(), levelShadow_[Level].data());
    } else {
        unsigned internal, format, type; int bpp;
        if (!D3DToGLFormat(format_, &internal, &format, &type, &bpp)) {
            fprintf(stderr, "[gl] GLTexture: unsupported format 0x%x (level not uploaded)\n", format_);
            glBindTexture(GL_TEXTURE_2D, 0); return;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const void *pixels = levelShadow_[Level].data();
        glTexImage2D(GL_TEXTURE_2D, Level, internal, w, h, 0, format, type, pixels);
        if (Level == 0)
            D3DApplyFormatSwizzle(GL_TEXTURE_2D, format_);
    }
    GLenum uerr = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    if (uerr != GL_NO_ERROR)
        fprintf(stderr, "[gl] texture upload error 0x%x: L%u fmt=0x%x %ux%u %s\n",
                uerr, Level, (unsigned)format_, w, h, cfmt ? "DXT" : "raw");
}

// ---- GLVolumeTexture (GL_TEXTURE_3D) --------------------------------------
GLVolumeTexture::GLVolumeTexture(IDirect3DDevice9 *device, UINT w, UINT h, UINT d, UINT levels,
                                 DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : device_(device), width_(w), height_(h), depth_(d),
      levels_(levels ? levels : 1), usage_(usage), format_(format), pool_(pool) {
    levelShadow_.resize(levels_);
    createGL();
}

void GLVolumeTexture::createGL() {
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_3D, tex_);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, levels_ - 1);
    glBindTexture(GL_TEXTURE_3D, 0);
}

void GLVolumeTexture::sync() {
    if (!tex_) createGL();
    if (pendLevels_) {
        unsigned p = pendLevels_;
        pendLevels_ = 0;
        for (UINT L = 0; L < levels_ && p; ++L, p >>= 1)
            if (p & 1) uploadLevel(L);
    }
}
GLVolumeTexture::~GLVolumeTexture() {
    if (ring_[0]) glDeleteTextures(3, ring_);
    else if (tex_) glDeleteTextures(1, &tex_);
}

HRESULT WINAPI GLVolumeTexture::GetDevice(IDirect3DDevice9 **ppDevice) {
    KB_SPEC("GetDevice VolumeTexture %s", KB_SpecThread());
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_; if (device_) device_->AddRef(); return D3D_OK;
}
HRESULT WINAPI GLVolumeTexture::GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) {
    KB_SPEC("VolumeTexture GetLevelDesc %s", KB_SpecThread());
    if (!pDesc || Level >= levels_) return E_INVALIDARG;
    *pDesc = D3DVOLUME_DESC{};
    pDesc->Format = format_; pDesc->Type = D3DRTYPE_VOLUME; pDesc->Usage = usage_; pDesc->Pool = pool_;
    pDesc->Width  = width_  >> Level ? width_  >> Level : 1;
    pDesc->Height = height_ >> Level ? height_ >> Level : 1;
    pDesc->Depth  = depth_  >> Level ? depth_  >> Level : 1;
    return D3D_OK;
}
HRESULT WINAPI GLVolumeTexture::LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) {
    if (!pLockedVolume || Level >= levels_) return E_INVALIDARG;
    KB_SPEC("LockBox %s fmt=%u usage=%x pool=%u level=%s flags=%x box=%d used=%d levels=%u", KB_SpecThread(), (unsigned)format_,
            (unsigned)usage_, (unsigned)pool_, Level ? "n" : "0", (unsigned)Flags, pBox != nullptr, (int)specUsed, levels_);
    UINT w = width_  >> Level ? width_  >> Level : 1;
    UINT h = height_ >> Level ? height_ >> Level : 1;
    UINT d = depth_  >> Level ? depth_  >> Level : 1;
    int bpp = D3DFormatBpp(format_);
    if (levelShadow_[Level].size() != (size_t)w * h * d * bpp) levelShadow_[Level].assign((size_t)w * h * d * bpp, 0);  // preserve (model-lighting volume: per-frame partial patches!)
    dirty_ = true;
    pLockedVolume->RowPitch   = (int)(w * bpp);
    pLockedVolume->SlicePitch = (int)(w * h * bpp);
    pLockedVolume->pBits      = levelShadow_[Level].data();
    return D3D_OK;
}
HRESULT WINAPI GLVolumeTexture::UnlockBox(UINT Level) {
    KB_SPEC("UnlockBox %s", KB_SpecThread());
    if (Level >= levels_ || !dirty_) return D3D_OK;
    dirty_ = false;
    if (!tex_) { pendLevels_ |= 1u << Level; return D3D_OK; }
    uploadLevel(Level);
    return D3D_OK;
}

void GLVolumeTexture::uploadLevel(UINT Level) {
    UINT w = width_  >> Level ? width_  >> Level : 1;
    UINT h = height_ >> Level ? height_ >> Level : 1;
    UINT d = depth_  >> Level ? depth_  >> Level : 1;
    unsigned internal, format, type; int bpp;
    if (D3DToGLFormat(format_, &internal, &format, &type, &bpp)) {
        if (levels_ == 1 && ++uploads_ > 1) {
            if (!ring_[0]) {
                ring_[0] = tex_;
                ringStorage_[0] = true;
                glGenTextures(2, &ring_[1]);
                for (int i = 1; i < 3; ++i) {
                    glBindTexture(GL_TEXTURE_3D, ring_[i]);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
                }
            }
            const unsigned old = tex_;
            ringIndex_ = (ringIndex_ + 1) % 3;
            tex_ = ring_[ringIndex_];
            glBindTexture(GL_TEXTURE_3D, tex_);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            if (ringStorage_[ringIndex_])
                glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, w, h, d, format, type, levelShadow_[0].data());
            else
                glTexImage3D(GL_TEXTURE_3D, 0, internal, w, h, d, 0, format, type, levelShadow_[0].data());
            ringStorage_[ringIndex_] = true;
            glBindTexture(GL_TEXTURE_3D, 0);
            KB_GLRetargetTexture(device_, old, tex_);
            return;
        }
        glBindTexture(GL_TEXTURE_3D, tex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage3D(GL_TEXTURE_3D, Level, internal, w, h, d, 0, format, type, levelShadow_[Level].data());
        glBindTexture(GL_TEXTURE_3D, 0);
    } else {
        fprintf(stderr, "[gl] GLVolumeTexture: unsupported format 0x%x\n", format_);
    }
}

// ---- GLCubeTexture (GL_TEXTURE_CUBE_MAP) ----------------------------------
GLCubeTexture::GLCubeTexture(IDirect3DDevice9 *device, UINT edgeLen, UINT levels,
                             DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : device_(device), edge_(edgeLen),
      levels_(levels ? levels : FullMipCount(edgeLen, edgeLen)), usage_(usage), format_(format), pool_(pool) {
    for (auto &face : levelShadow_) face.resize(levels_);
    createGL();
}

void GLCubeTexture::createGL() {
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex_);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                    levels_ > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, levels_ - 1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}


void GLCubeTexture::maybeGenMips() {
    // Single-mip cubes (reflection probes shipped without a chain) make the shader's
    // gloss->LOD blur (textureLod) a no-op: everything reflects MIRROR SHARP (the
    // "vaseline" gloss). Once all 6 level-0 faces have data, build the chain ourselves.
    if (mipsGenned_ || levels_ != 1 || level0Faces_ != 0x3F || !tex_) return;
    mipsGenned_ = true;
    // Native: glGenerateMipmap works; compressed cubes keep level 0 only.
    { int blockBytes = 0;
      if (D3DCompressedGLFormat(format_, &blockBytes) != 0) return; }
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex_);
    int maxLvl = 0; for (UINT e = edge_; e > 1; e >>= 1) ++maxLvl;
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, maxLvl);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

void GLCubeTexture::sync() {
    if (!tex_) createGL();
    if (pendAny_) {
        pendAny_ = false;
        for (unsigned f = 0; f < 6; ++f) {
            unsigned p = pendLevels_[f];
            pendLevels_[f] = 0;
            for (UINT L = 0; L < levels_ && p; ++L, p >>= 1)
                if (p & 1) { uploadFaceLevel(f, L); if (L == 0) level0Faces_ |= 1u << f; }
        }
    }
    maybeGenMips();
}
GLCubeTexture::~GLCubeTexture() { if (tex_) glDeleteTextures(1, &tex_); }

HRESULT WINAPI GLCubeTexture::GetDevice(IDirect3DDevice9 **ppDevice) {
    KB_SPEC("GetDevice CubeTexture %s", KB_SpecThread());
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_; if (device_) device_->AddRef(); return D3D_OK;
}
HRESULT WINAPI GLCubeTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
    KB_SPEC("CubeTexture GetLevelDesc %s", KB_SpecThread());
    if (!pDesc || Level >= levels_) return E_INVALIDARG;
    *pDesc = D3DSURFACE_DESC{};
    pDesc->Format = format_; pDesc->Type = D3DRTYPE_SURFACE; pDesc->Usage = usage_; pDesc->Pool = pool_;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    UINT e = edge_ >> Level ? edge_ >> Level : 1;
    pDesc->Width = e; pDesc->Height = e;
    return D3D_OK;
}
HRESULT WINAPI GLCubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level,
                                                IDirect3DSurface9 **ppSurface) {
    if (!ppSurface || (unsigned)face >= 6 || level >= levels_) return E_INVALIDARG;
    KB_SPEC("GetCubeMapSurface %s level=%s usage=%x fmt=%u", KB_SpecThread(), level ? "n" : "0", (unsigned)usage_, (unsigned)format_);
    *ppSurface = new GLSurface(this, face, level);
    return D3D_OK;
}
HRESULT WINAPI GLCubeTexture::LockRect(D3DCUBEMAP_FACES FaceType, UINT Level,
                                       D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
    if (!pLockedRect || (unsigned)FaceType >= 6 || Level >= levels_) return E_INVALIDARG;
    KB_SPEC("LockRectCube %s fmt=%u usage=%x pool=%u level=%s flags=%x rect=%d used=%d", KB_SpecThread(), (unsigned)format_,
            (unsigned)usage_, (unsigned)pool_, Level ? "n" : "0", (unsigned)Flags, pRect != nullptr, (int)specUsed);
    UINT e = edge_ >> Level ? edge_ >> Level : 1;
    std::vector<unsigned char> &shadow = levelShadow_[FaceType][Level];
    int blockBytes = 0;
    if (D3DCompressedGLFormat(format_, &blockBytes)) {
        UINT bw = (e + 3) / 4, bh = (e + 3) / 4;
        if (shadow.size() != (size_t)bw * bh * blockBytes) shadow.assign((size_t)bw * bh * blockBytes, 0);  // preserve
        pLockedRect->Pitch = (int)(bw * blockBytes);
    } else {
        int bpp = D3DFormatBpp(format_);
        if (shadow.size() != (size_t)e * e * bpp) shadow.assign((size_t)e * e * bpp, 0);  // preserve
        pLockedRect->Pitch = (int)(e * bpp);
    }
    dirty_ = true;
    pLockedRect->pBits = shadow.data();
    return D3D_OK;
}
HRESULT WINAPI GLCubeTexture::UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) {
    KB_SPEC("UnlockRectCube %s", KB_SpecThread());
    if ((unsigned)FaceType >= 6 || Level >= levels_ || !dirty_) return D3D_OK;
    dirty_ = false;
    if (!tex_) {
        pendLevels_[(unsigned)FaceType] |= 1u << Level;
        pendAny_ = true;
        return D3D_OK;
    }
    uploadFaceLevel((unsigned)FaceType, Level);
    if (Level == 0) { level0Faces_ |= 1u << (unsigned)FaceType; maybeGenMips(); }
    return D3D_OK;
}

void GLCubeTexture::uploadFaceLevel(unsigned Face, UINT Level) {
    UINT e = edge_ >> Level ? edge_ >> Level : 1;
    std::vector<unsigned char> &shadow = levelShadow_[Face][Level];
    GLenum target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + Face;
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex_);
    int blockBytes = 0; unsigned cfmt = D3DCompressedGLFormat(format_, &blockBytes);
    if (cfmt) {
        glCompressedTexImage2D(target, Level, cfmt, e, e, 0,
                               (GLsizei)shadow.size(), shadow.data());
    } else {
        unsigned internal, format, type; int bpp;
        if (!D3DToGLFormat(format_, &internal, &format, &type, &bpp)) {
            fprintf(stderr, "[gl] GLCubeTexture: unsupported format 0x%x (face %d level %d not uploaded)\n",
                    format_, (int)Face, (int)Level);
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0); return;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(target, Level, internal, e, e, 0, format, type, shadow.data());
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

// ---- GLSurface ------------------------------------------------------------
GLSurface::GLSurface(GLTexture *owner, UINT level)
    : owner_(owner), level_(level),
      width_(owner->width()  >> level ? owner->width()  >> level : 1),
      height_(owner->height() >> level ? owner->height() >> level : 1),
      format_(owner->format()) {}

GLSurface::GLSurface(GLCubeTexture *owner, D3DCUBEMAP_FACES face, UINT level)
    : cubeOwner_(owner), cubeFace_(face), level_(level),
      width_(owner->edgeLength() >> level ? owner->edgeLength() >> level : 1),
      height_(owner->edgeLength() >> level ? owner->edgeLength() >> level : 1),
      format_(owner->format()) {}

GLSurface::GLSurface(IDirect3DDevice9 *device, UINT width, UINT height, D3DFORMAT format, bool sysmem)
    : device_(device), width_(width), height_(height), format_(format), sysmem_(sysmem) {
    if (sysmem_) {
        shadow_.assign((size_t)width_ * height_ * D3DFormatBpp(format_), 0);
    } else {
        // Standalone render target: an immutable-storage GL texture.
        unsigned internal, fmt, type; int bpp;
        glGenTextures(1, &ownTex_);
        glBindTexture(GL_TEXTURE_2D, ownTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        (void)internal; (void)fmt; (void)type; (void)bpp;
        // Renderable, completeness-verified storage (see KB_AllocRenderableRT).
        KB_AllocRenderableRT(ownTex_, width_, height_, format_);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

GLSurface::GLSurface(IDirect3DDevice9 *device, UINT width, UINT height, D3DFORMAT format, GLBackbufferTag)
    : device_(device), width_(width), height_(height), format_(format), backbuffer_(true) {}

GLSurface::GLSurface(IDirect3DDevice9 *device, UINT width, UINT height, D3DFORMAT format, GLDepthStencilTag)
    : device_(device), width_(width), height_(height), format_(format), depthStencil_(true) {
    // Shadow path: back the depth-stencil surface with a REAL sampleable depth texture
    // (compare mode for sampler2DShadow). The engine's shadowmapFormatSecondary arrives
    // as garbage from the decompiled metrics struct (e.g. 374936), so the format is NOT
    // trusted — always allocate DEPTH24_STENCIL8. Without this the shadow FBO attached
    // a color texture as depth -> incomplete -> whole passes no-op'd (black regions).
    if (KB_ShadowsEnabled()) {
        glGenTextures(1, &ownTex_);
        glBindTexture(GL_TEXTURE_2D, ownTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE /* manual-PCF engine: raw depth reads */);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width_, height_, 0,
                     GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        ownTexIsDepth_ = true;
    }
}

GLSurface::~GLSurface() { if (ownTex_) glDeleteTextures(1, &ownTex_); }

unsigned GLSurface::texName() const {
    if (owner_) return owner_->glName();
    if (cubeOwner_) return cubeOwner_->glName();
    return ownTex_;
}

HRESULT WINAPI GLSurface::GetDevice(IDirect3DDevice9 **ppDevice) {
    KB_SPEC("GetDevice Surface %s", KB_SpecThread());
    if (owner_) return owner_->GetDevice(ppDevice);
    if (cubeOwner_) return cubeOwner_->GetDevice(ppDevice);
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_;
    if (device_) device_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI GLSurface::GetDesc(D3DSURFACE_DESC *pDesc) {
    KB_SPEC("SurfaceGetDesc %s", KB_SpecThread());
    if (owner_) return owner_->GetLevelDesc(level_, pDesc);
    if (cubeOwner_) return cubeOwner_->GetLevelDesc(level_, pDesc);
    if (!pDesc) return E_INVALIDARG;
    *pDesc = D3DSURFACE_DESC{};
    pDesc->Format = format_; pDesc->Type = D3DRTYPE_SURFACE;
    pDesc->Usage = depthStencil_ ? D3DUSAGE_DEPTHSTENCIL : (sysmem_ ? 0 : D3DUSAGE_RENDERTARGET);
    pDesc->Pool = sysmem_ ? D3DPOOL_SYSTEMMEM : D3DPOOL_DEFAULT;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    pDesc->Width = width_; pDesc->Height = height_;
    return D3D_OK;
}

HRESULT WINAPI GLSurface::LockRect(D3DLOCKED_RECT *lr, const RECT *r, DWORD f) {
    if (!owner_ && !cubeOwner_)
        KB_SPEC("SurfaceLockRect %s sysmem=%d fmt=%u rect=%d flags=%x", KB_SpecThread(), (int)sysmem_, (unsigned)format_,
                r != nullptr, (unsigned)f);
    if (owner_) return owner_->LockRect(level_, lr, r, f);
    if (cubeOwner_) return cubeOwner_->LockRect(cubeFace_, level_, lr, r, f);
    if (!lr) return E_INVALIDARG;
    if (!sysmem_) return E_FAIL;  // only system-memory surfaces are CPU-lockable here
    lr->Pitch = (int)(width_ * D3DFormatBpp(format_));
    lr->pBits = shadow_.data();
    return D3D_OK;
}

HRESULT WINAPI GLSurface::UnlockRect() {
    if (owner_) return owner_->UnlockRect(level_);
    if (cubeOwner_) return cubeOwner_->UnlockRect(cubeFace_, level_);
    return D3D_OK;  // sysmem: nothing to flush
}

HRESULT WINAPI GLSurface::GetContainer(REFIID, void **ppContainer) {
    KB_SPEC("SurfaceGetContainer %s", KB_SpecThread());
    if (!ppContainer) return E_INVALIDARG;
    if (owner_) { owner_->AddRef(); *ppContainer = owner_; }
    else if (cubeOwner_) { cubeOwner_->AddRef(); *ppContainer = cubeOwner_; }
    else                 { AddRef();             *ppContainer = this; }
    return D3D_OK;
}

// ---- GLVertexDeclaration --------------------------------------------------
GLVertexDeclaration::GLVertexDeclaration(IDirect3DDevice9 *device,
                                         const D3DVERTEXELEMENT9 *elements)
    : device_(device) {
    // Copy up to (and excluding) the D3DDECL_END terminator (Stream == 0xFF).
    for (const D3DVERTEXELEMENT9 *e = elements; e && e->Stream != 0xFF; ++e) {
        elements_.push_back(*e);
        char el[48];
        snprintf(el, sizeof(el), "%s%u:%u:%u:%u:%u", specSig.empty() ? "" : ",", (unsigned)e->Stream, (unsigned)e->Offset,
                 (unsigned)e->Type, (unsigned)e->Usage, (unsigned)e->UsageIndex);
        specSig += el;
    }
}

GLVertexDeclaration::~GLVertexDeclaration() { KB_VaoNoteDeadDecl(this); }

HRESULT WINAPI GLVertexDeclaration::GetDevice(IDirect3DDevice9 **ppDevice) {
    KB_SPEC("GetDevice VertexDeclaration %s", KB_SpecThread());
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_;
    if (device_) device_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI GLVertexDeclaration::GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) {
    KB_SPEC("GetDeclaration %s", KB_SpecThread());
    UINT n = (UINT)elements_.size() + 1;  // +1 for the terminator D3D reports
    if (pNumElements) *pNumElements = n;
    if (pElement) {
        for (size_t i = 0; i < elements_.size(); ++i) pElement[i] = elements_[i];
        pElement[elements_.size()] = D3DVERTEXELEMENT9 D3DDECL_END();
    }
    return D3D_OK;
}
