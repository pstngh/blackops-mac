// mtl_device.mm — the D3D9 device on Metal: frames, render passes, the D3D9 state and
// its translation at each draw, copies, readbacks and presentation (mtl_d3d9.h).
#include "mtl_device.h"
#include "mtl_msl.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

// Shared with the GL layer (gl_query.cpp, gl_d3d9.cpp, glcontext_sdl.cpp).
extern unsigned long g_kbDraws, g_kbBlits, g_kbUploads, g_kbPresentEnter;
extern unsigned long long g_kbUploadBytes;
void KB_NotePresentRect(int x, int y, int w, int h, int bbWidth, int bbHeight);
// The end of a map (cl_cgame_mp.cpp, main thread): the render thread writes the map's
// pipeline cache file at its next Present (or as the device goes, at a quit).
extern void (*g_kbRendererLevelEnd)(const char *mapName);
static std::mutex g_kbLevelEndMu;
static std::string g_kbLevelEndMap;
static std::atomic<bool> g_kbLevelEnded;
static void MTL_LevelEnd(const char *mapName) {
    std::string map = mapName ? mapName : "";
    const size_t slash = map.find_last_of('/');
    if (slash != std::string::npos) map = map.substr(slash + 1);
    const size_t dot = map.find('.');
    if (dot != std::string::npos) map = map.substr(0, dot);
    std::lock_guard<std::mutex> g(g_kbLevelEndMu);
    g_kbLevelEndMap = map;
    g_kbLevelEnded.store(true, std::memory_order_release);
}
static bool MTL_TakeLevelEnd(std::string *map) {
    if (!g_kbLevelEnded.exchange(false, std::memory_order_acq_rel)) return false;
    std::lock_guard<std::mutex> g(g_kbLevelEndMu);
    *map = g_kbLevelEndMap;
    return true;
}

// The Objective-C runtime's autorelease pool calls (what @autoreleasepool compiles to):
// a pool spans each command buffer, as the engine's render thread has none. Pools nest:
// no @autoreleasepool scope may hold a commit of a command buffer begun before it.
extern "C" void *objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void *pool);

namespace {

// Fullscreen copies: StretchRect with scaling or format conversion, and Present.
const char *kUtilMSL = R"(#include <metal_stdlib>
using namespace metal;
struct BlitOut { float4 pos [[position]]; float2 uv; };
vertex BlitOut kb_blit_vs(uint vid [[vertex_id]], constant float4 &r [[buffer(0)]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    BlitOut o;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    o.uv = mix(r.xy, r.zw, p);
    return o;
}
fragment float4 kb_blit_ps(BlitOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return t.sample(s, in.uv);
}
struct ClearOut { float4 pos [[position]]; };
vertex ClearOut kb_clear_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    ClearOut o;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    return o;
}
fragment float4 kb_clear_ps(constant float4 &c [[buffer(0)]]) { return c; }
)";

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

MTLBlendFactor BlendFactor(DWORD d) {
    switch (d) {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    default: MTL_Bug("blend factor %u", (unsigned)d); return MTLBlendFactorOne;
    }
}

MTLBlendOperation BlendOp(DWORD d) {
    switch (d) {
    case 1: return MTLBlendOperationAdd;
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: MTL_Bug("blend op %u", (unsigned)d); return MTLBlendOperationAdd;
    }
}

MTLCompareFunction CompareFunc(DWORD d) {
    switch (d) {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    case 8: return MTLCompareFunctionAlways;
    default: MTL_Bug("compare function %u", (unsigned)d); return MTLCompareFunctionAlways;
    }
}

MTLStencilOperation StencilOp(DWORD d) {
    switch (d) {
    case 1: return MTLStencilOperationKeep;
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: MTL_Bug("stencil op %u", (unsigned)d); return MTLStencilOperationKeep;
    }
}

MTLSamplerAddressMode AddressMode(DWORD d) {
    switch (d) {
    case 1: return MTLSamplerAddressModeRepeat;
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    default: MTL_Bug("texture address mode %u", (unsigned)d); return MTLSamplerAddressModeRepeat;
    }
}

// The render states this backend implements (the GL layer's KB_SPEC list).
bool KnownRenderState(unsigned s) {
    switch (s) {
    case D3DRS_ZENABLE: case D3DRS_FILLMODE: case D3DRS_ZWRITEENABLE: case D3DRS_ALPHATESTENABLE:
    case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_CULLMODE: case D3DRS_ZFUNC: case D3DRS_ALPHAREF:
    case D3DRS_ALPHAFUNC: case D3DRS_ALPHABLENDENABLE: case D3DRS_STENCILENABLE: case D3DRS_STENCILFAIL:
    case D3DRS_STENCILZFAIL: case D3DRS_STENCILPASS: case D3DRS_STENCILFUNC: case D3DRS_STENCILREF:
    case D3DRS_STENCILMASK: case D3DRS_STENCILWRITEMASK: case D3DRS_COLORWRITEENABLE: case D3DRS_BLENDOP:
    case D3DRS_SCISSORTESTENABLE: case D3DRS_SLOPESCALEDEPTHBIAS: case D3DRS_DEPTHBIAS:
    case D3DRS_TWOSIDEDSTENCILMODE: case D3DRS_CCW_STENCILFAIL: case D3DRS_CCW_STENCILZFAIL:
    case D3DRS_CCW_STENCILPASS: case D3DRS_CCW_STENCILFUNC: case D3DRS_ADAPTIVETESS_Y:
    case D3DRS_SEPARATEALPHABLENDENABLE: case D3DRS_SRCBLENDALPHA: case D3DRS_DESTBLENDALPHA: case D3DRS_BLENDOPALPHA:
        return true;
    default:
        return false;
    }
}

} // namespace

// KB_MTL_GLCOMPAT=1: sample as the GL layer does, for pixel comparisons with it: no
// mipmaps (it binds every texture with a non-mipmapped filter) and shadow maps read as
// raw depth, unfiltered (no hardware comparison), and no separate alpha blending.
bool GLCompat() {
    static const bool on = getenv("KB_MTL_GLCOMPAT") && *getenv("KB_MTL_GLCOMPAT") == '1';
    return on;
}

// The device holds a reference to what is bound, as D3D9's does.
template <class T> static void Rebind(T *&slot, T *obj) {
    if (obj) obj->AddRef();
    if (slot) slot->Release();
    slot = obj;
}

size_t MetalDevice::PipeHash::operator()(const PipeKey &k) const {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(&k);
    size_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(k); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// ---- Construction ---------------------------------------------------------------
MetalDevice::MetalDevice(MetalWindow *win, id<MTLDevice> dev, const D3DPRESENT_PARAMETERS &pp, int samples)
    : mtl(dev), win_(win), compiler_(dev) {
    renderThread_ = pthread_self();
    queue_ = [dev newCommandQueue];
    presentQueue_ = [dev newCommandQueue];
    presentEvent_ = [dev newEvent];
    frameSem_ = dispatch_semaphore_create(kFramesInFlight);
    bbWidth_ = (int)pp.BackBufferWidth;
    bbHeight_ = (int)pp.BackBufferHeight;
    samples_ = samples;
    fullscreen_ = !pp.Windowed;
    vsync_ = pp.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE;

    // D3D9's initial states.
    rs_[D3DRS_ZENABLE] = pp.EnableAutoDepthStencil ? 1 : 0;
    rs_[D3DRS_FILLMODE] = 3;
    rs_[D3DRS_ZWRITEENABLE] = 1;
    rs_[D3DRS_SRCBLEND] = 2;
    rs_[D3DRS_DESTBLEND] = 1;
    rs_[D3DRS_CULLMODE] = 3;
    rs_[D3DRS_ZFUNC] = 4;
    rs_[D3DRS_ALPHAFUNC] = 8;
    rs_[D3DRS_STENCILFAIL] = rs_[D3DRS_STENCILZFAIL] = rs_[D3DRS_STENCILPASS] = 1;
    rs_[D3DRS_CCW_STENCILFAIL] = rs_[D3DRS_CCW_STENCILZFAIL] = rs_[D3DRS_CCW_STENCILPASS] = 1;
    rs_[D3DRS_STENCILFUNC] = rs_[D3DRS_CCW_STENCILFUNC] = 8;
    rs_[D3DRS_STENCILMASK] = rs_[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFFu;
    rs_[D3DRS_COLORWRITEENABLE] = 0xF;
    rs_[D3DRS_BLENDOP] = rs_[D3DRS_BLENDOPALPHA] = 1;
    rs_[D3DRS_SRCBLENDALPHA] = 2;
    rs_[D3DRS_DESTBLENDALPHA] = 1;
    for (auto &s : samp_) {
        s[D3DSAMP_ADDRESSU] = s[D3DSAMP_ADDRESSV] = s[D3DSAMP_ADDRESSW] = 1;
        s[D3DSAMP_MAGFILTER] = s[D3DSAMP_MINFILTER] = 1;
        s[D3DSAMP_MIPFILTER] = 0;
        s[D3DSAMP_MAXANISOTROPY] = 1;
    }

    static const float defaults[8] = { 0, 0, 0, 1, 1, 1, 1, 1 };
    defaults_ = [dev newBufferWithBytes:defaults length:sizeof(defaults) options:MTLResourceStorageModeShared];
    visibility_ = [dev newBufferWithLength:kQuerySlots * 8 options:MTLResourceStorageModeShared];

    NSError *e = nil;
    utilLib_ = [dev newLibraryWithSource:@(kUtilMSL) options:nil error:&e];
    if (!utilLib_) MTL_Bug("utility shaders: %s", e.localizedDescription.UTF8String);
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    pointClamp_ = [dev newSamplerStateWithDescriptor:sd];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    linearClamp_ = [dev newSamplerStateWithDescriptor:sd];

    pipelineCache_.open(dev);
    g_kbRendererLevelEnd = MTL_LevelEnd;

    createBackBuffer();
    Rebind(rt_[0], bbSurface_);
    if (pp.EnableAutoDepthStencil) Rebind(ds_, autoDSSurface_);
    resetViewportToTarget();
    fprintf(stderr, "[mtl] device: %s, back buffer %dx%d, %d sample%s\n", dev.name.UTF8String, bbWidth_, bbHeight_, samples_,
            samples_ > 1 ? "s" : "");
}

MetalDevice::~MetalDevice() {
    finishGPU();
    g_kbRendererLevelEnd = nullptr;
    std::string map;
    pipelineCache_.endMap(MTL_TakeLevelEnd(&map) ? map : std::string("menus"));
    pipelineCache_.finish();
    for (MetalSurface *&s : rt_) Rebind(s, (MetalSurface *)nullptr);
    Rebind(ds_, (MetalSurface *)nullptr);
    for (IDirect3DBaseTexture9 *&t : tex_) Rebind(t, (IDirect3DBaseTexture9 *)nullptr);
    for (Stream &s : streams_) Rebind(s.vb, (MetalVertexBuffer *)nullptr);
    Rebind(ib_, (MetalIndexBuffer *)nullptr);
    Rebind(decl_, (MetalDecl *)nullptr);
    Rebind(vs_, (MetalVertexShader *)nullptr);
    Rebind(ps_, (MetalPixelShader *)nullptr);
    if (swapChain_) swapChain_->Release();
    delete bbSurface_;
    delete autoDSSurface_;
    delete bbColor_;
    delete bbResolve_;
    delete autoDepth_;
    delete win_;
}

// The back buffer (multisampled with D3D's MultiSampleType) and the automatic
// depth-stencil surface; textures we own, which Present draws into the drawable.
void MetalDevice::createBackBuffer() {
    delete bbColor_; delete bbResolve_; delete autoDepth_;
    bbColor_ = bbResolve_ = autoDepth_ = nullptr;
    auto make = [&](MTLPixelFormat pf, int samples) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:bbWidth_
                                                                                     height:bbHeight_ mipmapped:NO];
        td.textureType = samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
        td.sampleCount = samples;
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        return [mtl newTextureWithDescriptor:td];
    };
    bbColor_ = new MetalTexture(this, make(MTLPixelFormatBGRA8Unorm, samples_), D3DFMT_A8R8G8B8, D3DUSAGE_RENDERTARGET);
    if (samples_ > 1)
        bbResolve_ = new MetalTexture(this, make(MTLPixelFormatBGRA8Unorm, 1), D3DFMT_A8R8G8B8, D3DUSAGE_RENDERTARGET);
    autoDepth_ = new MetalTexture(this, make(MTLPixelFormatDepth32Float_Stencil8, samples_), D3DFMT_D24S8,
                                  D3DUSAGE_DEPTHSTENCIL);
    if (!bbSurface_) {
        bbSurface_ = new MetalSurface(this, MetalSurface::S_BACKBUFFER, bbWidth_, bbHeight_, D3DFMT_A8R8G8B8, nullptr,
                                      bbColor_, 0, 0);
        autoDSSurface_ = new MetalSurface(this, MetalSurface::S_DEPTHSTENCIL, bbWidth_, bbHeight_, D3DFMT_D24S8, nullptr,
                                          autoDepth_, 0, 0);
    }
    bbSurface_->setSize(bbWidth_, bbHeight_);
    bbSurface_->setTexture(bbColor_, false);
    autoDSSurface_->setSize(bbWidth_, bbHeight_);
    autoDSSurface_->setTexture(autoDepth_, false);
    bbResolved_ = true;
}

