// mtl_d3d9.h — the D3D9 interfaces on Metal (macOS), beside the GL layer (src/gfx_gl).
//
// The renderer in src/gfx_d3d calls Direct3DCreate9/CreateDevice as on Windows;
// KB_RENDERER=metal picks this backend at startup (mtl_backend.mm), else GL. It
// implements what the game uses, listed by the GL layer's KB_SPEC recorder (HANDOFF,
// "Metal backend"); anything else is reported once as "[mtl] BUG" (KB_MTL_STRICT=1
// aborts there instead).
//
// Threads: the D3D9 device, and every Metal command, belongs to the render thread
// (the thread that created the device). Resources are created and filled where the
// engine does it (loader threads too): Metal's device and resource methods are thread
// safe, and a buffer or texture the GPU may be reading is never written in place
// except through the D3D9 contracts (NOOVERWRITE).
//
// Frames: up to kFramesInFlight command buffers behind one semaphore. The CPU waits
// there, for a drawable, and for readbacks (screenshots, GetRenderTargetData), nowhere
// else.
#ifndef KISAK_MTL_D3D9_H
#define KISAK_MTL_D3D9_H

// The Win32 shim's BOOL is an int, Objective-C's a bool: the D3D9 declarations see
// WINBOOL (the shim's other name for it); BOOL is Objective-C's from here on.
#define BOOL WINBOOL
#include <d3d9.h>
#undef BOOL
#include <atomic>
#include <condition_variable>
#include <thread>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

class MetalDevice;
struct MetalWindow;
struct MSLShader;

// "[mtl] BUG: <message>" the first time a message is seen (KB_MTL_STRICT=1: abort).
void MTL_Bug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

template <class IFACE>
class MetalObject : public IFACE {
public:
    HRESULT WINAPI QueryInterface(REFIID, void **ppv) override { if (ppv) *ppv = this; AddRef(); return S_OK; }
    ULONG WINAPI AddRef() override  { return ++ref_; }
    ULONG WINAPI Release() override { ULONG r = --ref_; if (r == 0) delete this; return r; }
protected:
    virtual ~MetalObject() {}
    std::atomic<ULONG> ref_{1};
};

// ---- Formats --------------------------------------------------------------
struct MetalFormat {
    unsigned pixelFormat = 0;   // MTLPixelFormat, 0 = unsupported
    int      bytes = 0;         // per pixel, or per 4x4 block when compressed
    bool     compressed = false;
    bool     depth = false, stencil = false;
    unsigned char swizzle[4] = { 2, 3, 4, 5 };   // MTLTextureSwizzle r,g,b,a (identity)
};
MetalFormat MTL_Format(D3DFORMAT f, bool depthUsage);

// ---- Vertex and index buffers ----------------------------------------------
// Lock returns a pointer into the MTLBuffer (unified memory). DISCARD gives the buffer
// another backing store when the GPU may still read the current one: the old ones are
// reused once the frames that used them are done.
class MetalBuffer {
public:
    MetalBuffer(MetalDevice *dev, UINT length, DWORD usage);
    ~MetalBuffer();
    void *lock(UINT offset, UINT size, DWORD flags);
    id<MTLBuffer> current();   // render thread: the backing to draw from (marks it used this frame)
    // Unique to this buffer and backing (a DISCARD changes it): the device binds again.
    unsigned generation() const { return gen_.load(std::memory_order_acquire); }
    static std::atomic<unsigned> generations;
    UINT length() const { return length_; }
    bool dynamic() const { return (usage_ & D3DUSAGE_DYNAMIC) != 0; }
    DWORD usage() const { return usage_; }
private:
    struct Backing { id<MTLBuffer> buf; unsigned long long lastUse; };
    MetalDevice *dev_;
    UINT length_;
    DWORD usage_;
    std::mutex mu_;
    std::vector<Backing> backings_;   // [0] is the current one
    std::atomic<unsigned> gen_{0};
};

