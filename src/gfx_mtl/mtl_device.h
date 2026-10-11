// mtl_device.h — the D3D9 device and factory on Metal (mtl_d3d9.h has the overview).
#ifndef KISAK_MTL_DEVICE_H
#define KISAK_MTL_DEVICE_H

#include "mtl_d3d9.h"

class GLD3D9;

// The window and its CAMetalLayer, made on the main thread (mtl_window.mm). A
// presenter thread waits for the layer's next drawable and holds it ready: the
// layer hands out at most ~120 drawables a second here (a bare CAMetalLayer on the
// 240 Hz display too, with or without display sync), and Present takes one only if
// it is ready, so the game renders faster than that, as with GL.
struct MetalWindow {
    static MetalWindow *Create(id<MTLDevice> dev, int width, int height, bool fullscreen, bool vsync, int display);
    ~MetalWindow();
    void resize(int width, int height);       // windowed size in back-buffer pixels
    void setFullscreen(bool on);
    void setVSync(bool on);
    // The ready drawable, or nil; with `wait` (vsync) waits for one.
    id<CAMetalDrawable> takeDrawable(bool wait);
    void presenterLoop();
    CAMetalLayer *layer = nil;
    void *sdlWindow = nullptr;
    void *view = nullptr;                     // SDL_MetalView
    int size[2] = {};
    bool fullscreen = false;
    std::mutex mu;
    std::condition_variable cv;
    id<CAMetalDrawable> ready;
    bool stop = false, presenterRunning = false;
};

class MetalDevice final : public MetalObject<IDirect3DDevice9> {
public:
    static const int kFramesInFlight = 3;
    static const int kStages = 20;            // pixel samplers 0..15, vertex samplers 16..19 (D3D 257..260)

    MetalDevice(MetalWindow *win, id<MTLDevice> dev, const D3DPRESENT_PARAMETERS &pp, int samples);
    ~MetalDevice() override;