bool MetalDevice::onRenderThread() const { return pthread_equal(pthread_self(), renderThread_) != 0; }


// ---- Command buffers and encoders -------------------------------------------------
void MetalDevice::beginCommandBuffer() {
    if (cmd_) return;
    if (!frameOpen_) {
        // The one place a frame waits for the GPU: kFramesInFlight frames behind.
        dispatch_semaphore_wait(frameSem_, DISPATCH_TIME_FOREVER);
        frameOpen_ = true;
        ++frameCount_;
        stagingUsed_ = 0;
    }
    pool_ = objc_autoreleasePoolPush();
    cmd_ = [queue_ commandBuffer];
    const unsigned long long serial = serial_.load();
    std::atomic<unsigned long long> *completed = &completed_;
    [cmd_ addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        if (cb.status == MTLCommandBufferStatusError)
            MTL_Bug("command buffer failed: %s", cb.error.localizedDescription.UTF8String);
        completed->store(serial, std::memory_order_release);
    }];
}

void MetalDevice::endEncoder() {
    if (enc_) { [enc_ endEncoding]; enc_ = nil; }
    if (blit_) { [blit_ endEncoding]; blit_ = nil; }
}

void MetalDevice::commit(bool present) {
    endEncoder();
    if (!cmd_) return;
    if (present) {
        dispatch_semaphore_t sem = frameSem_;
        [cmd_ addCompletedHandler:^(id<MTLCommandBuffer>) { dispatch_semaphore_signal(sem); }];
        frameOpen_ = false;
    }
    [cmd_ commit];
    cmd_ = nil;
    serial_.fetch_add(1, std::memory_order_acq_rel);
    objc_autoreleasePoolPop(pool_);
    pool_ = nullptr;
}

// Commit what is recorded; an empty command buffer when nothing is, so a fence issued
// since the last one completes.
void MetalDevice::flush() {
    beginCommandBuffer();
    commit(false);
}

void MetalDevice::finishGPU() {
    if (!cmd_ && !frameOpen_ && serial_.load() == completed_.load() + 1) return;
    beginCommandBuffer();
    id<MTLCommandBuffer> cb = cmd_;
    commit(false);
    [cb waitUntilCompleted];
}

void MetalDevice::targetSize(UINT *w, UINT *h) const {
    *w = rt_[0] ? rt_[0]->width() : ds_ ? ds_->width() : 1;
    *h = rt_[0] ? rt_[0]->height() : ds_ ? ds_->height() : 1;
}

