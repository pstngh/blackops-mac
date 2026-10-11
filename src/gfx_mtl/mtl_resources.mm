// mtl_resources.mm — buffers, textures, surfaces, declarations, shaders and queries
// of the Metal backend (mtl_d3d9.h).
#include "mtl_device.h"
#include "mtl_msl.h"
#include "../gfx_shader/sm3.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <set>

// Counters shared with the GL layer's KB_FPS line (gl_query.cpp).
extern unsigned long g_kbUploads;
extern unsigned long long g_kbUploadBytes;

void MTL_Bug(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    static std::mutex mu;
    static std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> g(mu);
        if (!seen.insert(msg).second) return;
    }
    fprintf(stderr, "[mtl] BUG: %s\n", msg);
    static const bool strict = getenv("KB_MTL_STRICT") && *getenv("KB_MTL_STRICT") == '1';
    if (strict) abort();
}

// ---- Formats ------------------------------------------------------------------
MetalFormat MTL_Format(D3DFORMAT f, bool depthUsage) {
    MetalFormat m;
    auto set = [&](MTLPixelFormat pf, int bytes) { m.pixelFormat = (unsigned)pf; m.bytes = bytes; };
    switch ((unsigned)f) {
    case D3DFMT_A8R8G8B8:      set(MTLPixelFormatBGRA8Unorm, 4); break;
    case D3DFMT_X8R8G8B8:      set(MTLPixelFormatBGRA8Unorm, 4); m.swizzle[3] = MTLTextureSwizzleOne; break;
    case D3DFMT_A8B8G8R8:      set(MTLPixelFormatRGBA8Unorm, 4); break;
    case D3DFMT_R5G6B5:        set(MTLPixelFormatB5G6R5Unorm, 2); break;
    case D3DFMT_A8:            set(MTLPixelFormatA8Unorm, 1); break;
    case D3DFMT_L8:            set(MTLPixelFormatR8Unorm, 1);
                               m.swizzle[1] = m.swizzle[2] = MTLTextureSwizzleRed; m.swizzle[3] = MTLTextureSwizzleOne; break;
    case D3DFMT_A8L8:          set(MTLPixelFormatRG8Unorm, 2);
                               m.swizzle[1] = m.swizzle[2] = MTLTextureSwizzleRed; m.swizzle[3] = MTLTextureSwizzleGreen; break;
    case D3DFMT_A16B16G16R16F: set(MTLPixelFormatRGBA16Float, 8); break;
    case D3DFMT_A16B16G16R16:  set(MTLPixelFormatRGBA16Unorm, 8); break;
    case D3DFMT_G16R16F:       set(MTLPixelFormatRG16Float, 4); break;
    case D3DFMT_G16R16:        set(MTLPixelFormatRG16Unorm, 4); break;
    case D3DFMT_R32F:          set(MTLPixelFormatR32Float, 4); break;
    case D3DFMT_DXT1:          set(MTLPixelFormatBC1_RGBA, 8); m.compressed = true; break;
    case D3DFMT_DXT3:          set(MTLPixelFormatBC2_RGBA, 16); m.compressed = true; break;
    case D3DFMT_DXT5:          set(MTLPixelFormatBC3_RGBA, 16); m.compressed = true; break;
    case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24FS8:
        // Apple GPUs have no 24-bit depth. The bias scale follows the GL layer's
        // (2^24 per unit, gl_state.cpp), which Apple's GL gives D24 the same way.
        set(MTLPixelFormatDepth32Float_Stencil8, 8); m.depth = m.stencil = true; break;
    case D3DFMT_D16: case D3DFMT_D16_LOCKABLE:
        set(MTLPixelFormatDepth16Unorm, 2); m.depth = true; break;
    default:
        // The GL layer gives a depth-stencil texture of an unknown format (the engine's
        // shadow map format arrives wrong from the decompiled metrics) D24S8.
        if (depthUsage) { set(MTLPixelFormatDepth32Float_Stencil8, 8); m.depth = m.stencil = true; }
        break;
    }
    return m;
}

// ---- Buffers ------------------------------------------------------------------
std::atomic<unsigned> MetalBuffer::generations{1};

MetalBuffer::MetalBuffer(MetalDevice *dev, UINT length, DWORD usage) : dev_(dev), length_(length), usage_(usage) {
    gen_ = generations.fetch_add(1);
    id<MTLBuffer> b = [dev->mtl newBufferWithLength:length ? length : 4 options:MTLResourceStorageModeShared];
    backings_.push_back({ b, 0 });
}

MetalBuffer::~MetalBuffer() {}