    // Frame / target
    HRESULT WINAPI TestCooperativeLevel() override { return D3D_OK; }
    UINT    WINAPI GetAvailableTextureMem() override { return 256u * 1024 * 1024; }
    HRESULT WINAPI GetDeviceCaps(D3DCAPS9 *caps) override;
    HRESULT WINAPI Reset(D3DPRESENT_PARAMETERS *pp) override;
    HRESULT WINAPI Present(const RECT *, const RECT *, HWND, const RGNDATA *) override;
    HRESULT WINAPI GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **pp) override;
    HRESULT WINAPI GetSwapChain(UINT, IDirect3DSwapChain9 **pp) override;
    HRESULT WINAPI BeginScene() override { return D3D_OK; }
    HRESULT WINAPI EndScene() override { return D3D_OK; }
    HRESULT WINAPI Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override;
    HRESULT WINAPI SetViewport(const D3DVIEWPORT9 *vp) override;
    HRESULT WINAPI SetRenderTarget(DWORD index, IDirect3DSurface9 *rt) override;
    HRESULT WINAPI SetDepthStencilSurface(IDirect3DSurface9 *ds) override;

    // Resources
    HRESULT WINAPI CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                 IDirect3DTexture9 **pp, HANDLE *) override;
    HRESULT WINAPI CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                       IDirect3DVolumeTexture9 **pp, HANDLE *) override;
    HRESULT WINAPI CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                     IDirect3DCubeTexture9 **pp, HANDLE *) override;
    HRESULT WINAPI CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **pp,
                                      HANDLE *) override;
    HRESULT WINAPI CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **pp,
                                     HANDLE *) override;
    HRESULT WINAPI CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD, WINBOOL,
                                      IDirect3DSurface9 **pp, HANDLE *) override;
    HRESULT WINAPI CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD, WINBOOL,
                                             IDirect3DSurface9 **pp, HANDLE *) override;
    HRESULT WINAPI CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool, IDirect3DSurface9 **pp,
                                               HANDLE *) override;
    HRESULT WINAPI UpdateSurface(IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *) override;
    HRESULT WINAPI GetRenderTargetData(IDirect3DSurface9 *rt, IDirect3DSurface9 *dst) override;
    HRESULT WINAPI StretchRect(IDirect3DSurface9 *src, const RECT *srcRect, IDirect3DSurface9 *dst, const RECT *dstRect,
                               D3DTEXTUREFILTERTYPE filter) override;
    HRESULT WINAPI CreateVertexDeclaration(const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **pp) override;
    HRESULT WINAPI CreateVertexShader(const DWORD *f, IDirect3DVertexShader9 **pp) override;
    HRESULT WINAPI CreatePixelShader(const DWORD *f, IDirect3DPixelShader9 **pp) override;
    HRESULT WINAPI CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **pp) override;

    // State
    HRESULT WINAPI SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override;
    HRESULT WINAPI SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) override;
    HRESULT WINAPI SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
    HRESULT WINAPI SetTexture(DWORD stage, IDirect3DBaseTexture9 *tex) override;
    HRESULT WINAPI SetScissorRect(const RECT *r) override;
    HRESULT WINAPI SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *vb, UINT offset, UINT stride) override;
    HRESULT WINAPI SetIndices(IDirect3DIndexBuffer9 *ib) override;
    HRESULT WINAPI SetVertexDeclaration(IDirect3DVertexDeclaration9 *decl) override;
    HRESULT WINAPI SetVertexShader(IDirect3DVertexShader9 *vs) override;
    HRESULT WINAPI SetVertexShaderConstantF(UINT start, const float *data, UINT count) override;
    HRESULT WINAPI SetPixelShader(IDirect3DPixelShader9 *ps) override;
    HRESULT WINAPI SetPixelShaderConstantF(UINT start, const float *data, UINT count) override;
    void    WINAPI SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *) override {}

    // Draw
    HRESULT WINAPI DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) override;
    HRESULT WINAPI DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT numVertices,
                                        UINT startIndex, UINT count) override;
    HRESULT WINAPI DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT, const void *, UINT) override;

    // ---- For the resources (mtl_resources.mm) ----
    id<MTLDevice> mtl;
    bool onRenderThread() const;
    // The command buffer being recorded, and the last one the GPU finished.
    unsigned long long recordingSerial() const { return serial_.load(std::memory_order_acquire); }
    unsigned long long completedSerial() const { return completed_.load(std::memory_order_acquire); }
    // Render thread: copy `bytes` into `dst` through this frame's command buffer, ordered
    // with the draws around it.
    void copyToTexture(id<MTLTexture> dst, UINT slice, UINT level, MTLOrigin origin, MTLSize size, const void *bytes,
                       UINT pitch, UINT imageBytes);
    // Commit what is recorded and wait for the GPU (readbacks).
    void finishGPU();
    // Readback of a texture region (BGRA8 or the texture's own bytes) into `out`.
    bool readTexture(id<MTLTexture> tex, UINT slice, UINT level, UINT w, UINT h, std::vector<unsigned char> &out,
                     UINT *pitch);
    // Occlusion queries: slots of one visibility buffer, handed out in turn.
    unsigned long long allocQuerySlot();
    void beginQuery(unsigned long long slot);
    void endQuery();
    bool queryResult(unsigned long long slot, DWORD *samples);
    void flush();                             // commit the recorded commands (GetData FLUSH)
    MetalSurface *backBuffer() { return bbSurface_; }
    unsigned nextId() { return ++ids_; }
    MetalShaderCompiler *compiler() { return &compiler_; }
    MetalPipelineCache *pipelineCache() { return &pipelineCache_; }