id<MTLRenderCommandEncoder> MetalDevice::renderEncoder() {
    if (enc_) return enc_;
    beginCommandBuffer();
    if (blit_) { [blit_ endEncoding]; blit_ = nil; }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    for (int i = 0; i < 4; ++i) {
        MetalSurface *s = rt_[i];
        if (!s) continue;
        MTLRenderPassColorAttachmentDescriptor *a = rp.colorAttachments[i];
        a.texture = s->tex()->texture();
        a.level = s->level();
        a.slice = s->face();
        a.loadAction = (clearFlags_ & D3DCLEAR_TARGET) ? MTLLoadActionClear : MTLLoadActionLoad;
        a.clearColor = MTLClearColorMake(((clearColor_ >> 16) & 0xFF) / 255.0, ((clearColor_ >> 8) & 0xFF) / 255.0,
                                         (clearColor_ & 0xFF) / 255.0, ((clearColor_ >> 24) & 0xFF) / 255.0);
        a.storeAction = MTLStoreActionStore;
        if (s == bbSurface_) bbResolved_ = false;
    }
    if (ds_) {
        id<MTLTexture> t = ds_->tex()->texture();
        rp.depthAttachment.texture = t;
        rp.depthAttachment.level = ds_->level();
        rp.depthAttachment.loadAction = (clearFlags_ & D3DCLEAR_ZBUFFER) ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = clearZ_;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (ds_->tex()->mformat().stencil) {
            rp.stencilAttachment.texture = t;
            rp.stencilAttachment.level = ds_->level();
            rp.stencilAttachment.loadAction = (clearFlags_ & D3DCLEAR_STENCIL) ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.stencilAttachment.clearStencil = clearStencil_ & 0xFF;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
    }
    rp.visibilityResultBuffer = visibility_;
    rp.visibilityResultType = MTLVisibilityResultTypeAccumulate;
    clearFlags_ = 0;
    enc_ = [cmd_ renderCommandEncoderWithDescriptor:rp];
    [enc_ setFrontFacingWinding:MTLWindingClockwise];   // D3D9's front faces are clockwise
    [enc_ setVertexBuffer:defaults_ offset:0 atIndex:MSL_BUF_DEFAULTS];
    if (activeQuery_ >= 0) [enc_ setVisibilityResultMode:MTLVisibilityResultModeCounting offset:activeQuery_ * 8];
    bound_ = Bound();
    dirty_ = D_ALL;
    texDirty_ = ~0u;
    UINT w, h;
    targetSize(&w, &h);
    passWidth_ = (int)w;
    passHeight_ = (int)h;
    return enc_;
}

// A Clear nothing has drawn after: a pass of its own.
void MetalDevice::flushClear() {
    if (clearFlags_ && !enc_) renderEncoder();
}

// ---- Frame / targets --------------------------------------------------------------
// An SM3-class GPU, as the GL layer reports (FillDefaultCaps).
static void FillCaps(D3DCAPS9 *c) {
    *c = D3DCAPS9{};
    c->DeviceType = D3DDEVTYPE_HAL;
    c->MaxTextureWidth = c->MaxTextureHeight = 8192;
    c->MaxAnisotropy = 16;
    c->MaxSimultaneousTextures = 8;
    c->MaxTextureBlendStages = 8;
    c->NumSimultaneousRTs = 4;
    c->VertexShaderVersion = 0xFFFE0300;
    c->PixelShaderVersion = 0xFFFF0300;
    c->MaxVertexShaderConst = 256;
    c->MaxPrimitiveCount = 0x00FFFFFF;
    c->MaxVertexIndex = 0x00FFFFFF;
    c->MaxStreams = 16;
    c->RasterCaps |= 0x02000000;   // D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS
}

HRESULT WINAPI MetalDevice::GetDeviceCaps(D3DCAPS9 *c) {
    FillCaps(c);
    return D3D_OK;
}

void MetalDevice::resetViewportToTarget() {
    UINT w, h;
    targetSize(&w, &h);
    vp_.X = vp_.Y = 0;
    vp_.Width = w;
    vp_.Height = h;
    vp_.MinZ = 0.0f;
    vp_.MaxZ = 1.0f;
    scissor_ = RECT{ 0, 0, (LONG)w, (LONG)h };
    dirty_ |= D_VIEWPORT | D_SCISSOR;
}

HRESULT WINAPI MetalDevice::SetRenderTarget(DWORD index, IDirect3DSurface9 *rt) {
    if (index >= 4) return D3DERR_INVALIDCALL;
    MetalSurface *s = static_cast<MetalSurface *>(rt);
    if (index == 0 && !s) { MTL_Bug("SetRenderTarget(0, NULL)"); return D3DERR_INVALIDCALL; }
    if (s && s->kind() != MetalSurface::S_BACKBUFFER && s->kind() != MetalSurface::S_RENDERTARGET &&
        !(s->kind() == MetalSurface::S_LEVEL && (s->tex()->usage() & D3DUSAGE_RENDERTARGET))) {
        MTL_Bug("SetRenderTarget of a surface that is no render target (kind %d)", (int)s->kind());
        return D3DERR_INVALIDCALL;
    }
    if (index > 0 && s) MTL_Bug("multiple render targets");
    if (rt_[index] == s) {
        if (index == 0) resetViewportToTarget();
        return D3D_OK;
    }
    flushClear();
    endEncoder();
    Rebind(rt_[index], s);
    if (index == 0) resetViewportToTarget();   // as D3D9: the whole new target, scissor too
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::SetDepthStencilSurface(IDirect3DSurface9 *ds) {
    MetalSurface *s = static_cast<MetalSurface *>(ds);
    if (s && s->kind() != MetalSurface::S_DEPTHSTENCIL && !(s->kind() == MetalSurface::S_LEVEL && s->tex()->isDepth())) {
        MTL_Bug("SetDepthStencilSurface of a surface that is no depth-stencil (kind %d)", (int)s->kind());
        return D3DERR_INVALIDCALL;
    }
    if (ds_ == s) return D3D_OK;
    flushClear();
    endEncoder();
    Rebind(ds_, s);
    dirty_ |= D_PIPELINE;
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    UINT w, h;
    targetSize(&w, &h);
    if (!ds_) flags &= ~(DWORD)(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    if (!flags) return D3D_OK;
    // D3D9 clears the viewport, or the rectangles within it.
    RECT r = { (LONG)vp_.X, (LONG)vp_.Y, (LONG)(vp_.X + vp_.Width), (LONG)(vp_.Y + vp_.Height) };
    if (count > 1) { MTL_Bug("Clear of %u rectangles", (unsigned)count); return D3DERR_INVALIDCALL; }
    if (count == 1) {
        if (rects[0].x1 > r.left) r.left = rects[0].x1;
        if (rects[0].y1 > r.top) r.top = rects[0].y1;
        if (rects[0].x2 < r.right) r.right = rects[0].x2;
        if (rects[0].y2 < r.bottom) r.bottom = rects[0].y2;
        if (r.right <= r.left || r.bottom <= r.top) return D3D_OK;
    }
    if (r.left || r.top || (UINT)r.right != w || (UINT)r.bottom != h) {
        clearRect(r, flags, color);
        return D3D_OK;
    }
    // A clear after draws on these targets starts a new pass that clears them as it
    // loads; clears before any draw merge into the next pass's load actions.
    if (enc_) endEncoder();
    clearFlags_ |= flags;
    if (flags & D3DCLEAR_TARGET) clearColor_ = color;
    if (flags & D3DCLEAR_ZBUFFER) clearZ_ = z;
    if (flags & D3DCLEAR_STENCIL) clearStencil_ = stencil;
    return D3D_OK;
}

// A clear of part of the target: a rectangle drawn in the current pass.
void MetalDevice::clearRect(const RECT &r, DWORD flags, D3DCOLOR color) {
    if (flags != D3DCLEAR_TARGET || !rt_[0]) { MTL_Bug("Clear of part of a depth-stencil surface"); return; }
    id<MTLRenderCommandEncoder> enc = renderEncoder();
    id<MTLTexture> ct = rt_[0]->tex()->texture();
    const MTLPixelFormat df = ds_ ? ds_->tex()->texture().pixelFormat : MTLPixelFormatInvalid;
    const unsigned long long key = (unsigned long long)ct.pixelFormat | (unsigned long long)df << 16 |
                                   (unsigned long long)ct.sampleCount << 32;
    __strong id<MTLRenderPipelineState> &p = clearPipes_[key];
    if (!p) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [utilLib_ newFunctionWithName:@"kb_clear_vs"];
        pd.fragmentFunction = [utilLib_ newFunctionWithName:@"kb_clear_ps"];
        pd.colorAttachments[0].pixelFormat = ct.pixelFormat;
        pd.rasterSampleCount = ct.sampleCount;
        if (ds_) {
            pd.depthAttachmentPixelFormat = df;
            if (ds_->tex()->mformat().stencil) pd.stencilAttachmentPixelFormat = df;
        }
        NSError *e = nil;
        p = [mtl newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!p) { MTL_Bug("clear pipeline: %s", e.localizedDescription.UTF8String); return; }
    }
    const unsigned long long dssKey = 8ull << 2;   // no depth test or write, no stencil (depthStencil()'s key)
    __strong id<MTLDepthStencilState> &dss = dss_[dssKey];
    if (!dss) dss = [mtl newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
    [enc setRenderPipelineState:p];
    [enc setDepthStencilState:dss];
    [enc setCullMode:MTLCullModeNone];
    [enc setTriangleFillMode:MTLTriangleFillModeFill];
    [enc setViewport:(MTLViewport){ (double)r.left, (double)r.top, (double)(r.right - r.left), (double)(r.bottom - r.top), 0, 1 }];
    [enc setScissorRect:(MTLScissorRect){ (NSUInteger)r.left, (NSUInteger)r.top, (NSUInteger)(r.right - r.left),
                                          (NSUInteger)(r.bottom - r.top) }];
    const float c[4] = { ((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f,
                         ((color >> 24) & 0xFF) / 255.0f };
    [enc setFragmentBytes:c length:sizeof(c) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    bound_.pipe = nil;
    bound_.dss = nil;
    bound_.psConsts = 0;
    dirty_ |= D_PIPELINE | D_DEPTHSTENCIL | D_RASTER | D_VIEWPORT | D_SCISSOR | D_DRAWVALUES;
}

HRESULT WINAPI MetalDevice::SetViewport(const D3DVIEWPORT9 *vp) {
    UINT w, h;
    targetSize(&w, &h);
    if (vp->X + vp->Width > w || vp->Y + vp->Height > h) {
        MTL_Bug("viewport outside the render target");
        return D3DERR_INVALIDCALL;
    }
    vp_ = *vp;
    dirty_ |= D_VIEWPORT;
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::SetScissorRect(const RECT *r) {
    scissor_ = *r;
    dirty_ |= D_SCISSOR;
    return D3D_OK;
}

// ---- State -------------------------------------------------------------------------
HRESULT WINAPI MetalDevice::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
    const unsigned s = (unsigned)state;
    if (s >= 256 || !KnownRenderState(s)) { MTL_Bug("render state %u = %u", s, (unsigned)value); return D3D_OK; }
    if (rs_[s] == value) return D3D_OK;
    rs_[s] = value;
    switch (s) {
    case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_BLENDOP: case D3DRS_ALPHABLENDENABLE:
    case D3DRS_COLORWRITEENABLE: case D3DRS_ALPHATESTENABLE: case D3DRS_ALPHAFUNC:
    case D3DRS_SEPARATEALPHABLENDENABLE: case D3DRS_SRCBLENDALPHA: case D3DRS_DESTBLENDALPHA: case D3DRS_BLENDOPALPHA:
        dirty_ |= D_PIPELINE; break;
    case D3DRS_ALPHAREF:
        dirty_ |= D_DRAWVALUES; break;
    case D3DRS_ZENABLE: case D3DRS_ZWRITEENABLE: case D3DRS_ZFUNC: case D3DRS_STENCILENABLE: case D3DRS_STENCILFAIL:
    case D3DRS_STENCILZFAIL: case D3DRS_STENCILPASS: case D3DRS_STENCILFUNC: case D3DRS_STENCILMASK:
    case D3DRS_STENCILWRITEMASK: case D3DRS_TWOSIDEDSTENCILMODE: case D3DRS_CCW_STENCILFAIL:
    case D3DRS_CCW_STENCILZFAIL: case D3DRS_CCW_STENCILPASS: case D3DRS_CCW_STENCILFUNC:
        dirty_ |= D_DEPTHSTENCIL; break;
    case D3DRS_STENCILREF:
        dirty_ |= D_STENCILREF; break;
    case D3DRS_CULLMODE: case D3DRS_FILLMODE: case D3DRS_DEPTHBIAS: case D3DRS_SLOPESCALEDEPTHBIAS:
        dirty_ |= D_RASTER; break;
    case D3DRS_SCISSORTESTENABLE:
        dirty_ |= D_SCISSOR; break;
    case D3DRS_ADAPTIVETESS_Y:
        // NVIDIA's transparency AA request (r_aaAlpha): 'ATOC' or 'SSAA' FOURCCs, 0 off.
        if (value && value != MAKEFOURCC('A', 'T', 'O', 'C') && value != MAKEFOURCC('S', 'S', 'A', 'A'))
            MTL_Bug("D3DRS_ADAPTIVETESS_Y %08x", (unsigned)value);
        dirty_ |= D_PIPELINE;
        break;
    }
    return D3D_OK;
}

static int StageIndex(DWORD sampler) {
    if (sampler < 16) return (int)sampler;
    if (sampler >= 257 && sampler <= 260) return 16 + (int)(sampler - 257);
    return -1;
}

HRESULT WINAPI MetalDevice::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
    const int st = StageIndex(sampler);
    const unsigned t = (unsigned)type;
    if (st < 0 || !(t == D3DSAMP_ADDRESSU || t == D3DSAMP_ADDRESSV || t == D3DSAMP_ADDRESSW || t == D3DSAMP_MAGFILTER ||
                    t == D3DSAMP_MINFILTER || t == D3DSAMP_MIPFILTER || t == D3DSAMP_MAXANISOTROPY)) {
        MTL_Bug("sampler state %u = %u (sampler %u)", t, (unsigned)value, (unsigned)sampler);
        return D3D_OK;
    }
    if (samp_[st][t] == value) return D3D_OK;
    samp_[st][t] = value;
    texDirty_ |= 1u << st;
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    // Fixed-function texture stages: every draw has shaders here (the KB_SPEC list), so
    // these settings change nothing.
    (void)stage; (void)type; (void)value;
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::SetTexture(DWORD stage, IDirect3DBaseTexture9 *tex) {
    const int st = StageIndex(stage);
    if (st < 0) { MTL_Bug("texture stage %u", (unsigned)stage); return D3D_OK; }
    if (tex_[st] == tex) return D3D_OK;
    Rebind(tex_[st], tex);
    MetalTexture *m = nullptr;
    if (tex) {
        switch (tex->GetType()) {
        case D3DRTYPE_TEXTURE:       m = &static_cast<MetalTexture2D *>(static_cast<IDirect3DTexture9 *>(tex))->tex(); break;
        case D3DRTYPE_CUBETEXTURE:   m = &static_cast<MetalCubeTexture *>(static_cast<IDirect3DCubeTexture9 *>(tex))->tex(); break;
        case D3DRTYPE_VOLUMETEXTURE: m = &static_cast<MetalVolumeTexture *>(static_cast<IDirect3DVolumeTexture9 *>(tex))->tex(); break;
        default: MTL_Bug("SetTexture of resource type %u", (unsigned)tex->GetType()); break;
        }
    }
    if (m) m->markUsed();
    texm_[st] = m;
    texDirty_ |= 1u << st;
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *vb, UINT offset, UINT stride) {
    if (stream >= 4) { MTL_Bug("vertex stream %u", stream); return D3D_OK; }
    Stream &s = streams_[stream];
    MetalVertexBuffer *v = static_cast<MetalVertexBuffer *>(vb);
    if (s.vb == v && s.offset == offset && s.stride == stride) return D3D_OK;
    if (s.stride != stride) dirty_ |= D_PIPELINE;
    Rebind(s.vb, v);
    s.offset = offset; s.stride = stride;
    dirty_ |= D_STREAMS;
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::SetIndices(IDirect3DIndexBuffer9 *ib) {
    if (ib_ != ib) Rebind(ib_, static_cast<MetalIndexBuffer *>(ib));
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::SetVertexDeclaration(IDirect3DVertexDeclaration9 *decl) {
    MetalDecl *d = static_cast<MetalDecl *>(decl);
    if (decl_ == d) return D3D_OK;
    Rebind(decl_, d);
    dirty_ |= D_PIPELINE | D_STREAMS;
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::SetVertexShader(IDirect3DVertexShader9 *vs) {
    MetalVertexShader *v = static_cast<MetalVertexShader *>(vs);
    if (vs_ == v) return D3D_OK;
    Rebind(vs_, v);
    dirty_ |= D_PIPELINE | D_VSCONST;
    texDirty_ |= 0xFu << 16;
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::SetPixelShader(IDirect3DPixelShader9 *ps) {
    MetalPixelShader *p = static_cast<MetalPixelShader *>(ps);
    if (ps_ == p) return D3D_OK;
    Rebind(ps_, p);
    dirty_ |= D_PIPELINE | D_PSCONST;
    texDirty_ |= 0xFFFFu;
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::SetVertexShaderConstantF(UINT start, const float *data, UINT count) {
    if (start + count > 256) { MTL_Bug("vertex constants up to %u", start + count); return D3DERR_INVALIDCALL; }
    float *dst = vsc_ + start * 4;
    if (memcmp(dst, data, count * 16)) { memcpy(dst, data, count * 16); dirty_ |= D_VSCONST; }
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::SetPixelShaderConstantF(UINT start, const float *data, UINT count) {
    if (start + count > 256) { MTL_Bug("pixel constants up to %u", start + count); return D3DERR_INVALIDCALL; }
    float *dst = psc_ + start * 4;
    if (memcmp(dst, data, count * 16)) { memcpy(dst, data, count * 16); dirty_ |= D_PSCONST; }
    return D3D_OK;
}

// ---- State caches ------------------------------------------------------------------
id<MTLRenderPipelineState> MetalDevice::pipeline() {
    MetalShader &vs = vs_->s, &ps = ps_->s;
    PipeKey k;
    memset(&k, 0, sizeof(k));
    k.vs = vs.uid;
    k.ps = ps.uid;
    k.decl = decl_->uid;
    MetalSurface *rt = rt_[0];
    k.colorFmt = rt ? (unsigned short)rt->tex()->texture().pixelFormat : 0;
    k.depthFmt = ds_ ? (unsigned short)ds_->tex()->texture().pixelFormat : 0;
    k.samples = (unsigned char)(rt ? rt->tex()->texture().sampleCount : ds_ ? ds_->tex()->texture().sampleCount : 1);
    for (int i = 0; i < 4; ++i)
        if (decl_->streamMask & (1u << i)) k.strides[i] = (unsigned short)streams_[i].stride;
    // The alpha test is a specialisation of the pixel function; ALWAYS is no test.
    const DWORD af = rs_[D3DRS_ALPHATESTENABLE] ? rs_[D3DRS_ALPHAFUNC] : 8;
    k.alphaFunc = (unsigned char)(af == 8 ? 0 : af);
    // Transparency AA (r_aaAlpha: 'ATOC' or 'SSAA') on alpha-tested draws into a
    // multisampled target: the pixel shader runs per sample, so the test cuts each
    // sample, as the GL layer's sample shading does. (Alpha to coverage, NVIDIA's
    // 'ATOC', shows the colour these textures keep under low alpha: orange leaf edges.)
    const DWORD taa = rs_[D3DRS_ADAPTIVETESS_Y];
    if (k.alphaFunc && k.samples > 1 && (taa == MAKEFOURCC('A', 'T', 'O', 'C') || taa == MAKEFOURCC('S', 'S', 'A', 'A')))
        k.aa = 1;
    k.blendOn = rs_[D3DRS_ALPHABLENDENABLE] ? 1 : 0;
    if (k.blendOn) {
        k.src = (unsigned char)rs_[D3DRS_SRCBLEND];
        k.dst = (unsigned char)rs_[D3DRS_DESTBLEND];
        k.op = (unsigned char)rs_[D3DRS_BLENDOP];
        const bool separate = rs_[D3DRS_SEPARATEALPHABLENDENABLE] && !GLCompat();
        k.srcA = separate ? (unsigned char)rs_[D3DRS_SRCBLENDALPHA] : k.src;
        k.dstA = separate ? (unsigned char)rs_[D3DRS_DESTBLENDALPHA] : k.dst;
        k.opA = separate ? (unsigned char)rs_[D3DRS_BLENDOPALPHA] : k.op;
    }
    k.mask = (unsigned char)(rs_[D3DRS_COLORWRITEENABLE] & 0xF);
    auto it = pipes_.find(k);
    if (it != pipes_.end()) {
        PipeEntry &pe = it->second;
        if (pe.state && pe.epoch != pipelineCache_.epoch) {
            pe.epoch = pipelineCache_.epoch;
            pipelineCache_.use(pe.desc, pe.fd, pe.lib);
        }
        return pe.state;
    }

    // Built at the first draw that needs it, waiting for its shaders (KB_FPS counts it).
    const double t0 = Now(), w0 = compiler_.waited;
    ++statsPipes_;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs.function(0, 0);
    pd.fragmentFunction = ps.function(k.alphaFunc, k.aa ? MetalShader::VARIANT_PER_SAMPLE : 0);
    const double t1 = Now();
    statsBatchWait_ += compiler_.waited - w0;
    statsFunctions_ += t1 - t0 - (compiler_.waited - w0);
    if (!pd.vertexFunction || !pd.fragmentFunction) {
        pipes_[k] = PipeEntry{};
        statsBuild_ += Now() - t0;
        frameBuild_ += Now() - t0;
        return nil;
    }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (const MetalDecl::Attr &a : decl_->attrs) {
        if (!(vs.attribMask & (1u << a.index))) continue;
        vd.attributes[a.index].format = (MTLVertexFormat)a.format;
        vd.attributes[a.index].offset = a.offset;
        vd.attributes[a.index].bufferIndex = a.stream;
    }
    for (int i = 0; i < 4; ++i)
        if (decl_->streamMask & (1u << i)) {
            if (!k.strides[i] || (k.strides[i] & 3)) MTL_Bug("vertex stride %u", (unsigned)k.strides[i]);
            vd.layouts[i].stride = k.strides[i] ? k.strides[i] : 4;
        }
    // Inputs the declaration lacks read D3D's defaults through a constant attribute:
    // (0,0,0,1), or white for COLOR0 as the GL layer gives it.
    const unsigned missing = vs.attribMask & ~decl_->attribMask;
    if (missing) {
        for (int a = 0; a < MSL_MAX_ATTRIBS; ++a)
            if (missing & (1u << a)) {
                vd.attributes[a].format = MTLVertexFormatFloat4;
                vd.attributes[a].offset = a == 5 ? 16 : 0;
                vd.attributes[a].bufferIndex = MSL_BUF_DEFAULTS;
            }
        vd.layouts[MSL_BUF_DEFAULTS].stepFunction = MTLVertexStepFunctionConstant;
        vd.layouts[MSL_BUF_DEFAULTS].stepRate = 0;
        vd.layouts[MSL_BUF_DEFAULTS].stride = 32;
    }
    pd.vertexDescriptor = vd;
    pd.rasterSampleCount = k.samples;
    if (rt) {
        MTLRenderPipelineColorAttachmentDescriptor *c = pd.colorAttachments[0];
        c.pixelFormat = (MTLPixelFormat)k.colorFmt;
        c.blendingEnabled = k.blendOn;
        if (k.blendOn) {
            c.sourceRGBBlendFactor = BlendFactor(k.src);
            c.destinationRGBBlendFactor = BlendFactor(k.dst);
            c.rgbBlendOperation = BlendOp(k.op);
            c.sourceAlphaBlendFactor = BlendFactor(k.srcA);
            c.destinationAlphaBlendFactor = BlendFactor(k.dstA);
            c.alphaBlendOperation = BlendOp(k.opA);
        }
        // D3D's mask bits are R=1 G=2 B=4 A=8, Metal's the other way round.
        c.writeMask = ((k.mask & 1) ? MTLColorWriteMaskRed : 0) | ((k.mask & 2) ? MTLColorWriteMaskGreen : 0) |
                      ((k.mask & 4) ? MTLColorWriteMaskBlue : 0) | ((k.mask & 8) ? MTLColorWriteMaskAlpha : 0);
    }
    if (ds_) {
        pd.depthAttachmentPixelFormat = (MTLPixelFormat)k.depthFmt;
        if (ds_->tex()->mformat().stencil) pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k.depthFmt;
    }
    NSError *e = nil;
    const unsigned hits = pipelineCache_.pipelineHits;
    id<MTLRenderPipelineState> p = pipelineCache_.pipeline(pd, &e);
    statsCacheHits_ += pipelineCache_.pipelineHits - hits;
    if (!p) MTL_Bug("pipeline (vs %08x, ps %08x): %s", vs.hash, ps.hash, e.localizedDescription.UTF8String);
    id<MTLLibrary> psLib = nil;
    MTLFunctionDescriptor *psFd = ps.descriptor(k.alphaFunc, k.aa ? MetalShader::VARIANT_PER_SAMPLE : 0, &psLib);
    if (p) pipelineCache_.use(pd, psFd, psLib);
    pipes_[k] = PipeEntry{ p, pd, psFd, psLib, pipelineCache_.epoch };
    const double t2 = Now();
    statsBuild_ += t2 - t0;
    frameBuild_ += t2 - t0;
    ++framePipes_;
    if (t2 - t0 > statsBuildMax_) statsBuildMax_ = t2 - t0;
    return p;
}

id<MTLDepthStencilState> MetalDevice::depthStencil() {
    const bool z = rs_[D3DRS_ZENABLE] != 0 && ds_;
    const bool st = rs_[D3DRS_STENCILENABLE] != 0 && ds_ && ds_->tex()->mformat().stencil;
    const bool two = st && rs_[D3DRS_TWOSIDEDSTENCILMODE];
    unsigned long long key = (z ? 1 : 0) | (z && rs_[D3DRS_ZWRITEENABLE] ? 2 : 0) | ((unsigned long long)(z ? rs_[D3DRS_ZFUNC] : 8) << 2);
    if (st) {
        key |= 1ull << 6 | (two ? 1ull << 7 : 0);
        key |= (unsigned long long)(rs_[D3DRS_STENCILFUNC] & 0xF) << 8 | (unsigned long long)(rs_[D3DRS_STENCILFAIL] & 0xF) << 12 |
               (unsigned long long)(rs_[D3DRS_STENCILZFAIL] & 0xF) << 16 | (unsigned long long)(rs_[D3DRS_STENCILPASS] & 0xF) << 20;
        if (two)
            key |= (unsigned long long)(rs_[D3DRS_CCW_STENCILFUNC] & 0xF) << 24 | (unsigned long long)(rs_[D3DRS_CCW_STENCILFAIL] & 0xF) << 28 |
                   (unsigned long long)(rs_[D3DRS_CCW_STENCILZFAIL] & 0xF) << 32 | (unsigned long long)(rs_[D3DRS_CCW_STENCILPASS] & 0xF) << 36;
        key |= (unsigned long long)(rs_[D3DRS_STENCILMASK] & 0xFF) << 40 | (unsigned long long)(rs_[D3DRS_STENCILWRITEMASK] & 0xFF) << 48;
    }
    auto it = dss_.find(key);
    if (it != dss_.end()) return it->second;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = z ? CompareFunc(rs_[D3DRS_ZFUNC]) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = z && rs_[D3DRS_ZWRITEENABLE];
    if (st) {
        auto side = [&](DWORD func, DWORD fail, DWORD zfail, DWORD pass) {
            MTLStencilDescriptor *s = [MTLStencilDescriptor new];
            s.stencilCompareFunction = CompareFunc(func);
            s.stencilFailureOperation = StencilOp(fail);
            s.depthFailureOperation = StencilOp(zfail);
            s.depthStencilPassOperation = StencilOp(pass);
            s.readMask = rs_[D3DRS_STENCILMASK] & 0xFF;
            s.writeMask = rs_[D3DRS_STENCILWRITEMASK] & 0xFF;
            return s;
        };
        d.frontFaceStencil = side(rs_[D3DRS_STENCILFUNC], rs_[D3DRS_STENCILFAIL], rs_[D3DRS_STENCILZFAIL], rs_[D3DRS_STENCILPASS]);
        d.backFaceStencil = two ? side(rs_[D3DRS_CCW_STENCILFUNC], rs_[D3DRS_CCW_STENCILFAIL], rs_[D3DRS_CCW_STENCILZFAIL],
                                       rs_[D3DRS_CCW_STENCILPASS])
                                : d.frontFaceStencil;
    }
    id<MTLDepthStencilState> s = [mtl newDepthStencilStateWithDescriptor:d];
    dss_[key] = s;
    return s;
}

id<MTLSamplerState> MetalDevice::sampler(int st) {
    const DWORD *s = samp_[st];
    const bool depth = texm_[st] && texm_[st]->isDepth();
    // A shadow map is compared by the sampler (sample_compare: lit where z <= depth);
    // in GL compatibility it reads raw depth, unfiltered.
    const bool raw = depth && GLCompat();
    const DWORD minF = raw ? 1 : s[D3DSAMP_MINFILTER], magF = raw ? 1 : s[D3DSAMP_MAGFILTER];
    const DWORD mipF = depth || GLCompat() ? 0 : s[D3DSAMP_MIPFILTER];
    const DWORD aniso = minF == 3 ? s[D3DSAMP_MAXANISOTROPY] : 1;
    const bool compare = depth && !raw;
    unsigned long long key = (unsigned long long)(minF & 0xF) | (unsigned long long)(magF & 0xF) << 4 |
                             (unsigned long long)(mipF & 0xF) << 8 | (unsigned long long)(s[D3DSAMP_ADDRESSU] & 0xF) << 12 |
                             (unsigned long long)(s[D3DSAMP_ADDRESSV] & 0xF) << 16 | (unsigned long long)(s[D3DSAMP_ADDRESSW] & 0xF) << 20 |
                             (unsigned long long)(aniso & 0xFF) << 24 | (unsigned long long)compare << 32;
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    auto filter = [](DWORD f) {
        if (f > 3) MTL_Bug("texture filter %u", (unsigned)f);
        return f >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    };
    d.minFilter = filter(minF);
    d.magFilter = filter(magF);
    d.mipFilter = mipF == 0 ? MTLSamplerMipFilterNotMipmapped : mipF == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    if (mipF > 2) MTL_Bug("mip filter %u", (unsigned)mipF);
    d.maxAnisotropy = aniso < 1 ? 1 : aniso > 16 ? 16 : aniso;
    d.sAddressMode = AddressMode(s[D3DSAMP_ADDRESSU]);
    d.tAddressMode = AddressMode(s[D3DSAMP_ADDRESSV]);
    d.rAddressMode = AddressMode(s[D3DSAMP_ADDRESSW]);
    if (compare) d.compareFunction = MTLCompareFunctionLessEqual;
    id<MTLSamplerState> ss = [mtl newSamplerStateWithDescriptor:d];
    samplers_[key] = ss;
    return ss;
}

// D3D9 samples an empty stage as black.
id<MTLTexture> MetalDevice::dummyTexture(int dim) {
    if (!dummy_[dim]) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.textureType = dim == 3 ? MTLTextureTypeCube : dim == 4 ? MTLTextureType3D : MTLTextureType2D;
        td.pixelFormat = MTLPixelFormatRGBA8Unorm;
        td.width = td.height = 1;
        td.storageMode = MTLStorageModeShared;
        dummy_[dim] = [mtl newTextureWithDescriptor:td];
        static const unsigned char black[4] = { 0, 0, 0, 255 };
        for (int f = 0; f < (dim == 3 ? 6 : 1); ++f)
            [dummy_[dim] replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:f withBytes:black bytesPerRow:4
                         bytesPerImage:4];
    }
    return dummy_[dim];
}

// ---- Draws --------------------------------------------------------------------------
// The shadow maps (D3D9 hardware shadow-map semantics): the engine binds a depth texture
// to each sampler a pixel shader names shadowmapSampler* (a depth2d, compared), and
// nowhere else (the spec), filtered bilinearly as its shadow map sampler asks.
bool MetalDevice::shadowMapsBound() {
    bool ok = true;
    for (unsigned s = ps_->s.samplerMask & 0xFFFFu; s; s &= s - 1) {
        const int st = __builtin_ctz(s);
        const bool depth = texm_[st] && texm_[st]->isDepth(), shadow = (ps_->s.shadowMask >> st) & 1;
        if (depth != shadow) {
            MTL_Bug("ps %08x s%d: %s bound to a %s sampler", ps_->s.hash, st, depth ? "a depth texture" : texm_[st] ? "a texture" : "no texture",
                    shadow ? "shadow map" : "colour");
            if (shadow && !GLCompat()) ok = false;   // a depth2d can't take it
        }
        if (depth && (samp_[st][D3DSAMP_MINFILTER] != D3DTEXF_LINEAR || samp_[st][D3DSAMP_MAGFILTER] != D3DTEXF_LINEAR))
            MTL_Bug("shadow map filter %u/%u", (unsigned)samp_[st][D3DSAMP_MINFILTER], (unsigned)samp_[st][D3DSAMP_MAGFILTER]);
    }
    return ok;
}

// The one place D3D9 state becomes Metal commands.
bool MetalDevice::prepareDraw(D3DPRIMITIVETYPE type) {
    if (!vs_ || !ps_) { MTL_Bug("draw without a vertex and a pixel shader"); return false; }
    if (!vs_->s.ok() || !ps_->s.ok() || !decl_) return false;   // reported when created
    if (type == D3DPT_TRIANGLEFAN) { MTL_Bug("triangle fan"); return false; }
    if (!shadowMapsBound()) return false;
    id<MTLRenderCommandEncoder> enc = renderEncoder();
    if (dirty_ & D_PIPELINE) {
        id<MTLRenderPipelineState> p = pipeline();
        if (!p) return false;
        if (p != bound_.pipe) { [enc setRenderPipelineState:p]; bound_.pipe = p; }
    }
    if (dirty_ & (D_DEPTHSTENCIL | D_PIPELINE)) {
        id<MTLDepthStencilState> d = depthStencil();
        if (d != bound_.dss) { [enc setDepthStencilState:d]; bound_.dss = d; }
    }
    if (dirty_ & D_STENCILREF) [enc setStencilReferenceValue:rs_[D3DRS_STENCILREF] & 0xFF];
    if (dirty_ & D_RASTER) {
        const DWORD cull = rs_[D3DRS_CULLMODE];
        [enc setCullMode:cull == 2 ? MTLCullModeFront : cull == 3 ? MTLCullModeBack : MTLCullModeNone];
        [enc setTriangleFillMode:rs_[D3DRS_FILLMODE] == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        float bias, slope;
        memcpy(&bias, &rs_[D3DRS_DEPTHBIAS], 4);
        memcpy(&slope, &rs_[D3DRS_SLOPESCALEDEPTHBIAS], 4);
        [enc setDepthBias:bias * 16777216.0f slopeScale:slope clamp:0.0f];
    }
    if (dirty_ & D_VIEWPORT) {
        [enc setViewport:(MTLViewport){ (double)vp_.X, (double)vp_.Y, (double)vp_.Width, (double)vp_.Height, vp_.MinZ, vp_.MaxZ }];
        const float fixup[4] = { 1.0f / (float)(vp_.Width ? vp_.Width : 1), -1.0f / (float)(vp_.Height ? vp_.Height : 1), 0, 0 };
        [enc setVertexBytes:fixup length:sizeof(fixup) atIndex:MSL_BUF_DRAW];
    }
    if (dirty_ & D_DRAWVALUES) {
        const float vals[4] = { (rs_[D3DRS_ALPHAREF] & 0xFF) / 255.0f, 0, 0, 0 };
        [enc setFragmentBytes:vals length:sizeof(vals) atIndex:MSL_BUF_DRAW];
    }
    if (dirty_ & D_SCISSOR) {
        RECT r = rs_[D3DRS_SCISSORTESTENABLE] ? scissor_ : RECT{ 0, 0, passWidth_, passHeight_ };
        if (r.left < 0) r.left = 0;
        if (r.top < 0) r.top = 0;
        if (r.right > passWidth_) r.right = passWidth_;
        if (r.bottom > passHeight_) r.bottom = passHeight_;
        if (r.right <= r.left || r.bottom <= r.top) return false;   // nothing passes the scissor
        [enc setScissorRect:(MTLScissorRect){ (NSUInteger)r.left, (NSUInteger)r.top, (NSUInteger)(r.right - r.left),
                                              (NSUInteger)(r.bottom - r.top) }];
    }
    // Vertex buffers: again when the binding or the buffer's backing (a DISCARD) changed.
    for (unsigned m = decl_->streamMask; m; m &= m - 1) {
        const int i = __builtin_ctz(m);
        MetalVertexBuffer *vb = streams_[i].vb;
        if (!vb) { MTL_Bug("draw without vertex stream %d", i); return false; }
        const NSUInteger off = streams_[i].offset;
        const unsigned gen = vb->buffer().generation();
        if (vb != bound_.vbObj[i] || gen != bound_.vbGen[i]) {
            id<MTLBuffer> b = vb->buffer().current();
            bound_.vbObj[i] = vb;
            bound_.vbGen[i] = gen;
            if (b != bound_.vb[i]) { [enc setVertexBuffer:b offset:off atIndex:i]; bound_.vb[i] = b; bound_.vbOffset[i] = off; continue; }
        }
        if (off != bound_.vbOffset[i]) { [enc setVertexBufferOffset:off atIndex:i]; bound_.vbOffset[i] = off; }
    }
    const int vsc = vs_->s.constCount, psc = ps_->s.constCount;
    if (vsc > 0 && ((dirty_ & D_VSCONST) || vsc > bound_.vsConsts)) {
        [enc setVertexBytes:vsc_ length:vsc * 16 atIndex:MSL_BUF_CONSTANTS];
        bound_.vsConsts = vsc;
    }
    if (psc > 0 && ((dirty_ & D_PSCONST) || psc > bound_.psConsts)) {
        [enc setFragmentBytes:psc_ length:psc * 16 atIndex:MSL_BUF_CONSTANTS];
        bound_.psConsts = psc;
    }
    // Textures and samplers of the stages the shaders sample.
    const unsigned want = (ps_->s.samplerMask & 0xFFFFu) | ((vs_->s.samplerMask & 0xFu) << 16);
    for (unsigned m = want & texDirty_; m; m &= m - 1) {
        const int st = __builtin_ctz(m);
        const bool vertex = st >= 16;
        const int slot = vertex ? st - 16 : st;
        const int dim = vertex ? vs_->s.samplerDim[slot] : ps_->s.samplerDim[slot];
        MetalTexture *t = texm_[st];
        id<MTLTexture> tex;
        if (!t) tex = dummyTexture(dim);
        else {
            const int have = t->kind() == MetalTexture::TEX_CUBE ? 3 : t->kind() == MetalTexture::TEX_VOLUME ? 4 : 2;
            if (have != dim) {
                MTL_Bug("sampler %d declared %d-type, bound %d-type texture", st, dim, have);
                tex = dummyTexture(dim);
            } else {
                tex = t->sampleView();
            }
        }
        id<MTLSamplerState> smp = sampler(st);
        if (tex != bound_.tex[st]) {
            if (vertex) [enc setVertexTexture:tex atIndex:slot]; else [enc setFragmentTexture:tex atIndex:slot];
            bound_.tex[st] = tex;
        }
        if (smp != bound_.smp[st]) {
            if (vertex) [enc setVertexSamplerState:smp atIndex:slot]; else [enc setFragmentSamplerState:smp atIndex:slot];
            bound_.smp[st] = smp;
        }
    }
    texDirty_ &= ~want;
    dirty_ = 0;
    return true;
}

static MTLPrimitiveType PrimType(D3DPRIMITIVETYPE t, UINT prims, NSUInteger *count) {
    switch (t) {
    case D3DPT_POINTLIST:     *count = prims;     return MTLPrimitiveTypePoint;
    case D3DPT_LINELIST:      *count = prims * 2; return MTLPrimitiveTypeLine;
    case D3DPT_LINESTRIP:     *count = prims + 1; return MTLPrimitiveTypeLineStrip;
    case D3DPT_TRIANGLESTRIP: *count = prims + 2; return MTLPrimitiveTypeTriangleStrip;
    default:                  *count = prims * 3; return MTLPrimitiveTypeTriangle;
    }
}

HRESULT WINAPI MetalDevice::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) {
    ++g_kbDraws;
    if (!count || !prepareDraw(type)) return D3D_OK;
    NSUInteger n;
    const MTLPrimitiveType pt = PrimType(type, count, &n);
    [enc_ drawPrimitives:pt vertexStart:start vertexCount:n];
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base, UINT, UINT, UINT startIndex, UINT count) {
    ++g_kbDraws;
    if (!ib_) { MTL_Bug("indexed draw without indices"); return D3D_OK; }
    if (!count || !prepareDraw(type)) return D3D_OK;
    NSUInteger n;
    const MTLPrimitiveType pt = PrimType(type, count, &n);
    const bool i16 = ib_->format() == D3DFMT_INDEX16;
    const unsigned gen = ib_->buffer().generation();
    if (ib_ != bound_.ibObj || gen != bound_.ibGen) {
        bound_.ib = ib_->buffer().current();
        bound_.ibObj = ib_;
        bound_.ibGen = gen;
    }
    [enc_ drawIndexedPrimitives:pt indexCount:n indexType:i16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                    indexBuffer:bound_.ib indexBufferOffset:startIndex * (i16 ? 2 : 4) instanceCount:1
                     baseVertex:base baseInstance:0];
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT, const void *, UINT) {
    MTL_Bug("DrawPrimitiveUP");
    return D3D_OK;
}

// ---- Occlusion queries ---------------------------------------------------------------
// A slot the GPU finished with long ago (kQuerySlots queries back), zeroed: the
// counts accumulate over the render passes between BEGIN and END.
unsigned long long MetalDevice::allocQuerySlot() {
    const unsigned long long id = ++queryCount_;
    ((unsigned long long *)[visibility_ contents])[id % kQuerySlots] = 0;
    return id;
}

void MetalDevice::beginQuery(unsigned long long id) {
    activeQuery_ = (int)(id % kQuerySlots);
    if (enc_) [enc_ setVisibilityResultMode:MTLVisibilityResultModeCounting offset:activeQuery_ * 8];
}

void MetalDevice::endQuery() {
    activeQuery_ = -1;
    if (enc_) [enc_ setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
}

bool MetalDevice::queryResult(unsigned long long id, DWORD *samples) {
    if (queryCount_ - id >= kQuerySlots) {
        MTL_Bug("occlusion query read %d queries after it was issued", kQuerySlots);
        *samples = 0;
        return true;
    }
    const unsigned long long v = ((const unsigned long long *)[visibility_ contents])[id % kQuerySlots];
    *samples = v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (DWORD)v;
    return true;
}

// ---- Copies ---------------------------------------------------------------------------
void MetalDevice::copyToTexture(id<MTLTexture> dst, UINT slice, UINT level, MTLOrigin origin, MTLSize size,
                                const void *bytes, UINT pitch, UINT imageBytes) {
    beginCommandBuffer();
    const NSUInteger len = (NSUInteger)imageBytes * size.depth;
    // The frame's staging buffer (grown as needed: a replaced one lives on in the command
    // buffers that read it); a level over 4 MB (streamed texture mips) gets its own.
    static const NSUInteger kStagingMax = 4u << 20;
    id<MTLBuffer> staging;
    NSUInteger offset = 0;
    if (len > kStagingMax) {
        staging = [mtl newBufferWithBytes:bytes length:len options:MTLResourceStorageModeShared];
    } else {
        __strong id<MTLBuffer> &sb = staging_[frameCount_ % kFramesInFlight];
        offset = (stagingUsed_ + 255) & ~(NSUInteger)255;
        if (!sb || offset + len > sb.length) {
            sb = [mtl newBufferWithLength:sb ? sb.length * 2 : kStagingMax options:MTLResourceStorageModeShared];
            offset = 0;
        }
        staging = sb;
        memcpy((unsigned char *)sb.contents + offset, bytes, len);
        stagingUsed_ = offset + len;
    }
    if (enc_) { [enc_ endEncoding]; enc_ = nil; }
    if (!blit_) blit_ = [cmd_ blitCommandEncoder];
    [blit_ copyFromBuffer:staging sourceOffset:offset sourceBytesPerRow:pitch sourceBytesPerImage:imageBytes
               sourceSize:size toTexture:dst destinationSlice:slice destinationLevel:level destinationOrigin:origin];
}

// A textured rectangle from src into dst: StretchRect with scaling or a format change,
// and Present.
void MetalDevice::fullscreenPass(id<MTLTexture> src, UINT srcSlice, UINT srcLevel, const RECT &sr, id<MTLTexture> dst,
                                 UINT dstSlice, UINT dstLevel, const RECT &dr, bool linear, bool clearOutside) {
    (void)srcSlice; (void)srcLevel;
    flushClear();
    endEncoder();
    beginCommandBuffer();
    encodeBlit(cmd_, src, sr, dst, dstSlice, dstLevel, dr, linear, clearOutside);
}

// The textured rectangle as a render pass of its own in `cb`.
void MetalDevice::encodeBlit(id<MTLCommandBuffer> cb, id<MTLTexture> src, const RECT &sr, id<MTLTexture> dst,
                             UINT dstSlice, UINT dstLevel, const RECT &dr, bool linear, bool clearOutside) {
    const MTLPixelFormat pf = dst.pixelFormat;
    int slot = 0;
    while (slot < 8 && blitPipes_[slot] && blitFormats_[slot] != pf) ++slot;
    if (slot == 8) { MTL_Bug("too many StretchRect target formats"); return; }
    if (!blitPipes_[slot]) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [utilLib_ newFunctionWithName:@"kb_blit_vs"];
        pd.fragmentFunction = [utilLib_ newFunctionWithName:@"kb_blit_ps"];
        pd.colorAttachments[0].pixelFormat = pf;
        NSError *e = nil;
        blitPipes_[slot] = [mtl newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!blitPipes_[slot]) { MTL_Bug("blit pipeline: %s", e.localizedDescription.UTF8String); return; }
        blitFormats_[slot] = pf;
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].slice = dstSlice;
    rp.colorAttachments[0].level = dstLevel;
    rp.colorAttachments[0].loadAction = clearOutside ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:blitPipes_[slot]];
    [e setViewport:(MTLViewport){ (double)dr.left, (double)dr.top, (double)(dr.right - dr.left), (double)(dr.bottom - dr.top), 0, 1 }];
    const float w = (float)src.width, h = (float)src.height;
    const float r[4] = { sr.left / w, sr.top / h, sr.right / w, sr.bottom / h };
    [e setVertexBytes:r length:sizeof(r) atIndex:0];
    [e setFragmentTexture:src atIndex:0];
    [e setFragmentSamplerState:linear ? linearClamp_ : pointClamp_ atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

// The multisampled back buffer's contents in bbResolve_.
void MetalDevice::resolveBackBuffer() {
    if (samples_ <= 1 || bbResolved_) return;
    flushClear();
    endEncoder();
    beginCommandBuffer();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = bbColor_->texture();
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    rp.colorAttachments[0].resolveTexture = bbResolve_->texture();
    [[cmd_ renderCommandEncoderWithDescriptor:rp] endEncoding];
    bbResolved_ = true;
}

HRESULT WINAPI MetalDevice::StretchRect(IDirect3DSurface9 *srcS, const RECT *srcRect, IDirect3DSurface9 *dstS,
                                        const RECT *dstRect, D3DTEXTUREFILTERTYPE filter) {
    ++g_kbBlits;
    MetalSurface *src = static_cast<MetalSurface *>(srcS), *dst = static_cast<MetalSurface *>(dstS);
    if (!src || !dst || !src->tex() || !dst->tex() || src->kind() == MetalSurface::S_SYSMEM ||
        dst->kind() == MetalSurface::S_SYSMEM || src->tex()->isDepth() || dst->tex()->isDepth()) {
        MTL_Bug("StretchRect between these surfaces (kinds %d -> %d)", src ? (int)src->kind() : -1, dst ? (int)dst->kind() : -1);
        return D3DERR_INVALIDCALL;
    }
    const RECT sr = srcRect ? *srcRect : RECT{ 0, 0, (LONG)src->width(), (LONG)src->height() };
    const RECT dr = dstRect ? *dstRect : RECT{ 0, 0, (LONG)dst->width(), (LONG)dst->height() };
    flushClear();
    endEncoder();
    beginCommandBuffer();
    id<MTLTexture> st = src->tex()->texture(), dt = dst->tex()->texture();
    const bool sameSize = sr.right - sr.left == dr.right - dr.left && sr.bottom - sr.top == dr.bottom - dr.top;
    if (st.sampleCount > 1) {
        // The multisampled back buffer: resolve straight into a whole same-format target
        // of its size (the scene resolves), else through bbResolve_.
        if (sameSize && !sr.left && !sr.top && !dr.left && !dr.top && dt.pixelFormat == st.pixelFormat &&
            (UINT)(sr.right) == dst->width() && (UINT)sr.bottom == dst->height() && dt.width == st.width &&
            dt.height == st.height) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = st;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
            rp.colorAttachments[0].resolveTexture = dt;
            rp.colorAttachments[0].resolveLevel = dst->level();
            rp.colorAttachments[0].resolveSlice = dst->face();
            [[cmd_ renderCommandEncoderWithDescriptor:rp] endEncoding];
            return D3D_OK;
        }
        resolveBackBuffer();
        st = bbResolve_->texture();
    }
    if (sameSize && st.pixelFormat == dt.pixelFormat && dt.sampleCount == 1) {
        blit_ = [cmd_ blitCommandEncoder];
        [blit_ copyFromTexture:st sourceSlice:src->face() sourceLevel:src->level()
                  sourceOrigin:MTLOriginMake(sr.left, sr.top, 0)
                    sourceSize:MTLSizeMake(sr.right - sr.left, sr.bottom - sr.top, 1)
                     toTexture:dt destinationSlice:dst->face() destinationLevel:dst->level()
             destinationOrigin:MTLOriginMake(dr.left, dr.top, 0)];
        [blit_ endEncoding];
        blit_ = nil;
        return D3D_OK;
    }
    if (dt.sampleCount > 1) { MTL_Bug("scaled StretchRect into a multisampled surface"); return D3DERR_INVALIDCALL; }
    if (src->level() || src->face()) MTL_Bug("scaled StretchRect from a texture level or face");
    fullscreenPass(st, src->face(), src->level(), sr, dt, dst->face(), dst->level(), dr, filter != D3DTEXF_POINT &&
                   filter != D3DTEXF_NONE, false);
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::UpdateSurface(IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *) {
    MTL_Bug("UpdateSurface");
    return D3DERR_INVALIDCALL;
}

// ---- Readbacks ----------------------------------------------------------------------------
bool MetalDevice::readTexture(id<MTLTexture> tex, UINT slice, UINT level, UINT w, UINT h, std::vector<unsigned char> &out,
                              UINT *pitchOut) {
    const MTLPixelFormat pf = tex.pixelFormat;
    const UINT bpp = pf == MTLPixelFormatRGBA16Float || pf == MTLPixelFormatRGBA16Unorm ? 8 : 4;
    const UINT pitch = w * bpp;
    flushClear();
    endEncoder();
    beginCommandBuffer();
    id<MTLBuffer> buf = [mtl newBufferWithLength:(NSUInteger)pitch * h options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> b = [cmd_ blitCommandEncoder];
    [b copyFromTexture:tex sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:pitch
  destinationBytesPerImage:(NSUInteger)pitch * h];
    [b endEncoding];
    id<MTLCommandBuffer> cb = cmd_;
    commit(false);
    [cb waitUntilCompleted];
    out.assign((const unsigned char *)[buf contents], (const unsigned char *)[buf contents] + (size_t)pitch * h);
    *pitchOut = pitch;
    return true;
}

HRESULT WINAPI MetalDevice::GetRenderTargetData(IDirect3DSurface9 *rtS, IDirect3DSurface9 *dstS) {
    MetalSurface *rt = static_cast<MetalSurface *>(rtS), *dst = static_cast<MetalSurface *>(dstS);
    if (!rt || !dst || dst->kind() != MetalSurface::S_SYSMEM || !rt->tex()) {
        MTL_Bug("GetRenderTargetData between these surfaces");
        return D3DERR_INVALIDCALL;
    }
    id<MTLTexture> t = rt->tex()->texture();
    if (t.sampleCount > 1) { resolveBackBuffer(); t = bbResolve_->texture(); }
    const UINT w = rt->width() < dst->width() ? rt->width() : dst->width();
    const UINT h = rt->height() < dst->height() ? rt->height() : dst->height();
    std::vector<unsigned char> px;
    UINT pitch;
    readTexture(t, rt->face(), rt->level(), w, h, px, &pitch);
    const MetalFormat m = MTL_Format(dst->format(), false);
    const UINT dpitch = dst->width() * (m.bytes ? m.bytes : 4);
    if (m.bytes * w != pitch) MTL_Bug("GetRenderTargetData format change");
    for (UINT y = 0; y < h; ++y) memcpy(dst->sysmem().data() + (size_t)y * dpitch, px.data() + (size_t)y * pitch, pitch < dpitch ? pitch : dpitch);
    return D3D_OK;
}

HRESULT WINAPI MetalSwapChain::GetFrontBufferData(IDirect3DSurface9 *d) { return dev_->GetRenderTargetData(dev_->backBuffer(), d); }
HRESULT WINAPI MetalSwapChain::Present(const RECT *a, const RECT *b, HWND c, const RGNDATA *d, DWORD) { return dev_->Present(a, b, c, d); }
HRESULT WINAPI MetalSwapChain::GetBackBuffer(UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **pp) {
    *pp = dev_->backBuffer();
    (*pp)->AddRef();
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **pp) {
    bbSurface_->AddRef();
    *pp = bbSurface_;
    return D3D_OK;
}

HRESULT WINAPI MetalDevice::GetSwapChain(UINT, IDirect3DSwapChain9 **pp) {
    if (!swapChain_) swapChain_ = new MetalSwapChain(this);
    swapChain_->AddRef();
    *pp = swapChain_;
    return D3D_OK;
}

// A TGA of a surface (D3DXSaveSurfaceToFileA: the screenshot command saves the system
// memory surface GetFrontBufferData filled).
HRESULT MTL_SaveSurfaceToFile(const char *file, IDirect3DSurface9 *surface) {
    MetalSurface *s = static_cast<MetalSurface *>(surface);
    if (!s || s->kind() != MetalSurface::S_SYSMEM || MTL_Format(s->format(), false).bytes != 4) {
        MTL_Bug("D3DXSaveSurfaceToFileA of this surface");
        return D3DERR_INVALIDCALL;
    }
    FILE *f = fopen(file, "wb");
    if (!f) return E_FAIL;
    const int w = (int)s->width(), h = (int)s->height();
    unsigned char hdr[18] = {};
    hdr[2] = 2;
    hdr[12] = w & 0xFF; hdr[13] = (w >> 8) & 0xFF;
    hdr[14] = h & 0xFF; hdr[15] = (h >> 8) & 0xFF;
    hdr[16] = 32; hdr[17] = 8 | 0x20;   // top-down
    fwrite(hdr, 1, 18, f);
    fwrite(s->sysmem().data(), 1, (size_t)w * h * 4, f);
    fclose(f);
    return D3D_OK;
}

// KB_SCREENSHOT=<dir> (+KB_SCREENSHOT_EVERY=n): the back buffer every n presents, as the
// GL layer writes it.
static void MaybeScreenshot(MetalDevice *dev, MetalSurface *bb) {
    static const char *dir = getenv("KB_SCREENSHOT");
    if (!dir) return;
    static int every = [] { const char *e = getenv("KB_SCREENSHOT_EVERY"); int n = e ? atoi(e) : 0; return n > 0 ? n : 300; }();
    static unsigned long presents;
    if (++presents % every) return;
    MetalSurface tmp(dev, MetalSurface::S_SYSMEM, bb->width(), bb->height(), D3DFMT_A8R8G8B8, nullptr, nullptr, 0, 0);
    dev->GetRenderTargetData(bb, &tmp);
    for (size_t i = 3; i < tmp.sysmem().size(); i += 4) tmp.sysmem()[i] = 0xFF;
    char path[1024];
    snprintf(path, sizeof(path), "%s/present_%06lu.tga", dir, presents);
    MTL_SaveSurfaceToFile(path, &tmp);
    fprintf(stderr, "[mtl] wrote %s\n", path);
}

// ---- Present -------------------------------------------------------------------------------
HRESULT WINAPI MetalDevice::Present(const RECT *, const RECT *, HWND, const RGNDATA *) {
    compiler_.submitIdle();
    {
        std::string map;
        if (MTL_TakeLevelEnd(&map)) pipelineCache_.endMap(map);
    }
    MaybeScreenshot(this, bbSurface_);
    flushClear();
    endEncoder();
    resolveBackBuffer();
    beginCommandBuffer();
    // A ready drawable (waiting for one only with vsync), and a free present texture.
    const double t0 = Now();
    id<CAMetalDrawable> drawable = win_->takeDrawable(vsync_);
    statsWait_ += Now() - t0;
    const int slot = presentSlot_;
    if (drawable && presentBusy_[slot].load(std::memory_order_acquire)) drawable = nil;   // dropped: the display is behind
    if (drawable) {
        id<MTLTexture> dt = drawable.texture;
        const int winW = (int)dt.width, winH = (int)dt.height;
        if (!presentTex_[slot] || presentTex_[slot].width != dt.width || presentTex_[slot].height != dt.height) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                         width:dt.width height:dt.height
                                                                                     mipmapped:NO];
            td.storageMode = MTLStorageModePrivate;
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            presentTex_[slot] = [mtl newTextureWithDescriptor:td];
        }
        // A fullscreen window covers the display and the back buffer fills it (the
        // engine's aspect ratio is the monitor's there); a window keeps the back
        // buffer's shape (glcontext_sdl.cpp, gl_d3d9.cpp).
        int x0 = 0, y0 = 0, x1 = winW, y1 = winH;
        const bool scaled = winW != bbWidth_ || winH != bbHeight_;
        if (scaled && !fullscreen_) {
            if ((long long)winW * bbHeight_ > (long long)winH * bbWidth_) {
                const int w = (int)((long long)winH * bbWidth_ / bbHeight_);
                x0 = (winW - w) / 2; x1 = x0 + w;
            } else {
                const int h = (int)((long long)winW * bbHeight_ / bbWidth_);
                y0 = (winH - h) / 2; y1 = y0 + h;
            }
        }
        KB_NotePresentRect(x0, y0, x1 - x0, y1 - y0, bbWidth_, bbHeight_);
        id<MTLTexture> src = samples_ > 1 ? bbResolve_->texture() : bbColor_->texture();
        fullscreenPass(src, 0, 0, RECT{ 0, 0, bbWidth_, bbHeight_ }, presentTex_[slot], 0, 0, RECT{ x0, y0, x1, y1 },
                       scaled, x0 || y0 || x1 != winW || y1 != winH);
        [cmd_ encodeSignalEvent:presentEvent_ value:++presentValue_];
        commit(true);
        presentBusy_[slot] = true;
        presentSlot_ = (slot + 1) % 3;
        @autoreleasepool {   // after the frame's pool
        id<MTLCommandBuffer> pc = [presentQueue_ commandBuffer];
        [pc encodeWaitForEvent:presentEvent_ value:presentValue_];
        // A drawable is framebuffer-only: drawn into, not blitted.
        const RECT whole = { 0, 0, winW, winH };
        encodeBlit(pc, presentTex_[slot], whole, dt, 0, 0, whole, false, false);
        std::atomic<unsigned> *shown = &statsShown_;
        [drawable addPresentedHandler:^(id<MTLDrawable> d) {
            if (d.presentedTime > 0) shown->fetch_add(1, std::memory_order_relaxed);
        }];
        [pc presentDrawable:drawable];
        std::atomic<bool> *busy = &presentBusy_[slot];
        [pc addCompletedHandler:^(id<MTLCommandBuffer>) { busy->store(false, std::memory_order_release); }];
        [pc commit];
        }
        ++statsPresented_;
    } else {
        commit(true);
    }
    ++g_kbPresentEnter;
    {
        static double last;
        const double now = Now();
        if (last && now - last > 0.015) MTL_ShaderLog("p %.1f %u %.1f", (now - last) * 1000.0, framePipes_, frameBuild_ * 1000.0);
        last = now;
        frameBuild_ = 0;
        framePipes_ = 0;
    }
    stats();
    return D3D_OK;
}

// KB_FPS=<seconds>: the GL layer's [fps] line (tools/bench_views.py reads it).
void MetalDevice::stats() {
    static const double interval = [] {
        const char *e = getenv("KB_FPS");
        if (!e) return 0.0;
        const double s = atof(e);
        return s > 0.0 ? s : 1.0;
    }();
    if (interval <= 0.0) return;
    const double now = Now();
    if (!statsStart_) { statsStart_ = statsLast_ = now; return; }
    const double frame = (now - statsLast_) * 1000.0;
    statsLast_ = now;
    ++statsFrames_;
    statsSum_ += frame;
    if (frame > statsMax_) statsMax_ = frame;
    if (now - statsStart_ >= interval) {
        const double el = now - statsStart_;
        fprintf(stderr, "[fps] %.1f fps, frame %.2f ms avg %.2f max, swap %.2f ms avg, %lu draws/frame; "
                        "per frame: %lu blits, %lu uploads %llu KB, %llu KB orphaned; %.0f presented/s, %.0f shown/s; "
                        "%u pipelines built in %.0f ms (%.0f waiting for shader batches, %.0f functions, longest %.0f), %u from the "
                        "cache file\n",
                statsFrames_ / el, statsSum_ / statsFrames_, statsMax_, statsWait_ * 1000.0 / statsFrames_,
                (g_kbDraws - statsDraws_) / statsFrames_, (g_kbBlits - statsBlits_) / statsFrames_,
                (g_kbUploads - statsUploads_) / statsFrames_, (g_kbUploadBytes - statsUploadBytes_) / statsFrames_ / 1024, 0ull,
                statsPresented_ / el, statsShown_.exchange(0) / el, statsPipes_, statsBuild_ * 1000.0,
                statsBatchWait_ * 1000.0, statsFunctions_ * 1000.0, statsBuildMax_ * 1000.0, statsCacheHits_);
        statsStart_ = now;
        statsFrames_ = statsPresented_ = statsPipes_ = statsCacheHits_ = 0;
        statsSum_ = statsMax_ = statsWait_ = statsBuild_ = statsBatchWait_ = statsFunctions_ = statsBuildMax_ = 0;
        statsDraws_ = g_kbDraws; statsBlits_ = g_kbBlits; statsUploads_ = g_kbUploads; statsUploadBytes_ = g_kbUploadBytes;
    }
}

HRESULT WINAPI MetalDevice::Reset(D3DPRESENT_PARAMETERS *pp) {
    if (!pp->BackBufferWidth || !pp->BackBufferHeight) return D3D_OK;
    finishGPU();
    @autoreleasepool {
        bbWidth_ = (int)pp->BackBufferWidth;
        bbHeight_ = (int)pp->BackBufferHeight;
        int samples = (int)pp->MultiSampleType >= 2 ? (int)pp->MultiSampleType : 1;
        while (samples > 1 && ![mtl supportsTextureSampleCount:samples]) --samples;
        samples_ = samples;
        fullscreen_ = !pp->Windowed;
        vsync_ = pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE;
        createBackBuffer();
        pipes_.clear();
        for (MetalSurface *&r : rt_) Rebind(r, (MetalSurface *)nullptr);
        Rebind(rt_[0], bbSurface_);
        Rebind(ds_, pp->EnableAutoDepthStencil ? autoDSSurface_ : nullptr);
        resetViewportToTarget();
        win_->resize(bbWidth_, bbHeight_);
        win_->setFullscreen(fullscreen_);
        win_->setVSync(vsync_);
        fprintf(stderr, "[mtl] reset: back buffer %dx%d, %d sample%s\n", bbWidth_, bbHeight_, samples_, samples_ > 1 ? "s" : "");
    }
    return D3D_OK;
}

// ---- Resource creation -------------------------------------------------------------------
HRESULT WINAPI MetalDevice::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                          IDirect3DTexture9 **pp, HANDLE *) {
    @autoreleasepool {
        *pp = new MetalTexture2D(this, w, h, levels, usage, fmt, pool);
        return D3D_OK;
    }
}

HRESULT WINAPI MetalDevice::CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt,
                                                D3DPOOL pool, IDirect3DVolumeTexture9 **pp, HANDLE *) {
    @autoreleasepool {
        *pp = new MetalVolumeTexture(this, w, h, d, levels, usage, fmt, pool);
        return D3D_OK;
    }
}

HRESULT WINAPI MetalDevice::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                              IDirect3DCubeTexture9 **pp, HANDLE *) {
    @autoreleasepool {
        *pp = new MetalCubeTexture(this, edge, levels, usage, fmt, pool);
        return D3D_OK;
    }
}

HRESULT WINAPI MetalDevice::CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                               IDirect3DVertexBuffer9 **pp, HANDLE *) {
    @autoreleasepool {
        *pp = new MetalVertexBuffer(this, len, usage, fvf, pool);
        return D3D_OK;
    }
}

HRESULT WINAPI MetalDevice::CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                              IDirect3DIndexBuffer9 **pp, HANDLE *) {
    @autoreleasepool {
        if (fmt != D3DFMT_INDEX16 && (unsigned)fmt != 102) MTL_Bug("index format %u", (unsigned)fmt);
        *pp = new MetalIndexBuffer(this, len, usage, fmt, pool);
        return D3D_OK;
    }
}

// A render target or depth-stencil surface of its own (multisampled with the back buffer).
static MetalSurface *StandaloneSurface(MetalDevice *dev, MetalSurface::Kind kind, UINT w, UINT h, D3DFORMAT fmt,
                                       D3DMULTISAMPLE_TYPE ms) {
    const DWORD usage = kind == MetalSurface::S_DEPTHSTENCIL ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET;
    const MetalFormat m = MTL_Format(fmt, kind == MetalSurface::S_DEPTHSTENCIL);
    int samples = (int)ms >= 2 ? (int)ms : 1;
    while (samples > 1 && ![dev->mtl supportsTextureSampleCount:samples]) --samples;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)m.pixelFormat
                                                                                 width:w height:h mipmapped:NO];
    td.textureType = samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    td.sampleCount = samples;
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    if (!m.pixelFormat) MTL_Bug("surface format %u", (unsigned)fmt);
    MetalSurface *s = new MetalSurface(dev, kind, w, h, fmt, nullptr, nullptr, 0, 0);
    s->setTexture(new MetalTexture(dev, [dev->mtl newTextureWithDescriptor:td], fmt, usage), true);
    return s;
}

HRESULT WINAPI MetalDevice::CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD, WINBOOL,
                                               IDirect3DSurface9 **pp, HANDLE *) {
    @autoreleasepool {
        *pp = StandaloneSurface(this, MetalSurface::S_RENDERTARGET, w, h, fmt, ms);
        return D3D_OK;
    }
}

HRESULT WINAPI MetalDevice::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD, WINBOOL,
                                                      IDirect3DSurface9 **pp, HANDLE *) {
    @autoreleasepool {
        *pp = StandaloneSurface(this, MetalSurface::S_DEPTHSTENCIL, w, h, fmt, ms);
        return D3D_OK;
    }
}

HRESULT WINAPI MetalDevice::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL, IDirect3DSurface9 **pp,
                                                        HANDLE *) {
    @autoreleasepool {
        *pp = new MetalSurface(this, MetalSurface::S_SYSMEM, w, h, fmt, nullptr, nullptr, 0, 0);
        return D3D_OK;
    }
}

HRESULT WINAPI MetalDevice::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **pp) {
    @autoreleasepool {
        *pp = new MetalDecl(this, e);
        return D3D_OK;
    }
}

