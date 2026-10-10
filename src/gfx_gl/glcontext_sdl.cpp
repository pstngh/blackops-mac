// glcontext_sdl.cpp — SDL2 implementation of GLContext.
//
// SDL2 gives us one window/GL-context/input path across Linux, macOS and
// Windows-GL, so this single file is the entire window-system dependency of the
// GL backend. GLEW is initialised here so the rest of src/gfx_gl can call modern
// GL entry points directly.
#include "glcontext.h"
#include "gl_platform.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <cstdio>


#include "../platform/sdl/sdl_display.h"
#include "../platform/sdl/sdl_mainthread.h"

#include <algorithm>
#include <atomic>
#include <chrono>

// Per-present counter (gl_query.cpp). The program cache's per-frame link budget
// resets when it changes; without the increment here only the first 4 programs ever
// linked and every other draw was skipped as "still linking".
extern unsigned long g_kbPresentEnter;
extern unsigned long g_kbDraws, g_kbBlits, g_kbUploads;   // gl_query.cpp, for KB_FPS
extern unsigned long long g_kbUploadBytes, g_kbOrphanBytes;

namespace {
// KB_FPS=<seconds>: frames per second, the time between presents (average, worst),
// the time spent in the swap itself (waiting for vsync or the GPU) and, per frame, the
// draw calls, StretchRect blits, buffer uploads (count, bytes) and buffer storage
// orphaned by DISCARD locks, every <seconds> (1 if empty or 0). The render thread presents.
void SwapWithStats(SDL_Window *win) {
    static const double interval = [] {
        const char *e = getenv("KB_FPS");
        if (!e) return 0.0;
        const double s = atof(e);
        return s > 0.0 ? s : 1.0;
    }();
    if (interval <= 0.0) { SDL_GL_SwapWindow(win); return; }
    using clock = std::chrono::steady_clock;
    static clock::time_point start = clock::now(), last = start;
    static unsigned frames;
    static unsigned long draws0, blits0, uploads0;
    static unsigned long long uploadBytes0, orphanBytes0;
    static double sumFrame, maxFrame, sumSwap;
    const clock::time_point t0 = clock::now();
    SDL_GL_SwapWindow(win);
    const clock::time_point t1 = clock::now();
    const double frame = std::chrono::duration<double, std::milli>(t1 - last).count();
    last = t1;
    ++frames;
    sumFrame += frame;
    maxFrame = std::max(maxFrame, frame);
    sumSwap += std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double elapsed = std::chrono::duration<double>(t1 - start).count();
    if (elapsed >= interval) {
        fprintf(stderr, "[fps] %.1f fps, frame %.2f ms avg %.2f max, swap %.2f ms avg, %lu draws/frame; "
                        "per frame: %lu blits, %lu uploads %llu KB, %llu KB orphaned\n",
                frames / elapsed, sumFrame / frames, maxFrame, sumSwap / frames, (g_kbDraws - draws0) / frames,
                (g_kbBlits - blits0) / frames, (g_kbUploads - uploads0) / frames,
                (g_kbUploadBytes - uploadBytes0) / frames / 1024, (g_kbOrphanBytes - orphanBytes0) / frames / 1024);
        start = t1;
        frames = 0;
        draws0 = g_kbDraws; blits0 = g_kbBlits; uploads0 = g_kbUploads;
        uploadBytes0 = g_kbUploadBytes; orphanBytes0 = g_kbOrphanBytes;
        sumFrame = maxFrame = sumSwap = 0.0;
    }
}

// The window's size in pixels for Present, which scales the back buffer to it, and
// in points (the window's coordinates; a Retina display has 2x2 pixels per point).
// macOS answers window queries only on the main thread, so that thread records them
// (KB_GLNoteWindowSize) and the render thread reads the copy.
std::atomic<int> g_drawableWidth{0}, g_drawableHeight{0};
std::atomic<int> g_windowWidth{0}, g_windowHeight{0};

// Main thread: the window's size in pixels. On macOS, with displays of different
// pixel densities (a Retina display and a plain one), SDL reports a high-density
// window's drawable at 2x on the plain display too, though its GL surface there is
// 1x: the surface follows the density of the display the window is on.
void KB_WindowPixelSize(SDL_Window *win, int *w, int *h) {
#if defined(__APPLE__)
    int pw = 0, ph = 0;
    SDL_GetWindowSize(win, &pw, &ph);
    const int density = (SDL_GetWindowFlags(win) & SDL_WINDOW_ALLOW_HIGHDPI)
                            ? Sys_DisplayPixelDensity(SDL_GetWindowDisplayIndex(win)) : 1;
    *w = pw * density;
    *h = ph * density;
#else
    SDL_GL_GetDrawableSize(win, w, h);
#endif
}
} // namespace

