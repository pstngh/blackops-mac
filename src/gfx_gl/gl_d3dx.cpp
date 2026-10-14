// gl_d3dx.cpp — GL-backed implementations of the small D3DX surface the renderer
// uses (declared in <d3dx9.h>). Screenshot save + a shader-bytecode buffer.
//
// D3DXSaveSurfaceToFileA reads a surface back via an FBO and writes it out; only
// uncompressed TGA is produced today (the game's screenshot path). Surfaces hold
// D3D's row order (top row first), written as a top-down TGA. Shader reflection / HLSL compilation
// (D3DXGetShaderConstantTable, D3DXCompileShader) are not implemented: the game
// ships compiled bytecode, and the bytecode→GLSL translator already recovers the
// constant model. They return E_NOTIMPL with a clear marker.
#include "d3dx9.h"
#include "gl_object.h"
#include "gl_resources.h"
#include "gl_spec.h"

#include <GL/glew.h>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

class GLD3DXBuffer final : public GLObject<ID3DXBuffer> {
public:
    explicit GLD3DXBuffer(DWORD bytes) : data_(bytes ? bytes : 1) {}
    void *WINAPI GetBufferPointer() override { return data_.data(); }
    DWORD WINAPI GetBufferSize() override { return (DWORD)data_.size(); }
private:
    std::vector<unsigned char> data_;
};

// Read a surface into a BGRA8 CPU buffer. A system-memory surface (the screenshot
// target, filled by GetFrontBufferData) already holds BGRA8 in its shadow; a
// renderable surface is read back via a temporary FBO.
bool ReadSurfaceBGRA(GLSurface *s, int w, int h, std::vector<unsigned char> &out) {
    if (!s) return false;
    if (!s->texName()) {
        std::vector<unsigned char> &shadow = s->shadow();
        if (shadow.size() < (size_t)w * h * 4) return false;
        out.assign(shadow.begin(), shadow.begin() + (size_t)w * h * 4);
        return true;
    }
    out.assign((size_t)w * h * 4, 0);
    GLint prevRead = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           s->texName(), s->level());
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);
    glDeleteFramebuffers(1, &fbo);
    return true;
}

}  // namespace

extern "C" {

HRESULT WINAPI D3DXCreateBuffer(DWORD NumBytes, LPD3DXBUFFER *ppBuffer) {
    if (!ppBuffer) return E_INVALIDARG;
    KB_SPEC("D3DXCreateBuffer %s", KB_SpecThread());
    *ppBuffer = new GLD3DXBuffer(NumBytes);
    return D3D_OK;
}

// On macOS mtl_window.mm's D3DXSaveSurfaceToFileA calls this for GL surfaces.
#if defined(__APPLE__)
HRESULT WINAPI GL_D3DXSaveSurfaceToFileA(const char *pDestFile, D3DXIMAGE_FILEFORMAT /*fmt*/,
#else
HRESULT WINAPI D3DXSaveSurfaceToFileA(const char *pDestFile, D3DXIMAGE_FILEFORMAT /*fmt*/,
#endif
                                      IDirect3DSurface9 *pSrcSurface, const void * /*palette*/,
                                      const RECT *srcRect) {
    GLSurface *s = static_cast<GLSurface *>(pSrcSurface);
    KB_SPEC("D3DXSaveSurfaceToFileA %s", KB_SpecThread());
    if (!s || !pDestFile) return E_INVALIDARG;
    int sw = (int)s->width(), sh = (int)s->height();
    std::vector<unsigned char> bgra;
    if (!ReadSurfaceBGRA(s, sw, sh, bgra)) return E_FAIL;
    // The rect's part (the screenshot's: the window's, the surface being the monitor's size with
    // the back buffer in its corner), opaque.
    int x0 = 0, y0 = 0, x1 = sw, y1 = sh;
    if (srcRect) {
        x0 = std::max(0, (int)srcRect->left); y0 = std::max(0, (int)srcRect->top);
        x1 = std::min(sw, (int)srcRect->right); y1 = std::min(sh, (int)srcRect->bottom);
        if (x1 <= x0 || y1 <= y0) return E_INVALIDARG;
    }
    const int w = x1 - x0, h = y1 - y0;

    FILE *f = fopen(pDestFile, "wb");
    if (!f) return E_FAIL;
    // 18-byte uncompressed-true-colour TGA header; 32bpp BGRA, 8 alpha bits.
    unsigned char hdr[18] = {0};
    hdr[2]  = 2;
    hdr[12] = (unsigned char)(w & 0xFF); hdr[13] = (unsigned char)((w >> 8) & 0xFF);
    hdr[14] = (unsigned char)(h & 0xFF); hdr[15] = (unsigned char)((h >> 8) & 0xFF);
    hdr[16] = 32; hdr[17] = 8 | 0x20;   // 8 alpha bits, top-left origin
    fwrite(hdr, 1, 18, f);
    for (int y = y0; y < y1; ++y) {
        unsigned char *row = bgra.data() + ((size_t)y * sw + x0) * 4;
        for (int i = 3; i < w * 4; i += 4) row[i] = 0xFF;
        fwrite(row, 1, (size_t)w * 4, f);
    }
    fclose(f);
    return D3D_OK;
}

// --- Not implemented (game ships bytecode; reflection handled by the translator) ---
HRESULT WINAPI D3DXCompileShader(const char *, UINT, const void *, void *, const char *,
                                 const char *, DWORD, LPD3DXBUFFER *, LPD3DXBUFFER *,
                                 LPD3DXCONSTANTTABLE *) { return E_NOTIMPL; }
HRESULT WINAPI D3DXGetShaderConstantTable(const DWORD *, LPD3DXCONSTANTTABLE *pp) { if (pp) *pp = nullptr; return E_NOTIMPL; }
HRESULT WINAPI D3DXGetShaderInputSemantics(const DWORD *, void *, UINT *c) { if (c) *c = 0; return E_NOTIMPL; }
HRESULT WINAPI D3DXGetShaderOutputSemantics(const DWORD *, void *, UINT *c) { if (c) *c = 0; return E_NOTIMPL; }

}  // extern "C"