HRESULT WINAPI MetalDevice::CreateVertexShader(const DWORD *f, IDirect3DVertexShader9 **pp) {
    @autoreleasepool {
        *pp = new MetalVertexShader(this, f);
        return D3D_OK;
    }
}

HRESULT WINAPI MetalDevice::CreatePixelShader(const DWORD *f, IDirect3DPixelShader9 **pp) {
    @autoreleasepool {
        *pp = new MetalPixelShader(this, f);
        return D3D_OK;
    }
}

HRESULT WINAPI MetalDevice::CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **pp) {
    @autoreleasepool {
        if (type != D3DQUERYTYPE_EVENT && type != D3DQUERYTYPE_OCCLUSION) { MTL_Bug("query type %u", (unsigned)type); return D3DERR_NOTAVAILABLE; }
        *pp = new MetalQuery(this, type);
        return D3D_OK;
    }
}

// ---- Factory ---------------------------------------------------------------------------------
extern "C" IDirect3D9 *WINAPI GL_Direct3DCreate9(UINT sdk);

MetalD3D9::MetalD3D9() : gl_(GL_Direct3DCreate9(32)), dev_(MTLCreateSystemDefaultDevice()) {}
MetalD3D9::~MetalD3D9() { gl_->Release(); }

UINT WINAPI MetalD3D9::GetAdapterCount() { return gl_->GetAdapterCount(); }
UINT WINAPI MetalD3D9::GetAdapterModeCount(UINT a, D3DFORMAT f) { return gl_->GetAdapterModeCount(a, f); }
HRESULT WINAPI MetalD3D9::EnumAdapterModes(UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE *d) { return gl_->EnumAdapterModes(a, f, m, d); }
HMONITOR WINAPI MetalD3D9::GetAdapterMonitor(UINT a) { return gl_->GetAdapterMonitor(a); }
HRESULT WINAPI MetalD3D9::GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE *m) { return gl_->GetAdapterDisplayMode(a, m); }
HRESULT WINAPI MetalD3D9::CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD u, D3DRESOURCETYPE r, D3DFORMAT f) {
    return gl_->CheckDeviceFormat(a, t, af, u, r, f);
}