// Main thread: the window was created, resized, moved to another display or changed
// fullscreen state (the event pump calls this for SDL's window events too).
void KB_GLNoteWindowSize(SDL_Window *win) {
    static bool wasFullscreen;
    static int lastDisplay = -1;
    int w = 0, h = 0, pw = 0, ph = 0;
    if (win) {
        KB_WindowPixelSize(win, &w, &h);
        SDL_GetWindowSize(win, &pw, &ph);
    }
    const bool fullscreen = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN);
    const int display = win ? SDL_GetWindowDisplayIndex(win) : -1;
    if (w != g_drawableWidth || h != g_drawableHeight || pw != g_windowWidth || fullscreen != wasFullscreen || display != lastDisplay) {
        char points[48] = "";
        if (pw != w || ph != h)
            snprintf(points, sizeof(points), " (%dx%d points)", pw, ph);
        const char *name = display >= 0 ? SDL_GetDisplayName(display) : nullptr;
        fprintf(stderr, "[gl] window %dx%d%s%s on display %d (%s)\n", w, h, points, fullscreen ? " fullscreen" : "",
                display, name ? name : "?");
    }
    wasFullscreen = fullscreen;
    lastDisplay = display;
    Sys_NoteGameWindowDisplay(display);
    g_drawableWidth = w;
    g_drawableHeight = h;
    g_windowWidth = pw;
    g_windowHeight = ph;
}

// A position in the window (points, as SDL reports the cursor) in its pixels, and
// back (IN_Frame, IN_SetCursorPos).
void KB_GLWindowToPixels(int *x, int *y) {
    const int pw = g_windowWidth, ph = g_windowHeight, w = g_drawableWidth, h = g_drawableHeight;
    if (pw <= 0 || ph <= 0 || w <= 0 || h <= 0) return;
    *x = (int)((long long)*x * w / pw);
    *y = (int)((long long)*y * h / ph);
}

void KB_GLPixelsToWindow(int *x, int *y) {
    const int pw = g_windowWidth, ph = g_windowHeight, w = g_drawableWidth, h = g_drawableHeight;
    if (pw <= 0 || ph <= 0 || w <= 0 || h <= 0) return;
    *x = (int)((long long)*x * pw / w);
    *y = (int)((long long)*y * ph / h);
}

namespace {

class SDLGLContext final : public GLContext {
public:
    // The renderer creates its device on the render thread, but macOS only lets the
    // main thread create windows and GL contexts: create them there, then make the
    // context current on this thread.
    bool init(const GLContextDesc &desc) {
        desc_ = &desc;
        Sys_RunOnMainThread([](void *self) { static_cast<SDLGLContext *>(self)->created_ = static_cast<SDLGLContext *>(self)->create(); }, this);
        if (!created_) return false;
        SDL_GL_MakeCurrent(win_, ctx_);
        SetVSync(desc.vsync);

        glewExperimental = GL_TRUE;
        GLenum ge = glewInit();
        if (ge != GLEW_OK) {
            fprintf(stderr, "[gl] glewInit: %s\n", glewGetErrorString(ge));
            return false;
        }
        glGetError();  // GLEW can leave a benign GL_INVALID_ENUM behind on core profiles.
        return true;
    }

    ~SDLGLContext() override {
        Sys_RunOnMainThread([](void *self) { static_cast<SDLGLContext *>(self)->destroy(); }, this);
    }