private:
    struct Stream { MetalVertexBuffer *vb = nullptr; UINT offset = 0, stride = 0; };
    enum : unsigned {
        D_PIPELINE = 1u << 0, D_DEPTHSTENCIL = 1u << 1, D_RASTER = 1u << 2, D_VIEWPORT = 1u << 3,
        D_SCISSOR = 1u << 4, D_STENCILREF = 1u << 5, D_VSCONST = 1u << 6, D_PSCONST = 1u << 7,
        D_DRAWVALUES = 1u << 8, D_STREAMS = 1u << 9, D_ALL = 0xFFFFFFFFu,
    };

    void createBackBuffer();
    void beginCommandBuffer();
    id<MTLRenderCommandEncoder> renderEncoder();
    void endEncoder();
    void commit(bool present);
    void flushClear();
    bool prepareDraw(D3DPRIMITIVETYPE type);
    id<MTLRenderPipelineState> pipeline();
    id<MTLDepthStencilState> depthStencil();
    id<MTLSamplerState> sampler(int stage);
    id<MTLTexture> dummyTexture(int dim);
    void resolveBackBuffer();
    void resetViewportToTarget();
    void targetSize(UINT *w, UINT *h) const;
    void fullscreenPass(id<MTLTexture> src, UINT srcSlice, UINT srcLevel, const RECT &srcRect, id<MTLTexture> dst,
                        UINT dstSlice, UINT dstLevel, const RECT &dstRect, bool linear, bool clearOutside);
    void encodeBlit(id<MTLCommandBuffer> cb, id<MTLTexture> src, const RECT &srcRect, id<MTLTexture> dst, UINT dstSlice,
                    UINT dstLevel, const RECT &dstRect, bool linear, bool clearOutside);
    void stats();
    void clearRect(const RECT &r, DWORD flags, D3DCOLOR color);
    bool shadowMapsBound();

    MetalWindow *win_;
    id<MTLCommandQueue> queue_;
    dispatch_semaphore_t frameSem_;
    id<MTLCommandBuffer> cmd_;
    void *pool_ = nullptr;                    // autorelease pool of the command buffer
    id<MTLRenderCommandEncoder> enc_;
    id<MTLBlitCommandEncoder> blit_;
    std::atomic<unsigned long long> serial_{1}, completed_{0};
    bool frameOpen_ = false;
    pthread_t renderThread_;
    std::atomic<unsigned> ids_{0};
    MetalShaderCompiler compiler_;
    MetalPipelineCache pipelineCache_;
    // copyToTexture's staging memory: a buffer per frame in flight, reused once that frame's
    // command buffers have completed (the semaphore), instead of a new buffer per upload
    // (the model lighting volume, 1 MB a frame).
    id<MTLBuffer> staging_[kFramesInFlight] = {};
    NSUInteger stagingUsed_ = 0;
    unsigned frameCount_ = 0;

    // Back buffer (our own texture; Present draws it into the drawable) and the
    // device's automatic depth-stencil surface.
    int bbWidth_ = 0, bbHeight_ = 0, samples_ = 1;
    bool fullscreen_ = false, vsync_ = false;
    MetalTexture *bbColor_ = nullptr, *bbResolve_ = nullptr, *autoDepth_ = nullptr;
    MetalSurface *bbSurface_ = nullptr, *autoDSSurface_ = nullptr;
    MetalSwapChain *swapChain_ = nullptr;
    bool bbResolved_ = true;

    // D3D9 state.
    MetalSurface *rt_[4] = {};
    MetalSurface *ds_ = nullptr;
    D3DVIEWPORT9 vp_ = {};
    RECT scissor_ = {};
    DWORD rs_[256] = {};
    DWORD samp_[kStages][14] = {};
    IDirect3DBaseTexture9 *tex_[kStages] = {};
    MetalTexture *texm_[kStages] = {};
    Stream streams_[4];
    MetalIndexBuffer *ib_ = nullptr;
    MetalDecl *decl_ = nullptr;
    MetalVertexShader *vs_ = nullptr;
    MetalPixelShader *ps_ = nullptr;
    float vsc_[256 * 4] = {}, psc_[256 * 4] = {};
    unsigned dirty_ = D_ALL;
    unsigned texDirty_ = ~0u;                 // stages whose texture or sampler changed

    // A Clear to apply as the next pass's load actions.
    DWORD clearFlags_ = 0;
    D3DCOLOR clearColor_ = 0;
    float clearZ_ = 1.0f;
    DWORD clearStencil_ = 0;

    // What the open render encoder has bound.
    struct Bound {
        id<MTLRenderPipelineState> pipe;
        id<MTLDepthStencilState> dss;
        id<MTLBuffer> vb[4];
        NSUInteger vbOffset[4];
        MetalVertexBuffer *vbObj[4];
        unsigned vbGen[4];
        id<MTLBuffer> ib;
        MetalIndexBuffer *ibObj;
        unsigned ibGen;
        id<MTLTexture> tex[kStages];
        id<MTLSamplerState> smp[kStages];
        int vsConsts, psConsts;
    } bound_;
    int passWidth_ = 0, passHeight_ = 0;

    // Caches.
    struct PipeKey {
        unsigned vs, ps, decl;
        unsigned short colorFmt, depthFmt, strides[4];
        unsigned char samples, alphaFunc, blendOn, src, dst, op, srcA, dstA, opA, mask, aa, pad;
        bool operator==(const PipeKey &o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
    };
    struct PipeHash { size_t operator()(const PipeKey &k) const; };
    // A pipeline and how it was made (for the pipeline cache's map file; epoch: the map it was
    // last given to).
    struct PipeEntry {
        id<MTLRenderPipelineState> state;
        MTLRenderPipelineDescriptor *desc;
        MTLFunctionDescriptor *fd;
        id<MTLLibrary> lib;
        unsigned epoch;
    };
    std::unordered_map<PipeKey, PipeEntry, PipeHash> pipes_;
    std::unordered_map<unsigned long long, id<MTLDepthStencilState>> dss_;
    std::unordered_map<unsigned long long, id<MTLSamplerState>> samplers_;
    id<MTLTexture> dummy_[5] = {};
    // Presentation: the back buffer scaled into one of these by the render queue, then
    // copied into the drawable by its own queue, which may wait for the display without
    // holding up the frames behind it.
    id<MTLCommandQueue> presentQueue_;
    id<MTLEvent> presentEvent_;
    unsigned long long presentValue_ = 0;
    id<MTLTexture> presentTex_[3] = {};
    std::atomic<bool> presentBusy_[3] = {};
    int presentSlot_ = 0;
    id<MTLBuffer> defaults_;                  // constant attributes (0,0,0,1) and (1,1,1,1)
    id<MTLRenderPipelineState> blitPipes_[8] = {};   // fullscreen pass per target format
    MTLPixelFormat blitFormats_[8] = {};
    std::unordered_map<unsigned long long, id<MTLRenderPipelineState>> clearPipes_;
    id<MTLLibrary> utilLib_;
    id<MTLSamplerState> pointClamp_, linearClamp_;

    // Occlusion queries: a slot is used again kQuerySlots queries later (the game
    // issues ~24 a frame and reads them a frame or two later).
    static const int kQuerySlots = 32768;    // 256 KB, the largest visibility buffer Metal takes
    id<MTLBuffer> visibility_;
    unsigned long long queryCount_ = 0;
    int activeQuery_ = -1;

    // KB_FPS. Presented: frames given a drawable; shown: those the display showed.
    double statsStart_ = 0, statsLast_ = 0, statsSum_ = 0, statsMax_ = 0, statsWait_ = 0, statsBuild_ = 0;
    double statsBatchWait_ = 0, statsFunctions_ = 0, statsBuildMax_ = 0;   // of statsBuild_
    unsigned statsFrames_ = 0, statsPresented_ = 0, statsPipes_ = 0, statsCacheHits_ = 0;
    double frameBuild_ = 0;                   // this frame's pipeline builds (KB_MTL_SHADERLOG)
    unsigned framePipes_ = 0;
    std::atomic<unsigned> statsShown_{0};
    unsigned long statsDraws_ = 0, statsBlits_ = 0, statsUploads_ = 0;
    unsigned long long statsUploadBytes_ = 0;
};

