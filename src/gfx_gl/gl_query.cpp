// gl_query.cpp — GL occlusion / event queries + GLDevice::CreateQuery.
#include "gl_query.h"
#include "gl_d3d9.h"

#include <GL/glew.h>

// Counters read by the KB_SCREENSHOT report (gl_d3d9.cpp) and the program cache's
// per-present link budget (gl_program.cpp).
unsigned long g_kbProgLinks   = 0;   // (vs,ps) program links (lazy, at first draw use)
unsigned long g_kbDraws       = 0;   // GLDevice::Draw(Indexed)Primitive calls
unsigned long g_kbSkipPending = 0;   // draws skipped because the program is still linking
unsigned long g_kbBuiltinFall = 0;   // draws degraded to the builtin program (translate/compile gave 0)
unsigned long g_kbBlits       = 0;   // StretchRect / glBlitFramebuffer calls
unsigned long g_kbPresentEnter= 0;   // presents (SwapBuffers entries)
unsigned long g_kbUploads     = 0;   // vertex/index buffer uploads (KB_FPS)
unsigned long long g_kbUploadBytes = 0;   // ... their bytes
unsigned long long g_kbOrphanBytes = 0;   // bytes of buffer storage orphaned by DISCARD locks

// GL_SAMPLES_PASSED: the exact sample count, as D3D9's occlusion query returns it.
static constexpr GLenum KB_OCCLUSION_TARGET = GL_SAMPLES_PASSED;

GLQuery::GLQuery(IDirect3DDevice9 *device, D3DQUERYTYPE type) : device_(device), type_(type) {
    if (type_ == D3DQUERYTYPE_OCCLUSION) glGenQueries(1, &glQuery_);
}

GLQuery::~GLQuery() {
    if (glQuery_) glDeleteQueries(1, &glQuery_);
    if (sync_)    glDeleteSync(static_cast<GLsync>(sync_));
}

HRESULT WINAPI GLQuery::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_;
    if (device_) device_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI GLQuery::Issue(DWORD dwIssueFlags) {
    if (type_ == D3DQUERYTYPE_OCCLUSION) {
        if (dwIssueFlags & D3DISSUE_BEGIN) { haveResult_ = false; glBeginQuery(KB_OCCLUSION_TARGET, glQuery_); }
        if (dwIssueFlags & D3DISSUE_END)   glEndQuery(KB_OCCLUSION_TARGET);
    } else if (type_ == D3DQUERYTYPE_EVENT) {
        if (dwIssueFlags & D3DISSUE_END) {
            if (sync_) glDeleteSync(static_cast<GLsync>(sync_));
            sync_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
    }
    return D3D_OK;
}

HRESULT WINAPI GLQuery::GetData(void *pData, DWORD /*dwSize*/, DWORD dwGetDataFlags) {
    bool flush = (dwGetDataFlags & D3DGETDATA_FLUSH) != 0;

    if (type_ == D3DQUERYTYPE_OCCLUSION) {
        GLint available = 0;
        glGetQueryObjectiv(glQuery_, GL_QUERY_RESULT_AVAILABLE, &available);
        // Defer when the engine is just polling (no FLUSH): never block here, or a wave of
        // newly visible objects (looking around, spawning) each forces a GPU flush. Only
        // the explicit-FLUSH path reads the result (may block).
        if (!available && !flush) {
            return S_FALSE;
        }
        GLuint samples = 0;
        glGetQueryObjectuiv(glQuery_, GL_QUERY_RESULT, &samples);  // blocks if flush
        if (pData) *static_cast<DWORD *>(pData) = samples;
        return S_OK;
    }

    if (type_ == D3DQUERYTYPE_EVENT) {
        // No outstanding fence (never Issue'd, or already consumed): real D3D9
        // reports such an event query as signaled. Returning S_FALSE here makes
        // R_FinishGpuFence's `while (GetData == S_FALSE)` spin forever, since the
        // engine waits on dx.flushGpuQuery without ever issuing it.
        if (!sync_) { if (pData) *static_cast<DWORD *>(pData) = TRUE; return S_OK; }
        // This fence is the engine's GPU throttle (R_FinishGpuFence): it keeps the render
        // backend from outrunning the GPU. glClientWaitSync with a zero timeout doesn't
        // block; it returns S_FALSE until the GPU passes the fence, and the engine's wait
        // loop throttles on that. The flush bit (when the engine asks for it) makes sure
        // the fence reaches the GPU while the engine polls.
        const GLbitfield fbit = flush ? GL_SYNC_FLUSH_COMMANDS_BIT : 0;
        GLenum r = glClientWaitSync(static_cast<GLsync>(sync_), fbit, 0);
        bool done = (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED);
        if (!done) return S_FALSE;
        if (pData) *static_cast<DWORD *>(pData) = TRUE;
        return S_OK;
    }
    return S_OK;
}

HRESULT WINAPI GLDevice::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) {
    if (!ppQuery) return E_INVALIDARG;
    *ppQuery = new GLQuery(this, Type);
    return D3D_OK;
}