void *MetalBuffer::lock(UINT offset, UINT size, DWORD flags) {
    if (size == 0) size = length_ - offset;   // D3D9: 0 locks to the end
    if (offset + size > length_) { MTL_Bug("buffer lock past the end"); return nullptr; }
    std::lock_guard<std::mutex> g(mu_);
    Backing &cur = backings_[0];
    const unsigned long long done = dev_->completedSerial();
    if (flags & D3DLOCK_DISCARD) {
        if (cur.lastUse > done) {
            // The GPU may still read it: take a backing it has finished with, or a new one.
            size_t i = 1;
            while (i < backings_.size() && backings_[i].lastUse > done) ++i;
            if (i == backings_.size())
                backings_.push_back({ [dev_->mtl newBufferWithLength:length_ options:MTLResourceStorageModeShared], 0 });
            std::swap(backings_[0], backings_[i]);
            gen_.store(generations.fetch_add(1), std::memory_order_release);
        }
    } else if (!(flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY)) && cur.lastUse > done) {
        // A plain lock waits for the GPU to finish with the buffer, as D3D9's does.
        MTL_Bug("synchronous lock of a buffer the GPU is using (usage %x)", (unsigned)usage_);
        dev_->finishGPU();
    }
    ++g_kbUploads;
    g_kbUploadBytes += size;
    return (unsigned char *)[backings_[0].buf contents] + offset;
}

id<MTLBuffer> MetalBuffer::current() {
    std::lock_guard<std::mutex> g(mu_);
    backings_[0].lastUse = dev_->recordingSerial();
    return backings_[0].buf;
}

HRESULT WINAPI MetalVertexBuffer::GetDevice(IDirect3DDevice9 **pp) { *pp = dev_; dev_->AddRef(); return D3D_OK; }
HRESULT WINAPI MetalIndexBuffer::GetDevice(IDirect3DDevice9 **pp) { *pp = dev_; dev_->AddRef(); return D3D_OK; }

HRESULT WINAPI MetalVertexBuffer::Lock(UINT off, UINT size, void **pp, DWORD flags) {
    *pp = buf_.lock(off, size, flags);
    return *pp ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT WINAPI MetalIndexBuffer::Lock(UINT off, UINT size, void **pp, DWORD flags) {
    *pp = buf_.lock(off, size, flags);
    return *pp ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT WINAPI MetalVertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *d) {
    *d = D3DVERTEXBUFFER_DESC{};
    d->Format = D3DFMT_UNKNOWN; d->Type = D3DRTYPE_VERTEXBUFFER; d->Usage = buf_.usage(); d->Pool = pool_;
    d->Size = buf_.length(); d->FVF = fvf_;
    return D3D_OK;
}

HRESULT WINAPI MetalIndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *d) {
    *d = D3DINDEXBUFFER_DESC{};
    d->Format = format_; d->Type = D3DRTYPE_INDEXBUFFER; d->Usage = buf_.usage(); d->Pool = pool_; d->Size = buf_.length();
    return D3D_OK;
}

// ---- Textures -----------------------------------------------------------------
static UINT FullMipCount(UINT w, UINT h, UINT d) {
    UINT m = w > h ? w : h, n = 1;
    if (d > m) m = d;
    while (m > 1) { m >>= 1; ++n; }
    return n;
}