// The factory: adapters and formats as the GL layer answers them (the engine's choices
// must not depend on the backend), devices on Metal.
class MetalD3D9 final : public MetalObject<IDirect3D9> {
public:
    MetalD3D9();
    ~MetalD3D9() override;
    UINT    WINAPI GetAdapterCount() override;
    HRESULT WINAPI GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9 *id) override;
    UINT    WINAPI GetAdapterModeCount(UINT adapter, D3DFORMAT fmt) override;
    HRESULT WINAPI EnumAdapterModes(UINT adapter, D3DFORMAT fmt, UINT mode, D3DDISPLAYMODE *m) override;
    HMONITOR WINAPI GetAdapterMonitor(UINT adapter) override;
    HRESULT WINAPI GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE *m) override;
    HRESULT WINAPI GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS9 *caps) override;
    HRESULT WINAPI CheckDeviceType(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, WINBOOL) override { return D3D_OK; }
    HRESULT WINAPI CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD usage, D3DRESOURCETYPE rt,
                                     D3DFORMAT f) override;
    HRESULT WINAPI CheckDeviceMultiSampleType(UINT, D3DDEVTYPE, D3DFORMAT, WINBOOL, D3DMULTISAMPLE_TYPE type,
                                              DWORD *q) override;
    HRESULT WINAPI CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) override { return D3D_OK; }
    HRESULT WINAPI CreateDevice(UINT adapter, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *pp,
                                IDirect3DDevice9 **out) override;
private:
    IDirect3D9 *gl_;
    id<MTLDevice> dev_;
};

#endif // KISAK_MTL_DEVICE_H