    void  MakeCurrent() override        { SDL_GL_MakeCurrent(win_, ctx_); }
    void  SwapBuffers() override        { ++g_kbPresentEnter; SwapWithStats(win_); }
    void  Resize(int w, int h) override {
        size_[0] = w; size_[1] = h;
        Sys_RunOnMainThread([](void *self) {
            auto *c = static_cast<SDLGLContext *>(self);
            // A fullscreen window keeps covering the display; Present scales.
            if (!c->fullscreen_) c->fitWindow();
            KB_GLNoteWindowSize(c->win_);
        }, this);
    }
    void  SetFullscreen(bool on) override {
        if (on == fullscreen_) return;
        fullscreen_ = on;
        Sys_RunOnMainThread([](void *self) {
            auto *c = static_cast<SDLGLContext *>(self);
            if (c->fullscreen_) {
                SDL_SetWindowFullscreen(c->win_, SDL_WINDOW_FULLSCREEN_DESKTOP);
            } else {
                const int display = SDL_GetWindowDisplayIndex(c->win_);
                SDL_SetWindowFullscreen(c->win_, 0);
                c->fitWindow();
                SDL_SetWindowPosition(c->win_, SDL_WINDOWPOS_CENTERED_DISPLAY(display > 0 ? display : 0),
                                      SDL_WINDOWPOS_CENTERED_DISPLAY(display > 0 ? display : 0));
            }
            KB_GLNoteWindowSize(c->win_);
        }, this);
    }
    // The context is current on this (the render) thread.
    void  SetVSync(bool on) override    { SDL_GL_SetSwapInterval(on ? 1 : 0); }
    void  GetDrawableSize(int *w, int *h) override { *w = g_drawableWidth; *h = g_drawableHeight; }
    void *GetProcAddress(const char *n) override { return SDL_GL_GetProcAddress(n); }

private:
    bool create() {
        const GLContextDesc &desc = *desc_;
#if defined(__APPLE__)
        // As sdl_window.cpp's InitVideo (whichever starts video first): fullscreen
        // without a Space animation, and swaps that don't wait for the main thread.
        SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
        SDL_SetHint(SDL_HINT_MAC_OPENGL_ASYNC_DISPATCH, "1");
#endif
        if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "[gl] SDL_InitSubSystem(VIDEO): %s\n", SDL_GetError());
            return false;
        }
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE,     8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE,   8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,    8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,   8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,   desc.depthStencil ? 24 : 0);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, desc.depthStencil ? 8  : 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, desc.doubleBuffer ? 1  : 0);
#if defined(KB_GL_CORE)
        // macOS: 4.1 core is the only modern context (compatibility stops at 2.1).
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,  SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,         SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif

        Uint32 flags = SDL_WINDOW_OPENGL | (desc.visible ? 0u : Uint32(SDL_WINDOW_HIDDEN));
        if (desc.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        int density = 1;
#if defined(__APPLE__)
        // Retina: the window gets a pixel per back-buffer pixel, so its size in
        // points is the back buffer's divided by the display's pixel density.
        flags |= SDL_WINDOW_ALLOW_HIGHDPI;
        density = Sys_DisplayPixelDensity(desc.display);
#endif
        fullscreen_ = desc.fullscreen;
        size_[0] = desc.width; size_[1] = desc.height;
        win_ = SDL_CreateWindow("KisakBlack", SDL_WINDOWPOS_CENTERED_DISPLAY(desc.display),
                                SDL_WINDOWPOS_CENTERED_DISPLAY(desc.display),
                                desc.width / density, desc.height / density, flags);
        if (!win_) { fprintf(stderr, "[gl] SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
        if (!fullscreen_) fitWindow();
        KB_GLNoteWindowSize(win_);
        // Request raise + keyboard focus so menu/game input (which needs X input
        // focus, unlike the polled mouse position) reaches the window.
        if (desc.visible) SDL_RaiseWindow(win_);

        ctx_ = SDL_GL_CreateContext(win_);
        if (!ctx_) { fprintf(stderr, "[gl] SDL_GL_CreateContext: %s\n", SDL_GetError()); return false; }
        SDL_GL_MakeCurrent(win_, nullptr);  // init() makes it current on the calling thread
        return true;
    }

    void destroy() {
        if (ctx_) SDL_GL_DeleteContext(ctx_);
        if (win_) SDL_DestroyWindow(win_);
    }

    // Main thread: size the window so its pixels are the back buffer's (size_), at
    // the pixel density of the display it is on.
    void fitWindow() {
        int pw = 0, ph = 0, w = 0, h = 0;
        SDL_GetWindowSize(win_, &pw, &ph);
        KB_WindowPixelSize(win_, &w, &h);
        if (pw <= 0 || ph <= 0 || w <= 0 || h <= 0) return;
        const int fitW = (int)((long long)size_[0] * pw / w), fitH = (int)((long long)size_[1] * ph / h);
        if (fitW != pw || fitH != ph) SDL_SetWindowSize(win_, fitW, fitH);
    }

    const GLContextDesc *desc_ = nullptr;
    bool          created_ = false;
    bool          fullscreen_ = false;
    int           size_[2] = {};       // the windowed size (the back buffer's)
    SDL_Window   *win_ = nullptr;
    SDL_GLContext ctx_ = nullptr;
};

} // namespace

GLContext *GLContext::Create(const GLContextDesc &desc) {
    auto *c = new SDLGLContext();
    if (!c->init(desc)) { delete c; return nullptr; }
    return c;
}