MetalTexture::MetalTexture(MetalDevice *dev, IUnknown *owner, Kind kind, UINT w, UINT h, UINT d, UINT levels,
                           DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
    : dev_(dev), owner_(owner), kind_(kind), width_(w), height_(h), depth_(d),
      levels_(levels ? levels : FullMipCount(w, h, kind == TEX_VOLUME ? d : 1)), usage_(usage), format_(fmt),
      pool_(pool), mfmt_(MTL_Format(fmt, (usage & D3DUSAGE_DEPTHSTENCIL) != 0)) {
    if (!mfmt_.pixelFormat) {
        MTL_Bug("texture format %u (usage %x)", (unsigned)fmt, (unsigned)usage);
        mfmt_ = MTL_Format(D3DFMT_A8R8G8B8, false);
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = kind == TEX_CUBE ? MTLTextureTypeCube : kind == TEX_VOLUME ? MTLTextureType3D : MTLTextureType2D;
    td.pixelFormat = (MTLPixelFormat)mfmt_.pixelFormat;
    td.width = w; td.height = h; td.depth = kind == TEX_VOLUME ? d : 1;
    td.mipmapLevelCount = levels_;
    const bool target = (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0 || mfmt_.depth;
    td.usage = MTLTextureUsageShaderRead | (target ? MTLTextureUsageRenderTarget : 0);
    td.storageMode = target ? MTLStorageModePrivate : MTLStorageModeShared;
    const bool swizzled = memcmp(mfmt_.swizzle, "\x02\x03\x04\x05", 4) != 0;
    if (swizzled) td.usage |= MTLTextureUsagePixelFormatView;
    tex_ = [dev->mtl newTextureWithDescriptor:td];
    if (swizzled) {
        MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(
            (MTLTextureSwizzle)mfmt_.swizzle[0], (MTLTextureSwizzle)mfmt_.swizzle[1],
            (MTLTextureSwizzle)mfmt_.swizzle[2], (MTLTextureSwizzle)mfmt_.swizzle[3]);
        view_ = [tex_ newTextureViewWithPixelFormat:td.pixelFormat textureType:td.textureType
                                            levels:NSMakeRange(0, levels_)
                                            slices:NSMakeRange(0, kind == TEX_CUBE ? 6 : 1) swizzle:sw];
    }
}

MetalTexture::MetalTexture(MetalDevice *dev, id<MTLTexture> tex, D3DFORMAT fmt, DWORD usage)
    : dev_(dev), owner_(nullptr), kind_(TEX_2D), width_((UINT)tex.width), height_((UINT)tex.height), depth_(1), levels_(1),
      usage_(usage), format_(fmt), pool_(D3DPOOL_DEFAULT), mfmt_(MTL_Format(fmt, (usage & D3DUSAGE_DEPTHSTENCIL) != 0)),
      tex_(tex) {
    used_ = true;
}

MetalTexture::~MetalTexture() {
    for (MetalSurface *s : surfaces_) delete s;
}

UINT MetalTexture::width(UINT l) const  { UINT v = width_ >> l;  return v ? v : 1; }
UINT MetalTexture::height(UINT l) const { UINT v = height_ >> l; return v ? v : 1; }
UINT MetalTexture::depthOf(UINT l) const { UINT v = depth_ >> l; return kind_ == TEX_VOLUME && v ? v : 1; }

size_t MetalTexture::levelBytes(UINT level, UINT *pitch, UINT *rows) const {
    const UINT w = width(level), h = height(level);
    if (mfmt_.compressed) {
        *pitch = ((w + 3) / 4) * mfmt_.bytes;
        *rows = (h + 3) / 4;
    } else {
        *pitch = w * mfmt_.bytes;
        *rows = h;
    }
    return (size_t)*pitch * *rows * depthOf(level);
}

HRESULT MetalTexture::lockRect(UINT face, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) {
    if (level >= levels_ || face >= (kind_ == TEX_CUBE ? 6u : 1u)) return D3DERR_INVALIDCALL;
    if (rect) { MTL_Bug("LockRect with a rectangle"); return D3DERR_INVALIDCALL; }
    if (mfmt_.depth || (usage_ & D3DUSAGE_RENDERTARGET)) { MTL_Bug("LockRect of a render target texture"); return D3DERR_INVALIDCALL; }
    UINT pitch, rows;
    const size_t bytes = levelBytes(level, &pitch, &rows);
    std::lock_guard<std::mutex> g(mu_);
    for (Pending &p : locks_)
        if (p.face == face && p.level == level) { MTL_Bug("texture level locked twice"); return D3DERR_INVALIDCALL; }
    Pending p;
    p.face = face; p.level = level; p.locked = true;
    p.data.resize(bytes);
    if (!(flags & D3DLOCK_DISCARD)) fillLock(p, pitch, rows);
    locks_.push_back(std::move(p));
    lr->pBits = locks_.back().data.data();
    lr->Pitch = (INT)pitch;
    return D3D_OK;
}

HRESULT MetalTexture::lockBox(UINT level, D3DLOCKED_BOX *lb, const D3DBOX *box, DWORD flags) {
    if (kind_ != TEX_VOLUME || level >= levels_) return D3DERR_INVALIDCALL;
    if (box) { MTL_Bug("LockBox with a box"); return D3DERR_INVALIDCALL; }
    UINT pitch, rows;
    const size_t bytes = levelBytes(level, &pitch, &rows);
    std::lock_guard<std::mutex> g(mu_);
    for (Pending &p : locks_)
        if (p.level == level) { MTL_Bug("volume level locked twice"); return D3DERR_INVALIDCALL; }
    Pending p;
    p.level = level; p.locked = true;
    p.data.resize(bytes);
    if (!(flags & D3DLOCK_DISCARD)) fillLock(p, pitch, rows);
    locks_.push_back(std::move(p));
    lb->pBits = locks_.back().data.data();
    lb->RowPitch = (INT)pitch;
    lb->SlicePitch = (INT)(pitch * rows);
    return D3D_OK;
}

HRESULT MetalTexture::unlockRect(UINT face, UINT level) {
    Pending p;
    {
        std::lock_guard<std::mutex> g(mu_);
        size_t i = 0;
        while (i < locks_.size() && !(locks_[i].face == face && locks_[i].level == level)) ++i;
        if (i == locks_.size()) { MTL_Bug("UnlockRect without a lock"); return D3DERR_INVALIDCALL; }
        p = std::move(locks_[i]);
        locks_.erase(locks_.begin() + i);
    }
    upload(p);
    return D3D_OK;
}

HRESULT MetalTexture::unlockBox(UINT level) { return unlockRect(0, level); }

// The level's current contents into a lock's buffer; mu_ held.
void MetalTexture::fillLock(Pending &p, UINT pitch, UINT rows) {
    const size_t i = (size_t)p.face * levels_ + p.level;
    if (i < kept_.size() && kept_[i].size() == p.data.size()) {
        memcpy(p.data.data(), kept_[i].data(), p.data.size());
        return;
    }
    if (i >= written_.size() || !written_[i]) return;   // never written: undefined contents
    [tex_ getBytes:p.data.data() bytesPerRow:pitch bytesPerImage:pitch * rows
        fromRegion:MTLRegionMake3D(0, 0, 0, width(p.level), height(p.level), depthOf(p.level)) mipmapLevel:p.level
             slice:p.face];
}

// A texture the GPU has never used is written in place (any thread); after that the
// copy goes through the frame's command buffer, ordered with the draws around it, and
// the bytes are kept for the next lock.
void MetalTexture::upload(Pending &p) {
    UINT pitch, rows;
    levelBytes(p.level, &pitch, &rows);
    const MTLRegion r = MTLRegionMake3D(0, 0, 0, width(p.level), height(p.level), depthOf(p.level));
    const size_t i = (size_t)p.face * levels_ + p.level;
    ++g_kbUploads;
    g_kbUploadBytes += p.data.size();
    {
        std::lock_guard<std::mutex> g(mu_);
        if (written_.size() <= i) written_.resize((size_t)levels_ * (kind_ == TEX_CUBE ? 6 : 1));
        written_[i] = true;
    }
    if (!used_.load()) {
        [tex_ replaceRegion:r mipmapLevel:p.level slice:p.face withBytes:p.data.data() bytesPerRow:pitch
              bytesPerImage:pitch * rows];
        std::lock_guard<std::mutex> g(mu_);
        if (i < kept_.size()) kept_[i].clear();
        return;
    }
    if (!dev_->onRenderThread()) {
        MTL_Bug("texture rewritten off the render thread after the GPU used it");
        [tex_ replaceRegion:r mipmapLevel:p.level slice:p.face withBytes:p.data.data() bytesPerRow:pitch
              bytesPerImage:pitch * rows];
        return;
    }
    dev_->copyToTexture(tex_, p.face, p.level, r.origin, r.size, p.data.data(), pitch, pitch * rows);
    std::lock_guard<std::mutex> g(mu_);
    if (kept_.size() <= i) kept_.resize((size_t)levels_ * (kind_ == TEX_CUBE ? 6 : 1));
    kept_[i] = std::move(p.data);
}

MetalSurface *MetalTexture::surface(UINT face, UINT level) {
    std::lock_guard<std::mutex> g(mu_);
    for (MetalSurface *s : surfaces_)
        if (s->face() == face && s->level() == level) return s;
    MetalSurface *s = new MetalSurface(dev_, MetalSurface::S_LEVEL, width(level), height(level), format_, owner_, this,
                                       face, level);
    surfaces_.push_back(s);
    return s;
}

static HRESULT LevelDesc(MetalTexture &t, UINT level, D3DSURFACE_DESC *d) {
    if (level >= t.levels()) return D3DERR_INVALIDCALL;
    *d = D3DSURFACE_DESC{};
    d->Format = t.format(); d->Type = D3DRTYPE_SURFACE; d->Usage = t.usage(); d->Pool = t.pool();
    d->Width = t.width(level); d->Height = t.height(level);
    return D3D_OK;
}

HRESULT WINAPI MetalTexture2D::GetDevice(IDirect3DDevice9 **pp) { *pp = dev_; dev_->AddRef(); return D3D_OK; }
HRESULT WINAPI MetalCubeTexture::GetDevice(IDirect3DDevice9 **pp) { *pp = dev_; dev_->AddRef(); return D3D_OK; }
HRESULT WINAPI MetalVolumeTexture::GetDevice(IDirect3DDevice9 **pp) { *pp = dev_; dev_->AddRef(); return D3D_OK; }
DWORD WINAPI MetalTexture2D::SetLOD(DWORD lod) { if (lod) MTL_Bug("SetLOD %u", (unsigned)lod); return 0; }
DWORD WINAPI MetalCubeTexture::SetLOD(DWORD lod) { if (lod) MTL_Bug("SetLOD %u", (unsigned)lod); return 0; }
DWORD WINAPI MetalVolumeTexture::SetLOD(DWORD lod) { if (lod) MTL_Bug("SetLOD %u", (unsigned)lod); return 0; }
HRESULT WINAPI MetalTexture2D::AddDirtyRect(const RECT *) { MTL_Bug("AddDirtyRect"); return D3D_OK; }
HRESULT WINAPI MetalTexture2D::GetLevelDesc(UINT level, D3DSURFACE_DESC *d) { return LevelDesc(t_, level, d); }
HRESULT WINAPI MetalCubeTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC *d) { return LevelDesc(t_, level, d); }

HRESULT WINAPI MetalVolumeTexture::GetLevelDesc(UINT level, D3DVOLUME_DESC *d) {
    if (level >= t_.levels()) return D3DERR_INVALIDCALL;
    *d = D3DVOLUME_DESC{};
    d->Format = t_.format(); d->Type = D3DRTYPE_VOLUME; d->Usage = t_.usage(); d->Pool = t_.pool();
    d->Width = t_.width(level); d->Height = t_.height(level); d->Depth = t_.depthOf(level);
    return D3D_OK;
}

HRESULT WINAPI MetalTexture2D::GetSurfaceLevel(UINT level, IDirect3DSurface9 **pp) {
    if (level >= t_.levels()) return D3DERR_INVALIDCALL;
    MetalSurface *s = t_.surface(0, level);
    s->AddRef();
    *pp = s;
    return D3D_OK;
}

HRESULT WINAPI MetalCubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **pp) {
    if (level >= t_.levels() || (unsigned)face >= 6) return D3DERR_INVALIDCALL;
    MetalSurface *s = t_.surface((UINT)face, level);
    s->AddRef();
    *pp = s;
    return D3D_OK;
}

// ---- Surfaces -----------------------------------------------------------------
MetalSurface::MetalSurface(MetalDevice *dev, Kind kind, UINT w, UINT h, D3DFORMAT fmt, IUnknown *container,
                           MetalTexture *tex, UINT face, UINT level)
    : dev_(dev), kind_(kind), width_(w), height_(h), format_(fmt), container_(container), tex_(tex), face_(face),
      level_(level) {
    if (kind == S_SYSMEM) {
        const MetalFormat m = MTL_Format(fmt, false);
        if (m.compressed || !m.bytes) MTL_Bug("system memory surface format %u", (unsigned)fmt);
        sysmem_.assign((size_t)w * h * (m.bytes ? m.bytes : 4), 0);
    }
}

MetalSurface::~MetalSurface() { delete own_; }

HRESULT WINAPI MetalSurface::GetDevice(IDirect3DDevice9 **pp) { *pp = dev_; dev_->AddRef(); return D3D_OK; }

HRESULT WINAPI MetalSurface::GetContainer(REFIID, void **pp) {
    IUnknown *c = container_ ? container_ : static_cast<IUnknown *>(this);
    c->AddRef();
    *pp = c;
    return D3D_OK;
}

HRESULT WINAPI MetalSurface::GetDesc(D3DSURFACE_DESC *d) {
    *d = D3DSURFACE_DESC{};
    d->Format = format_; d->Type = D3DRTYPE_SURFACE; d->Width = width_; d->Height = height_;
    switch (kind_) {
    case S_LEVEL:        d->Usage = tex_->usage(); d->Pool = tex_->pool(); break;
    case S_RENDERTARGET: d->Usage = D3DUSAGE_RENDERTARGET; d->Pool = D3DPOOL_DEFAULT; break;
    case S_DEPTHSTENCIL: d->Usage = D3DUSAGE_DEPTHSTENCIL; d->Pool = D3DPOOL_DEFAULT; break;
    case S_SYSMEM:       d->Pool = D3DPOOL_SYSTEMMEM; break;
    case S_BACKBUFFER:   d->Usage = D3DUSAGE_RENDERTARGET; d->Pool = D3DPOOL_DEFAULT; break;
    }
    return D3D_OK;
}

HRESULT WINAPI MetalSurface::LockRect(D3DLOCKED_RECT *lr, const RECT *r, DWORD f) {
    if (kind_ == S_LEVEL) {
        if (r) { MTL_Bug("surface LockRect with a rectangle"); return D3DERR_INVALIDCALL; }
        return tex_->lockRect(face_, level_, lr, nullptr, f);
    }
    if (kind_ != S_SYSMEM) { MTL_Bug("LockRect of a GPU surface (kind %d)", (int)kind_); return D3DERR_INVALIDCALL; }
    const MetalFormat m = MTL_Format(format_, false);
    const UINT bpp = m.bytes ? (UINT)m.bytes : 4;
    unsigned char *p = sysmem_.data();
    if (r) p += (size_t)r->top * width_ * bpp + (size_t)r->left * bpp;
    lr->pBits = p;
    lr->Pitch = (INT)(width_ * bpp);
    return D3D_OK;
}

HRESULT WINAPI MetalSurface::UnlockRect() {
    if (kind_ == S_LEVEL) return tex_->unlockRect(face_, level_);
    return D3D_OK;
}

// ---- Vertex declarations ------------------------------------------------------
static MTLVertexFormat VertexFormat(BYTE t) {
    switch (t) {
    case D3DDECLTYPE_FLOAT1:    return MTLVertexFormatFloat;
    case D3DDECLTYPE_FLOAT2:    return MTLVertexFormatFloat2;
    case D3DDECLTYPE_FLOAT3:    return MTLVertexFormatFloat3;
    case D3DDECLTYPE_FLOAT4:    return MTLVertexFormatFloat4;
    case D3DDECLTYPE_D3DCOLOR:  return MTLVertexFormatUChar4Normalized_BGRA;
    case D3DDECLTYPE_UBYTE4:    return MTLVertexFormatUChar4;     // read as 0..255 floats, as D3D9
    case D3DDECLTYPE_SHORT2:    return MTLVertexFormatShort2;
    case D3DDECLTYPE_SHORT4:    return MTLVertexFormatShort4;
    case D3DDECLTYPE_UBYTE4N:   return MTLVertexFormatUChar4Normalized;
    case D3DDECLTYPE_SHORT2N:   return MTLVertexFormatShort2Normalized;
    case D3DDECLTYPE_SHORT4N:   return MTLVertexFormatShort4Normalized;
    case D3DDECLTYPE_USHORT2N:  return MTLVertexFormatUShort2Normalized;
    case D3DDECLTYPE_USHORT4N:  return MTLVertexFormatUShort4Normalized;
    case D3DDECLTYPE_DEC3N:     return MTLVertexFormatInt1010102Normalized;
    case D3DDECLTYPE_FLOAT16_2: return MTLVertexFormatHalf2;
    case D3DDECLTYPE_FLOAT16_4: return MTLVertexFormatHalf4;
    default:                    return MTLVertexFormatInvalid;
    }
}

MetalDecl::MetalDecl(MetalDevice *dev, const D3DVERTEXELEMENT9 *e) : uid(dev->nextId()), dev_(dev) {
    for (; e->Stream != 0xFF; ++e) {
        elements.push_back(*e);
        const int a = MSL_AttribIndex(e->Usage, e->UsageIndex);
        const MTLVertexFormat f = VertexFormat(e->Type);
        if (a < 0 || f == MTLVertexFormatInvalid || e->Method != 0 || e->Stream >= 4) {
            MTL_Bug("vertex element type %u method %u usage %u index %u stream %u", (unsigned)e->Type,
                    (unsigned)e->Method, (unsigned)e->Usage, (unsigned)e->UsageIndex, (unsigned)e->Stream);
            continue;
        }
        attrs.push_back({ a, (unsigned)f, e->Offset, e->Stream });
        attribMask |= 1u << a;
        streamMask |= 1u << e->Stream;
    }
}

HRESULT WINAPI MetalDecl::GetDevice(IDirect3DDevice9 **pp) { *pp = dev_; dev_->AddRef(); return D3D_OK; }

HRESULT WINAPI MetalDecl::GetDeclaration(D3DVERTEXELEMENT9 *e, UINT *n) {
    if (n) *n = (UINT)elements.size() + 1;
    if (e) {
        for (size_t i = 0; i < elements.size(); ++i) e[i] = elements[i];
        e[elements.size()] = D3DVERTEXELEMENT9 D3DDECL_END();
    }
    return D3D_OK;
}

// ---- Shaders ------------------------------------------------------------------
static double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// KB_MTL_SHADERLOG=<file>: a line per event, seconds since the first: "c <entry> <batch>" a
// shader created (the batch it compiles in), "s <batch> <shaders>" a batch given to Metal,
// "d <batch>" compiled, "w <batch> <ms>" a draw waited for it, "u <entry>" a shader's
// first use, "p <ms>" a frame over 15 ms (Present).
void MTL_ShaderLog(const char *fmt, ...) {
    static FILE *f = [] { const char *e = getenv("KB_MTL_SHADERLOG"); return e ? fopen(e, "w") : (FILE *)nullptr; }();
    if (!f) return;
    static const double t0 = Seconds();
    static std::mutex mu;
    std::lock_guard<std::mutex> g(mu);
    fprintf(f, "%.4f ", Seconds() - t0);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

MetalShaderCompiler::MetalShaderCompiler(id<MTLDevice> dev) : dev_(dev) {
    const char *e = getenv("KB_MTL_BATCH");
    size_ = e && atoi(e) > 0 ? atoi(e) : 64;
    const char *c = getenv("KB_MTL_COMPILES");
    const int cores = (int)[NSProcessInfo processInfo].activeProcessorCount;
    cap_ = c && atoi(c) > 0 ? atoi(c) : cores > 3 ? cores - 2 : 1;
}

std::shared_ptr<MetalShaderBatch> MetalShaderCompiler::add(const std::string &entry, const std::string &source) {
    std::shared_ptr<MetalShaderBatch> full;
    std::shared_ptr<MetalShaderBatch> b;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = entries_.find(entry);
        if (it != entries_.end()) return it->second;
        if (!open_) {
            open_ = std::make_shared<MetalShaderBatch>();
            open_->source = MSL_Prelude();
            open_->seq = ++batches_;
        }
        b = open_;
        MTL_ShaderLog("c %s %d", entry.c_str(), b->seq);
        entries_[entry] = b;
        lastAdd_ = Seconds();
        std::lock_guard<std::mutex> bg(b->mu);
        b->source += source;
        if (++b->count == size_) { full = b; open_.reset(); }
    }
    if (full) enqueue(full);
    return b;
}

void MetalShaderCompiler::enqueue(const std::shared_ptr<MetalShaderBatch> &b) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (inflight_ >= cap_) { queue_.push_back(b); return; }
        ++inflight_;
    }
    submit(b);
}