class MetalVertexBuffer final : public MetalObject<IDirect3DVertexBuffer9> {
public:
    MetalVertexBuffer(MetalDevice *dev, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool)
        : dev_(dev), buf_(dev, length, usage), fvf_(fvf), pool_(pool) {}
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **pp) override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_VERTEXBUFFER; }
    DWORD   WINAPI SetPriority(DWORD) override { return 0; }
    DWORD   WINAPI GetPriority() override { return 0; }
    void    WINAPI PreLoad() override {}
    HRESULT WINAPI Lock(UINT off, UINT size, void **pp, DWORD flags) override;
    HRESULT WINAPI Unlock() override { return D3D_OK; }
    HRESULT WINAPI GetDesc(D3DVERTEXBUFFER_DESC *d) override;
    MetalBuffer &buffer() { return buf_; }
private:
    MetalDevice *dev_;
    MetalBuffer buf_;
    DWORD fvf_;
    D3DPOOL pool_;
};

class MetalIndexBuffer final : public MetalObject<IDirect3DIndexBuffer9> {
public:
    MetalIndexBuffer(MetalDevice *dev, UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
        : dev_(dev), buf_(dev, length, usage), format_(fmt), pool_(pool) {}
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **pp) override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_INDEXBUFFER; }
    DWORD   WINAPI SetPriority(DWORD) override { return 0; }
    DWORD   WINAPI GetPriority() override { return 0; }
    void    WINAPI PreLoad() override {}
    HRESULT WINAPI Lock(UINT off, UINT size, void **pp, DWORD flags) override;
    HRESULT WINAPI Unlock() override { return D3D_OK; }
    HRESULT WINAPI GetDesc(D3DINDEXBUFFER_DESC *d) override;
    MetalBuffer &buffer() { return buf_; }
    D3DFORMAT format() const { return format_; }
private:
    MetalDevice *dev_;
    MetalBuffer buf_;
    D3DFORMAT format_;
    D3DPOOL pool_;
};

// ---- Textures and surfaces --------------------------------------------------
// One class for 2D, cube and volume textures. CPU-filled textures are shared storage,
// written with replaceRegion at unlock while the GPU has never used them; after that
// an unlock copies through the frame's command buffer (render thread). Render targets
// and depth textures are private.
class MetalSurface;

class MetalTexture {
public:
    enum Kind { TEX_2D, TEX_CUBE, TEX_VOLUME };
    // `owner` is the D3D9 texture object (its surfaces' container).
    MetalTexture(MetalDevice *dev, IUnknown *owner, Kind kind, UINT w, UINT h, UINT d, UINT levels, DWORD usage,
                 D3DFORMAT fmt, D3DPOOL pool);
    // An existing texture (the back buffer and its depth).
    MetalTexture(MetalDevice *dev, id<MTLTexture> tex, D3DFORMAT fmt, DWORD usage);
    ~MetalTexture();
    Kind kind() const { return kind_; }
    UINT width(UINT level = 0) const;
    UINT height(UINT level = 0) const;
    UINT depthOf(UINT level = 0) const;
    UINT levels() const { return levels_; }
    DWORD usage() const { return usage_; }
    D3DFORMAT format() const { return format_; }
    D3DPOOL pool() const { return pool_; }
    const MetalFormat &mformat() const { return mfmt_; }
    bool isDepth() const { return mfmt_.depth; }
    id<MTLTexture> texture() const { return tex_; }
    id<MTLTexture> sampleView() const { return view_ ? view_ : tex_; }   // with the format's swizzle
    void markUsed() { used_.store(true, std::memory_order_relaxed); }
    HRESULT lockRect(UINT face, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags);
    HRESULT unlockRect(UINT face, UINT level);
    HRESULT lockBox(UINT level, D3DLOCKED_BOX *lb, const D3DBOX *box, DWORD flags);
    HRESULT unlockBox(UINT level);
    MetalSurface *surface(UINT face, UINT level);   // owned by the texture, counted with it
private:
    struct Pending { std::vector<unsigned char> data; UINT face = 0, level = 0; bool locked = false; };
    MetalDevice *dev_;
    IUnknown *owner_;
    Kind kind_;
    UINT width_, height_, depth_, levels_;
    DWORD usage_;
    D3DFORMAT format_;
    D3DPOOL pool_;
    MetalFormat mfmt_;
    id<MTLTexture> tex_ = nullptr;
    id<MTLTexture> view_ = nullptr;
    std::atomic<bool> used_{false};
    std::mutex mu_;
    std::vector<Pending> locks_;              // open locks (face, level)
    std::vector<MetalSurface *> surfaces_;    // created by surface(), released with the texture
    // A lock without DISCARD returns the level's contents (the engine patches part of the
    // model lighting volume every frame). Levels last written through the GPU keep their
    // bytes here; others are read from the (shared) texture.
    std::vector<std::vector<unsigned char>> kept_;   // [face * levels + level]
    std::vector<bool> written_;
    void upload(Pending &p);
    void fillLock(Pending &p, UINT pitch, UINT rows);
    size_t levelBytes(UINT level, UINT *pitch, UINT *rows) const;
};

