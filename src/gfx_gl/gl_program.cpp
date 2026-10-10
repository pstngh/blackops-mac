// gl_program.cpp — programmable shader path: create/bind shaders, upload the c#
// constant registers, and link+cache the bound (vs, ps) pair into a GL program.
#include "gl_d3d9.h"
#include "gl_platform.h"
#include "gl_shader.h"
#include "gl_resources.h"
#include "gl_spec.h"

#include <GL/glew.h>

#include <cstdio>
#include <cstring>

HRESULT WINAPI GLDevice::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) {
    if (!ppShader || !pFunction) return E_INVALIDARG;
    *ppShader = new GLVertexShader(this, pFunction);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) {
    if (!ppShader || !pFunction) return E_INVALIDARG;
    *ppShader = new GLPixelShader(this, pFunction);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetVertexShader(IDirect3DVertexShader9 *pShader) {
    GLVertexShader *vs = static_cast<GLVertexShader *>(pShader);
    KB_SPEC("SetVertexShader %s null=%d", KB_SpecThread(), vs == nullptr);
    if (vs_ == vs) return D3D_OK;   // no-change fast path (engine re-sets per drawSurf)
    vs_ = vs;
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetPixelShader(IDirect3DPixelShader9 *pShader) {
    GLPixelShader *ps = static_cast<GLPixelShader *>(pShader);
    KB_SPEC("SetPixelShader %s null=%d", KB_SpecThread(), ps == nullptr);
    if (ps_ == ps) return D3D_OK;   // no-change fast path
    ps_ = ps;
    return D3D_OK;
}

// Instancing (gl_d3d9_draw.cpp): the per-object matrix candidate = the vs-const range changed
// since the last draw, tracked here.

HRESULT WINAPI GLDevice::SetVertexShaderConstantF(UINT StartRegister, const float *pData, UINT Vec4Count) {
    KB_SPEC("SetVertexShaderConstantF %s end=%u", KB_SpecThread(), StartRegister + Vec4Count);
    // No-change fast path: the engine re-sets identical constants around most draws.
    // Flushing+bumping only on REAL changes keeps draw batches alive (flushes/f used
    // to equal draws/f) and lets useDrawProgram skip the per-draw uniform uploads.
    if (pData && StartRegister + Vec4Count <= 256) {
        float *dst = vsConst_ + StartRegister * 4;
        size_t bytes = Vec4Count * 4 * sizeof(float);
        if (std::memcmp(dst, pData, bytes) != 0) {
            std::memcpy(dst, pData, bytes);
            if (vsDirtyMin_ > vsDirtyMax_) vsDirtyBaseVer_ = vsVer_;  // span was empty
            if (StartRegister < vsDirtyMin_) vsDirtyMin_ = StartRegister;
            if (StartRegister + Vec4Count - 1 > vsDirtyMax_) vsDirtyMax_ = StartRegister + Vec4Count - 1;
            ++vsVer_;
        }
    }
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetPixelShaderConstantF(UINT StartRegister, const float *pData, UINT Vec4Count) {
    KB_SPEC("SetPixelShaderConstantF %s end=%u", KB_SpecThread(), StartRegister + Vec4Count);
    if (pData && StartRegister + Vec4Count <= 256) {
        float *dst = psConst_ + StartRegister * 4;
        size_t bytes = Vec4Count * 4 * sizeof(float);
        if (std::memcmp(dst, pData, bytes) != 0) {
            std::memcpy(dst, pData, bytes);
            if (psDirtyMin_ > psDirtyMax_) psDirtyBaseVer_ = psVer_;  // span was empty
            if (StartRegister < psDirtyMin_) psDirtyMin_ = StartRegister;
            if (StartRegister + Vec4Count - 1 > psDirtyMax_) psDirtyMax_ = StartRegister + Vec4Count - 1;
            ++psVer_;
        }
    }
    return D3D_OK;
}

// Finish a program's link and, once linked, cache all uniform locations. Returns false
// while it isn't usable yet.

bool GLDevice::finalizeProgram(LinkedProgram &lp) {
    GLint ok = 0;
    glGetProgramiv(lp.prog, GL_LINK_STATUS, &ok);   // ready now (or native): does not stall
    if (!ok) {
        char log[1024];
        log[0] = 0;
        glGetProgramInfoLog(lp.prog, sizeof(log), nullptr, log);
        // A failed status with an empty log is treated as success (a workaround from
        // the WebGL target, whose status queries could be stale; a desktop driver always
        // logs a real failure). Only a real error text means a real failure (delete +
        // bounded retry below).
        if (!log[0]) {
            // Empty-log "failure": trust it and finish setup; the draw path does a
            // verified bind later and skips only the draws whose program isn't usable.
            static int staleN = 0;
            if (++staleN <= 3)
                fprintf(stderr, "[gl] link status false+empty (#%d) — trusting; draw-path verifies the bind\n", staleN);
            ok = 1;
        }
    }
    if (!ok) {
        char log[1024];
        log[0] = 0;
        glGetProgramInfoLog(lp.prog, sizeof(log), nullptr, log);
        static int linkFailPrints = 0;
        if (++linkFailPrints <= 16)
            fprintf(stderr, "[gl] program link failed (try %d): %s\n", lp.linkTries + 1, log[0] ? log : "(empty log)");
        // Self-heal: marking this "ready" made its materials permanently black
        // (useProgram on an invalid program — the 256x 'program not valid' spam).
        // Delete and re-link on a later frame instead; the caller skips the draw.
        extern unsigned long g_kbPresentEnter;
        glDeleteProgram(lp.prog);
        lp.prog = 0;
        lp.pendPolls = 0;
        ++lp.linkTries;
        lp.lastFailPres = g_kbPresentEnter;
        return false;
    }
    lp.vscLoc = glGetUniformLocation(lp.prog, "vsc");
    lp.pscLoc = glGetUniformLocation(lp.prog, "psc");
    // How many vec4s the constant arrays actually declare (the translator now sizes them
    // to the shader's highest referenced register, not a blanket 256). Upload only these
    // per draw instead of a blanket 256+256 vec4s.
    auto arraySize = [&](int loc, const char *want) -> int {
        if (loc < 0) return 0;
        GLint nu = 0; glGetProgramiv(lp.prog, GL_ACTIVE_UNIFORMS, &nu);
        size_t wl = strlen(want);
        for (int i = 0; i < nu; ++i) {
            char nm[80]; GLint sz = 0; GLenum ty = 0; GLsizei len = 0;
            nm[0] = 0;
            glGetActiveUniform(lp.prog, i, sizeof(nm), &len, &sz, &ty, nm);
            if (!strncmp(nm, want, wl) && (nm[wl] == '[' || nm[wl] == '\0')) return sz;
        }
        return 0;
    };
    lp.posFixupLoc = glGetUniformLocation(lp.prog, "kbPosFixup");
    lp.vscCount = arraySize(lp.vscLoc, "vsc");
    lp.pscCount = arraySize(lp.pscLoc, "psc");
#ifdef KB_GL_MODERN_GLSL
    lp.alphaFuncLoc = glGetUniformLocation(lp.prog, "uAlphaTestFunc");
    lp.alphaRefLoc  = glGetUniformLocation(lp.prog, "uAlphaRef");
#endif
    for (int i = 0; i < kMaxStages; ++i) {
        char name[4]; snprintf(name, sizeof(name), "s%d", i);
        lp.samplerLoc[i] = glGetUniformLocation(lp.prog, name);
    }
    // Sampler->unit mapping (s0->0, ...) is set on the FIRST verified bind in the draw
    // path (see below) — NOT here. Binding right after link can be rejected on this
    // driver (result undelivered), which would leave the uniforms unset; doing it at
    // the first clean bind guarantees the program is actually current.
    lp.ready = true;
    return true;
}

// Pick the draw program: the linked (vs,ps) pair if both are bound and valid,
// otherwise the built-in pre-transformed program. Sets the program's uniforms.
// Returns false if the (vs,ps) program is still linking — caller must skip the draw.
bool GLDevice::useDrawProgram() {
    // Resolve any staged blend render-states once here — the single choke point every draw
    // path funnels through (builtin + programmable + instanced + batched). See commitBlendState.
    commitBlendState();
    commitSampleShading();
    if (!(vs_ && ps_ && vs_->ok() && ps_->ok())) {
        bindBuiltinForDraw();
        return true;
    }

    // glShader() compiles lazily; a GLSL compile REJECTION leaves it 0. Never attach a
    // 0 shader: draw with the builtin instead, like !ok().
    // Shadow-sampler mask: stages with a DEPTH texture bound need the pixel shader's
    // matching samplers typed sampler2DShadow (depth-compare). glShader(mask) returns a
    // cached per-mask variant; the program cache keys on shader ids so variants link
    // independently.
    // NO sampler retyping: the GLSL dump of the real shaders proved Black Ops does
    // MANUAL PCF — plain texture(s2, xy) taps reading raw depth + in-shader compares.
    // sampler2DShadow variants broke every tap (no vec2 overload) and the draws fell
    // to the builtin = black ground. Depth textures sample as regular sampler2D with
    // COMPARE_MODE NONE, returning depth in .x — exactly what the shaders expect.
    unsigned shadowMask = 0;
    (void)0;
    {
        static bool once = false;
        if (shadowMask && !once) {
            once = true;
            fprintf(stderr, "[gl] shadow sampler variant active (stage mask=0x%x)\n", shadowMask);
        }
    }
    unsigned vsN = vs_->glShader();
    unsigned psN = ps_->glShader(shadowMask);
    if (!vsN || !psN) {
        // Engine-supplied shaders degraded to the builtin passthrough: world geometry
        // drawn this way lands at garbage clip coords = INVISIBLE for the frame.
        extern unsigned long g_kbBuiltinFall; ++g_kbBuiltinFall;
        bindBuiltinForDraw();
        return true;
    }

    // Per-frame NEW-LINK budget: each kicked link is force-finished immediately
    // (see finalizeProgram — stale results are unreadable on this driver), so this
    // bounds the per-frame stall. Programs over budget skip their draws and get
    // kicked on a later frame.
    extern unsigned long g_kbPresentEnter;
    static unsigned long s_linkPres = ~0ul; static int s_linksThisPres = 0;
    auto linkBudgetOk = [&]() -> bool {
        if (s_linkPres != g_kbPresentEnter) { s_linkPres = g_kbPresentEnter; s_linksThisPres = 0; }
        return s_linksThisPres < 4;
    };

    uint64_t key = (uint64_t(vsN) << 32) | psN;
    auto it = progCache_.find(key);
    if (it == progCache_.end()) {
        if (!linkBudgetOk()) {
            extern unsigned long g_kbSkipPending; ++g_kbSkipPending;
            return false;                   // kick the link on a later frame
        }
        ++s_linksThisPres;
        unsigned prog = glCreateProgram();
        glAttachShader(prog, vsN);
        glAttachShader(prog, psN);
        // Match the device's canonical attribute locations (see gl_shader.cpp).
        GLBindAttribLocations(prog);
        glLinkProgram(prog);
        LinkedProgram lp; lp.prog = prog;   // ready=false; finalized once the link completes
        extern unsigned long g_kbProgLinks;
        ++g_kbProgLinks;
        it = progCache_.emplace(key, lp).first;
    }

    LinkedProgram &lp = it->second;
    if (!lp.prog) {
        // A failed link was deleted for retry. Cooldown + bounded attempts: a real
        // (deterministic) link error stops after 8 tries; a transient GPU-process
        // failure heals on a later frame instead of leaving materials black forever.
        if (lp.linkTries >= 16 || g_kbPresentEnter - lp.lastFailPres < 10 || !linkBudgetOk()) {
            extern unsigned long g_kbSkipPending; ++g_kbSkipPending;
            return false;
        }
        ++s_linksThisPres;
        unsigned prog = glCreateProgram();
        glAttachShader(prog, vsN);
        glAttachShader(prog, psN);
        GLBindAttribLocations(prog);
        glLinkProgram(prog);
        lp.prog = prog; lp.ready = false; lp.pendPolls = 0; lp.bindOk = false;
        extern unsigned long g_kbProgLinks; ++g_kbProgLinks;
    }
    if (!lp.ready && !finalizeProgram(lp)) {
        extern unsigned long g_kbSkipPending; ++g_kbSkipPending;
        return false;                        // still compiling this frame -> skip the draw
    }

    if (curProgram_ != lp.prog) {
        glUseProgram(lp.prog); curProgram_ = lp.prog;
        if (!lp.bindOk) {
            // Sampler sN reads unit N. Left at the default 0, a cube or 3D sampler shares
            // unit 0 with a 2D one and the draw fails with GL_INVALID_OPERATION (the lit
            // world and model passes).
            lp.bindOk = true;
            for (int i = 0; i < kMaxStages; ++i)
                if (lp.samplerLoc[i] >= 0) glUniform1i(lp.samplerLoc[i], i);
        }
    }
    if (lp.vscLoc >= 0 && lp.vscCount > 0 && lp.upVsVer != vsVer_) {
            glUniform4fv(lp.vscLoc, lp.vscCount, vsConst_);
        lp.upVsVer = vsVer_;
    }
    if (lp.pscLoc >= 0 && lp.pscCount > 0 && lp.upPsVer != psVer_) {
            glUniform4fv(lp.pscLoc, lp.pscCount, psConst_);
        lp.upPsVer = psVer_;
    }

    if (lp.posFixupLoc >= 0) {
        float fx = 1.0f / (float)vpWidth_, fy = 1.0f / (float)vpHeight_;
        if (lp.upPosFixup[0] != fx || lp.upPosFixup[1] != fy) {
            glUniform4f(lp.posFixupLoc, fx, fy, 0.0f, 0.0f);
            lp.upPosFixup[0] = fx; lp.upPosFixup[1] = fy;
        }
    }

#ifdef KB_GL_MODERN_GLSL
    // Feed the in-shader alpha test. uAlphaTestFunc carries the D3DCMP_* value
    // (1..8) when enabled, 0 when disabled; uAlphaRef is the normalized [0,1] ref.
    // Gated on change: these vary per material batch, not per draw — unconditional
    // re-upload was 2 GL calls on every one of ~10k draws.
    if (lp.alphaFuncLoc >= 0) {
        int af = alphaTestOn_ ? (int)alphaFunc_ : 0;
        if (lp.upAlphaFunc != af) { glUniform1i(lp.alphaFuncLoc, af); lp.upAlphaFunc = af; }
    }
    if (lp.alphaRefLoc >= 0) {
        float ar = (float)alphaRef_ / 255.0f;
        if (lp.upAlphaRef != ar) { glUniform1f(lp.alphaRefLoc, ar); lp.upAlphaRef = ar; }
    }
#endif

    // Bind each referenced sampler s# to texture unit # and the matching texture.
    // Uses the cached location (queried once at link) — no per-draw glGetUniformLocation.
    for (int i = 0; i < kMaxStages; ++i) {
        int loc = lp.samplerLoc[i];
        if (loc < 0) continue;
        // (sampler->unit binding now done once at link, see finalizeProgram)
        if (boundTexName_[i]) {
            // Per-unit cache: rebinding the same texture (the common case inside a
            // material batch) costs two GL calls per stage per draw for nothing.
            if (unitTex_[i] != boundTexName_[i]) {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(boundTexTarget_[i], boundTexName_[i]);
                unitTex_[i] = boundTexName_[i];
                applyStageSampler(i, boundTexTarget_[i]);
            }
        }
    }
    return true;
}