// A batch compiled: Metal gets the next.
void MetalShaderCompiler::finished() {
    std::shared_ptr<MetalShaderBatch> next;
    {
        std::lock_guard<std::mutex> g(mu_);
        --inflight_;
        if (queue_.empty() || inflight_ >= cap_) return;
        next = std::move(queue_.front());
        queue_.pop_front();
        ++inflight_;
    }
    submit(next);
}

void MetalShaderCompiler::submitIdle() {
    std::shared_ptr<MetalShaderBatch> b;
    {
        std::lock_guard<std::mutex> g(mu_);
        // Shaders come in bursts while a zone loads (gaps up to ~0.25 s on mp_nuked);
        // the engine is done when it pauses longer.
        if (!open_ || Seconds() - lastAdd_ < 0.5) return;
        b = std::move(open_);
    }
    enqueue(b);
}

void MetalShaderCompiler::submit(const std::shared_ptr<MetalShaderBatch> &b) {
    NSString *src;
    {
        std::lock_guard<std::mutex> g(b->mu);
        if (b->submitted) return;
        b->submitted = true;
        src = [NSString stringWithUTF8String:b->source.c_str()];
    }
    MTLCompileOptions *opts = [MTLCompileOptions new];
    opts.preserveInvariance = YES;
    std::shared_ptr<MetalShaderBatch> keep = b;
    MTL_ShaderLog("s %d %d", b->seq, b->count);
    [dev_ newLibraryWithSource:src options:opts completionHandler:^(id<MTLLibrary> lib, NSError *e) {
        MTL_ShaderLog("d %d", keep->seq);
        if (!lib) MTL_Bug("shader batch does not compile: %s", e.localizedDescription.UTF8String);
        {
            std::lock_guard<std::mutex> g(keep->mu);
            keep->lib = lib;
            keep->done = true;
            keep->source.clear();
            keep->cv.notify_all();
        }
        finished();
    }];
}