class MetalTexture2D final : public MetalObject<IDirect3DTexture9> {
public:
    MetalTexture2D(MetalDevice *dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
        : dev_(dev), t_(dev, this, MetalTexture::TEX_2D, w, h, 1, levels, usage, fmt, pool) {}
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **pp) override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_TEXTURE; }
    DWORD   WINAPI SetPriority(DWORD) override { return 0; }
    DWORD   WINAPI GetPriority() override { return 0; }
    void    WINAPI PreLoad() override {}
    DWORD   WINAPI SetLOD(DWORD) override;
    DWORD   WINAPI GetLOD() override { return 0; }
    DWORD   WINAPI GetLevelCount() override { return t_.levels(); }
    HRESULT WINAPI GetLevelDesc(UINT level, D3DSURFACE_DESC *d) override;
    HRESULT WINAPI GetSurfaceLevel(UINT level, IDirect3DSurface9 **pp) override;
    HRESULT WINAPI LockRect(UINT level, D3DLOCKED_RECT *lr, const RECT *r, DWORD f) override { return t_.lockRect(0, level, lr, r, f); }
    HRESULT WINAPI UnlockRect(UINT level) override { return t_.unlockRect(0, level); }
    HRESULT WINAPI AddDirtyRect(const RECT *) override;
    MetalTexture &tex() { return t_; }
private:
    MetalDevice *dev_;
    MetalTexture t_;
};

class MetalCubeTexture final : public MetalObject<IDirect3DCubeTexture9> {
public:
    MetalCubeTexture(MetalDevice *dev, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
        : dev_(dev), t_(dev, this, MetalTexture::TEX_CUBE, edge, edge, 1, levels, usage, fmt, pool) {}
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **pp) override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_CUBETEXTURE; }
    DWORD   WINAPI SetPriority(DWORD) override { return 0; }
    DWORD   WINAPI GetPriority() override { return 0; }
    void    WINAPI PreLoad() override {}
    DWORD   WINAPI SetLOD(DWORD) override;
    DWORD   WINAPI GetLOD() override { return 0; }
    DWORD   WINAPI GetLevelCount() override { return t_.levels(); }
    HRESULT WINAPI GetLevelDesc(UINT level, D3DSURFACE_DESC *d) override;
    HRESULT WINAPI GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **pp) override;
    HRESULT WINAPI LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *lr, const RECT *r, DWORD f) override
        { return t_.lockRect((UINT)face, level, lr, r, f); }
    HRESULT WINAPI UnlockRect(D3DCUBEMAP_FACES face, UINT level) override { return t_.unlockRect((UINT)face, level); }
    MetalTexture &tex() { return t_; }
private:
    MetalDevice *dev_;
    MetalTexture t_;
};

class MetalVolumeTexture final : public MetalObject<IDirect3DVolumeTexture9> {
public:
    MetalVolumeTexture(MetalDevice *dev, UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
        : dev_(dev), t_(dev, this, MetalTexture::TEX_VOLUME, w, h, d, levels, usage, fmt, pool) {}
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **pp) override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_VOLUMETEXTURE; }
    DWORD   WINAPI SetPriority(DWORD) override { return 0; }
    DWORD   WINAPI GetPriority() override { return 0; }
    void    WINAPI PreLoad() override {}
    DWORD   WINAPI SetLOD(DWORD) override;
    DWORD   WINAPI GetLOD() override { return 0; }
    DWORD   WINAPI GetLevelCount() override { return t_.levels(); }
    HRESULT WINAPI GetLevelDesc(UINT level, D3DVOLUME_DESC *d) override;
    HRESULT WINAPI LockBox(UINT level, D3DLOCKED_BOX *lb, const D3DBOX *b, DWORD f) override { return t_.lockBox(level, lb, b, f); }
    HRESULT WINAPI UnlockBox(UINT level) override { return t_.unlockBox(level); }
    MetalTexture &tex() { return t_; }