HRESULT WINAPI MetalD3D9::GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9 *id) {
    *id = D3DADAPTER_IDENTIFIER9{};
    snprintf(id->Description, sizeof(id->Description), "%s", dev_ ? dev_.name.UTF8String : "Metal");
    snprintf(id->Driver, sizeof(id->Driver), "Metal");
    return D3D_OK;
}

HRESULT WINAPI MetalD3D9::GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS9 *caps) {
    FillCaps(caps);
    return D3D_OK;
}

// The engine steps its sample count down until this accepts one (R_SetupAntiAliasing).
HRESULT WINAPI MetalD3D9::CheckDeviceMultiSampleType(UINT, D3DDEVTYPE, D3DFORMAT, WINBOOL, D3DMULTISAMPLE_TYPE type, DWORD *q) {
    if (q) *q = 1;
    const int n = (int)type;
    if (n >= 2 && !(dev_ && [dev_ supportsTextureSampleCount:n])) return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}

HRESULT WINAPI MetalD3D9::CreateDevice(UINT adapter, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *pp,
                                       IDirect3DDevice9 **out) {
    @autoreleasepool {
        *out = nullptr;
        if (!dev_) { fprintf(stderr, "[mtl] no Metal device\n"); return E_FAIL; }
        const int w = pp->BackBufferWidth ? (int)pp->BackBufferWidth : 640;
        const int h = pp->BackBufferHeight ? (int)pp->BackBufferHeight : 480;
        MetalWindow *win = MetalWindow::Create(dev_, w, h, !pp->Windowed,
                                               pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE, (int)adapter);
        if (!win) return E_FAIL;
        D3DPRESENT_PARAMETERS p = *pp;
        p.BackBufferWidth = w;
        p.BackBufferHeight = h;
        int samples = (int)pp->MultiSampleType >= 2 ? (int)pp->MultiSampleType : 1;
        while (samples > 1 && ![dev_ supportsTextureSampleCount:samples]) --samples;
        *out = new MetalDevice(win, dev_, p, samples);
        return D3D_OK;
    }
}