id<MTLLibrary> MetalShaderCompiler::library(const std::shared_ptr<MetalShaderBatch> &b) {
    bool take = false;
    {
        // A draw needs it: the open batch compiles what it has, a queued one goes
        // ahead of the others, past the cap.
        std::lock_guard<std::mutex> g(mu_);
        if (open_ == b) {
            open_.reset();
            take = true;
        } else {
            auto it = std::find(queue_.begin(), queue_.end(), b);
            if (it != queue_.end()) { queue_.erase(it); take = true; }
        }
        if (take) ++inflight_;
    }
    if (take) submit(b);
    std::unique_lock<std::mutex> g(b->mu);
    if (!b->done) {
        const double t0 = Seconds();
        b->cv.wait(g, [&] { return b->done; });
        waited += Seconds() - t0;
        MTL_ShaderLog("w %d %.1f", b->seq, (Seconds() - t0) * 1000.0);
    }
    return b->lib;
}

// KB_DUMP_MSL=<dir> writes each translation as <dir>/<hash>.<vs|ps>.metal.
MetalShader::MetalShader(MetalDevice *dev, const DWORD *tokens, bool pixel) : dev_(dev), pixel_(pixel) {
    extern unsigned KB_ShaderHash(const unsigned int *tokens);
    hash = KB_ShaderHash(tokens);
    uid = dev->nextId();
    tokens_.assign(tokens, tokens + sm3::TokenCount(tokens));
    MSLShader m;
    if (!translate(0, &m)) return;
    attribMask = m.attribMask;
    samplerMask = m.samplerMask;
    shadowMask = m.shadowMask;
    memcpy(samplerDim, m.samplerDim, sizeof(samplerDim));
    constCount = m.constCount;
    varyingsOut = m.varyingsOut;
    varyingsIn = m.varyingsIn;
    entry_ = m.entry;
    batch_ = dev->compiler()->add(entry_, m.source);
    ok_ = true;
}