private:
    MetalDevice *dev_;
    MetalTexture t_;
};

// A surface: a texture's level or cube face (render target or depth-stencil), a
// standalone render target or depth-stencil, a system-memory surface, or the back
// buffer. A level surface shares its texture's reference count, as in D3D9.
class MetalSurface final : public MetalObject<IDirect3DSurface9> {
public:
    enum Kind { S_LEVEL, S_RENDERTARGET, S_DEPTHSTENCIL, S_SYSMEM, S_BACKBUFFER };
    MetalSurface(MetalDevice *dev, Kind kind, UINT w, UINT h, D3DFORMAT fmt, IUnknown *container,
                 MetalTexture *tex, UINT face, UINT level);
    ~MetalSurface() override;
    ULONG WINAPI AddRef() override  { return container_ ? container_->AddRef() : MetalObject::AddRef(); }
    ULONG WINAPI Release() override { return container_ ? container_->Release() : MetalObject::Release(); }
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **pp) override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_SURFACE; }
    DWORD   WINAPI SetPriority(DWORD) override { return 0; }
    DWORD   WINAPI GetPriority() override { return 0; }
    void    WINAPI PreLoad() override {}
    HRESULT WINAPI GetContainer(REFIID, void **pp) override;
    HRESULT WINAPI GetDesc(D3DSURFACE_DESC *d) override;
    HRESULT WINAPI LockRect(D3DLOCKED_RECT *lr, const RECT *r, DWORD f) override;
    HRESULT WINAPI UnlockRect() override;

    Kind kind() const { return kind_; }
    UINT width() const { return width_; }
    UINT height() const { return height_; }
    D3DFORMAT format() const { return format_; }
    UINT face() const { return face_; }
    UINT level() const { return level_; }
    MetalTexture *tex() const { return tex_; }          // S_LEVEL, S_RENDERTARGET, S_DEPTHSTENCIL
    std::vector<unsigned char> &sysmem() { return sysmem_; }
    void setSize(UINT w, UINT h) { width_ = w; height_ = h; }   // the back buffer after Reset
    void setTexture(MetalTexture *t, bool own) { tex_ = t; if (own) own_ = t; }
private:
    MetalDevice *dev_;
    Kind kind_;
    UINT width_, height_;
    D3DFORMAT format_;
    IUnknown *container_;         // a level surface's texture
    MetalTexture *tex_;
    MetalTexture *own_ = nullptr; // a standalone target's texture
    UINT face_, level_;
    std::vector<unsigned char> sysmem_;
};

class MetalSwapChain final : public MetalObject<IDirect3DSwapChain9> {
public:
    explicit MetalSwapChain(MetalDevice *dev) : dev_(dev) {}
    HRESULT WINAPI Present(const RECT *, const RECT *, HWND, const RGNDATA *, DWORD) override;
    HRESULT WINAPI GetFrontBufferData(IDirect3DSurface9 *dst) override;
    HRESULT WINAPI GetBackBuffer(UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **pp) override;
private:
    MetalDevice *dev_;
};

// ---- Declarations, shaders, queries -----------------------------------------
class MetalDecl final : public MetalObject<IDirect3DVertexDeclaration9> {
public:
    MetalDecl(MetalDevice *dev, const D3DVERTEXELEMENT9 *e);
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **pp) override;
    HRESULT WINAPI GetDeclaration(D3DVERTEXELEMENT9 *e, UINT *n) override;
    struct Attr { int index; unsigned format; unsigned offset; unsigned stream; };
    std::vector<D3DVERTEXELEMENT9> elements;
    std::vector<Attr> attrs;      // Metal attribute index, MTLVertexFormat, offset, stream
    unsigned attribMask = 0;
    unsigned streamMask = 0;
    unsigned uid;                 // for pipeline keys
private:
    MetalDevice *dev_;
};