bool MetalShader::translate(unsigned variant, MSLShader *m) {
    sm3::Shader sh;
    std::string err;
    if (!sm3::Parse(tokens_.data(), sh, err)) { MTL_Bug("shader %08x: %s", hash, err.c_str()); return false; }
    if (sh.pixel != pixel_) { MTL_Bug("shader %08x: wrong stage", hash); return false; }
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%08x%s", hash, pixel_ ? "p" : "v");
    if (!MSL_Translate(sh, *m, variant ? std::string() : std::string(prefix), (variant & VARIANT_PER_SAMPLE) != 0,
                       GLCompat())) {
        MTL_Bug("shader %08x: %s", hash, m->error.c_str());
        return false;
    }
    if (const char *dir = getenv("KB_DUMP_MSL")) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%08x%s.%s.metal", dir, hash, variant ? ("_v" + std::to_string(variant)).c_str() : "",
                 pixel_ ? "ps" : "vs");
        if (FILE *f = fopen(path, "w")) { fputs(MSL_Prelude(), f); fputs(m->source.c_str(), f); fclose(f); }
    }
    return true;
}

id<MTLFunction> MetalShader::function(int alphaFunc, unsigned variant) {
    if (alphaFunc < 0 || alphaFunc > 8 || !pixel_) alphaFunc = 0;
    if (!ok_) return nil;
    std::lock_guard<std::mutex> g(mu_);
    Variant &v = variants_[variant];
    if (!v.lib) {
        if (!variant) {
            MTL_ShaderLog("u %s", entry_.c_str());
            v.lib = dev_->compiler()->library(batch_);
        } else {
            // A variant: rare, compiled on its own when first drawn.
            MSLShader m;
            if (!translate(variant, &m)) return nil;
            MTLCompileOptions *opts = [MTLCompileOptions new];
            opts.preserveInvariance = YES;
            NSError *e = nil;
            const std::string src = std::string(MSL_Prelude()) + m.source;
            v.lib = [dev_->mtl newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:opts error:&e];
            if (!v.lib) MTL_Bug("shader %08x (variant %x) does not compile: %s", hash, variant, e.localizedDescription.UTF8String);
        }
        if (!v.lib) return nil;
    }
    if (!v.fn[alphaFunc]) {
        NSString *name = variant ? @"kb_main" : [NSString stringWithUTF8String:entry_.c_str()];
        NSError *e = nil;
        if (!pixel_) {
            v.fn[0] = [v.lib newFunctionWithName:name];
        } else {
            MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
            int a = alphaFunc;
            [cv setConstantValue:&a type:MTLDataTypeInt atIndex:MSL_FC_ALPHAFUNC];
            v.fn[alphaFunc] = [v.lib newFunctionWithName:name constantValues:cv error:&e];
        }
        if (!v.fn[alphaFunc]) MTL_Bug("shader %08x (alpha %d): %s", hash, alphaFunc, e ? e.localizedDescription.UTF8String : "no entry");
    }
    return v.fn[alphaFunc];
}