// Shader libraries compiled in the background, many shaders to a library: the system
// compiler takes ~70 ms for a lone pixel shader and ~8 ms each in a library of a few
// hundred (it compiles a library's functions in parallel), and caches what it has
// compiled. A batch is submitted when full or when a draw needs one of its shaders.
struct MetalShaderBatch {
    std::string source;
    int count = 0;
    bool submitted = false, done = false;
    id<MTLLibrary> lib;
    std::mutex mu;
    std::condition_variable cv;
};

class MetalShaderCompiler {
public:
    explicit MetalShaderCompiler(id<MTLDevice> dev) : dev_(dev) {}
    // The batch compiling `entry` (whose MSL is `source`); the same shader again
    // (same bytecode) shares its first batch.
    std::shared_ptr<MetalShaderBatch> add(const std::string &entry, const std::string &source);
    id<MTLLibrary> library(const std::shared_ptr<MetalShaderBatch> &b);   // waits, submitting it if needed
private:
    void submit(const std::shared_ptr<MetalShaderBatch> &b);
    id<MTLDevice> dev_;
    std::mutex mu_;
    std::shared_ptr<MetalShaderBatch> open_;
    std::unordered_map<std::string, std::shared_ptr<MetalShaderBatch>> entries_;
};

// A shader's MSL, translated when the engine creates it and compiled in a batch. A
// pixel shader is specialised for the alpha test, and translated again (a library of
// its own, made at first use) for the per-sample variant (transparency AA).
class MetalShader {
public:
    MetalShader(MetalDevice *dev, const DWORD *tokens, bool pixel);
    bool ok() const { return ok_; }
    bool pixel() const { return pixel_; }
    enum { VARIANT_PER_SAMPLE = 1 };
    id<MTLFunction> function(int alphaFunc, unsigned variant);
    unsigned hash = 0, uid = 0;
    unsigned attribMask = 0, samplerMask = 0, compareMask = 0;
    unsigned char samplerDim[16] = {};
    int constCount = 0;
    unsigned long long varyingsOut = 0, varyingsIn = 0;
private:
    struct Variant { id<MTLLibrary> lib; id<MTLFunction> fn[9]; };
    MetalDevice *dev_;
    bool pixel_, ok_ = false;
    std::vector<DWORD> tokens_;
    std::string entry_;
    std::shared_ptr<MetalShaderBatch> batch_;
    std::mutex mu_;
    std::unordered_map<unsigned, Variant> variants_;
    bool translate(unsigned variant, MSLShader *out);
};

class MetalVertexShader final : public MetalObject<IDirect3DVertexShader9> {
public:
    MetalVertexShader(MetalDevice *dev, const DWORD *t) : dev_(dev), s(dev, t, false) {}
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **pp) override;
    HRESULT WINAPI GetFunction(void *, UINT *n) override { if (n) *n = 0; return D3D_OK; }
    MetalDevice *dev_;
    MetalShader s;
};

class MetalPixelShader final : public MetalObject<IDirect3DPixelShader9> {
public:
    MetalPixelShader(MetalDevice *dev, const DWORD *t) : dev_(dev), s(dev, t, true) {}
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **pp) override;
    HRESULT WINAPI GetFunction(void *, UINT *n) override { if (n) *n = 0; return D3D_OK; }
    MetalDevice *dev_;
    MetalShader s;
};

class MetalQuery final : public MetalObject<IDirect3DQuery9> {
public:
    MetalQuery(MetalDevice *dev, D3DQUERYTYPE type) : dev_(dev), type_(type) {}
    ~MetalQuery() override;
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **pp) override;
    D3DQUERYTYPE WINAPI GetType() override { return type_; }
    DWORD   WINAPI GetDataSize() override { return sizeof(DWORD); }
    HRESULT WINAPI Issue(DWORD flags) override;
    HRESULT WINAPI GetData(void *data, DWORD size, DWORD flags) override;
private:
    MetalDevice *dev_;
    D3DQUERYTYPE type_;
    unsigned long long serial_ = 0;   // command buffer holding the END; 0 = never issued
    unsigned long long slot_ = 0;     // occlusion: visibility result slot (MetalDevice::allocQuerySlot)
    bool active_ = false;
};

#endif // KISAK_MTL_D3D9_H