HRESULT WINAPI MetalVertexShader::GetDevice(IDirect3DDevice9 **pp) { *pp = dev_; dev_->AddRef(); return D3D_OK; }
HRESULT WINAPI MetalPixelShader::GetDevice(IDirect3DDevice9 **pp) { *pp = dev_; dev_->AddRef(); return D3D_OK; }

// ---- Queries ------------------------------------------------------------------
// Event: done when the command buffer holding its END has finished. Occlusion: the
// samples counted between BEGIN and END, from the frame's visibility buffer.
MetalQuery::~MetalQuery() {
    if (active_) dev_->endQuery();
}

HRESULT WINAPI MetalQuery::GetDevice(IDirect3DDevice9 **pp) { *pp = dev_; dev_->AddRef(); return D3D_OK; }

HRESULT WINAPI MetalQuery::Issue(DWORD flags) {
    if (type_ == D3DQUERYTYPE_EVENT) {
        if (flags & D3DISSUE_END) serial_ = dev_->recordingSerial();
        return D3D_OK;
    }
    if (type_ != D3DQUERYTYPE_OCCLUSION) { MTL_Bug("query type %u", (unsigned)type_); return D3DERR_INVALIDCALL; }
    if (flags & D3DISSUE_BEGIN) {
        if (active_) dev_->endQuery();
        slot_ = dev_->allocQuerySlot();
        dev_->beginQuery(slot_);
        active_ = true;
        serial_ = 0;
    }
    if (flags & D3DISSUE_END) {
        if (active_) dev_->endQuery();
        active_ = false;
        serial_ = dev_->recordingSerial();
    }
    return D3D_OK;
}

HRESULT WINAPI MetalQuery::GetData(void *data, DWORD size, DWORD flags) {
    if (!serial_) {
        // Never issued: D3D9 reports an event as signalled (R_FinishGpuFence waits on
        // one it never issues, gl_query.cpp).
        if (type_ == D3DQUERYTYPE_EVENT) { if (data && size >= 4) *(DWORD *)data = TRUE; return D3D_OK; }
        return S_FALSE;
    }
    if (dev_->completedSerial() < serial_) {
        // The engine polls its GPU fence from the main thread too: only the render
        // thread commits.
        if ((flags & D3DGETDATA_FLUSH) && dev_->onRenderThread() && dev_->recordingSerial() == serial_) dev_->flush();
        return S_FALSE;
    }
    if (type_ == D3DQUERYTYPE_EVENT) { if (data && size >= 4) *(DWORD *)data = TRUE; return D3D_OK; }
    DWORD samples = 0;
    if (!dev_->queryResult(slot_, &samples)) return S_FALSE;
    if (data && size >= 4) *(DWORD *)data = samples;
    return D3D_OK;
}
