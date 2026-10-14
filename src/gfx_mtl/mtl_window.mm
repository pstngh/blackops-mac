// mtl_window.mm — the Metal backend's window (SDL, with a CAMetalLayer) and the
// startup choice between the Metal and GL backends.
#include "mtl_device.h"

#define BOOL WINBOOL   // as in mtl_d3d9.h
#include <d3dx9.h>
#undef BOOL
#include <SDL2/SDL.h>
#include <SDL2/SDL_metal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "../platform/sdl/sdl_display.h"
#include "../platform/sdl/sdl_mainthread.h"

void KB_GLNoteWindowSize(SDL_Window *win);   // glcontext_sdl.cpp: the window's size for the cursor

namespace {

// Main thread: size the window so its pixels are the back buffer's, at the pixel
// density of its display (glcontext_sdl.cpp's fitWindow).
void FitWindow(SDL_Window *win, int width, int height) {
    int pw = 0, ph = 0;
    SDL_GetWindowSize(win, &pw, &ph);
    const int density = Sys_DisplayPixelDensity(SDL_GetWindowDisplayIndex(win));
    if (pw <= 0 || ph <= 0) return;
    const int fitW = width / density, fitH = height / density;
    if (fitW != pw || fitH != ph) SDL_SetWindowSize(win, fitW, fitH);
}

struct CreateArgs {
    id<MTLDevice> dev;
    MetalWindow *w;
    int width, height, display;
    bool vsync, ok;
};

void CreateOnMain(void *p) {
    CreateArgs *a = static_cast<CreateArgs *>(p);
    MetalWindow *w = a->w;
    // As glcontext_sdl.cpp: fullscreen without a Space animation.
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[mtl] SDL_InitSubSystem(VIDEO): %s\n", SDL_GetError());
        return;
    }
    Uint32 flags = SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI;
    if (w->fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    const int density = Sys_DisplayPixelDensity(a->display);
    SDL_Window *win = SDL_CreateWindow("KisakBlack", SDL_WINDOWPOS_CENTERED_DISPLAY(a->display),
                                       SDL_WINDOWPOS_CENTERED_DISPLAY(a->display), a->width / density,
                                       a->height / density, flags);
    if (!win) { fprintf(stderr, "[mtl] SDL_CreateWindow: %s\n", SDL_GetError()); return; }
    w->sdlWindow = win;
    if (!w->fullscreen) FitWindow(win, a->width, a->height);
    KB_GLNoteWindowSize(win);
    SDL_RaiseWindow(win);
    w->view = SDL_Metal_CreateView(win);
    if (!w->view) { fprintf(stderr, "[mtl] SDL_Metal_CreateView: %s\n", SDL_GetError()); return; }
    w->layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(w->view);
    w->layer.device = a->dev;
    w->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    w->layer.framebufferOnly = YES;
    w->layer.maximumDrawableCount = 3;
    w->layer.displaySyncEnabled = a->vsync ? YES : NO;
    a->ok = true;
}

} // namespace

MetalWindow *MetalWindow::Create(id<MTLDevice> dev, int width, int height, bool fullscreen, bool vsync, int display) {
    MetalWindow *w = new MetalWindow;
    w->size[0] = width; w->size[1] = height;
    w->fullscreen = fullscreen;
    CreateArgs a = { dev, w, width, height, display, vsync, false };
    Sys_RunOnMainThread(CreateOnMain, &a);
    if (!a.ok) { delete w; return nullptr; }
    w->presenterRunning = true;
    std::thread([w] { w->presenterLoop(); }).detach();
    return w;
}

void MetalWindow::presenterLoop() {
    for (;;) {
        @autoreleasepool {
            {
                std::unique_lock<std::mutex> g(mu);
                cv.wait(g, [&] { return stop || ready == nil; });
                if (stop) break;
            }
            id<CAMetalDrawable> d = [layer nextDrawable];
            std::lock_guard<std::mutex> g(mu);
            ready = d;
            cv.notify_all();
        }
    }
    std::lock_guard<std::mutex> g(mu);
    presenterRunning = false;
    cv.notify_all();
}

id<CAMetalDrawable> MetalWindow::takeDrawable(bool wait) {
    std::unique_lock<std::mutex> g(mu);
    if (wait) cv.wait(g, [&] { return ready != nil; });
    id<CAMetalDrawable> d = ready;
    ready = nil;
    cv.notify_all();
    return d;
}

MetalWindow::~MetalWindow() {
    {
        std::unique_lock<std::mutex> g(mu);
        stop = true;
        cv.notify_all();
        cv.wait(g, [&] { return !presenterRunning; });
        ready = nil;
    }
    Sys_RunOnMainThread([](void *p) {
        MetalWindow *w = static_cast<MetalWindow *>(p);
        w->layer = nil;
        if (w->view) SDL_Metal_DestroyView(w->view);
        if (w->sdlWindow) SDL_DestroyWindow(static_cast<SDL_Window *>(w->sdlWindow));
    }, this);
}

void MetalWindow::resize(int width, int height) {
    size[0] = width; size[1] = height;
    Sys_RunOnMainThread([](void *p) {
        MetalWindow *w = static_cast<MetalWindow *>(p);
        SDL_Window *win = static_cast<SDL_Window *>(w->sdlWindow);
        if (!w->fullscreen) FitWindow(win, w->size[0], w->size[1]);   // fullscreen keeps covering the display
        KB_GLNoteWindowSize(win);
    }, this);
}

void MetalWindow::setFullscreen(bool on) {
    if (on == fullscreen) return;
    fullscreen = on;
    Sys_RunOnMainThread([](void *p) {
        MetalWindow *w = static_cast<MetalWindow *>(p);
        SDL_Window *win = static_cast<SDL_Window *>(w->sdlWindow);
        if (w->fullscreen) {
            SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        } else {
            const int display = SDL_GetWindowDisplayIndex(win);
            SDL_SetWindowFullscreen(win, 0);
            FitWindow(win, w->size[0], w->size[1]);
            SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED_DISPLAY(display > 0 ? display : 0),
                                  SDL_WINDOWPOS_CENTERED_DISPLAY(display > 0 ? display : 0));
        }
        KB_GLNoteWindowSize(win);
    }, this);
}

void MetalWindow::setVSync(bool on) {
    struct A { MetalWindow *w; bool on; } a = { this, on };
    Sys_RunOnMainThread([](void *p) {
        A *a = static_cast<A *>(p);
        a->w->layer.displaySyncEnabled = a->on ? YES : NO;
    }, &a);
}

// ---- Backend choice ----------------------------------------------------------
// This backend unless KB_RENDERER=gl: the GL layer (src/gfx_gl) stays the fallback on
// macOS and Linux's renderer.
extern "C" IDirect3D9 *WINAPI GL_Direct3DCreate9(UINT sdk);
extern "C" HRESULT WINAPI GL_D3DXSaveSurfaceToFileA(const char *file, D3DXIMAGE_FILEFORMAT fmt,
                                                     IDirect3DSurface9 *surface, const void *palette, const RECT *rect);
HRESULT MTL_SaveSurfaceToFile(const char *file, IDirect3DSurface9 *surface, const RECT *rect);

static bool KB_UseMetal() {
    static int use = -1;
    if (use < 0) {
        const char *r = getenv("KB_RENDERER");
        use = !r || strcmp(r, "gl");
        if (r && strcmp(r, "gl") && strcmp(r, "metal"))
            fprintf(stderr, "[mtl] KB_RENDERER=%s is neither metal nor gl: using Metal\n", r);
    }
    return use != 0;
}

extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk) {
    if (KB_UseMetal()) {
        fprintf(stderr, "[mtl] Metal backend (KB_RENDERER=gl for the GL layer)\n");
        return new MetalD3D9();
    }
    return GL_Direct3DCreate9(sdk);
}

extern "C" HRESULT WINAPI D3DXSaveSurfaceToFileA(const char *file, D3DXIMAGE_FILEFORMAT fmt, IDirect3DSurface9 *surface,
                                                  const void *palette, const RECT *rect) {
    if (KB_UseMetal()) return MTL_SaveSurfaceToFile(file, surface, rect);
    return GL_D3DXSaveSurfaceToFileA(file, fmt, surface, palette, rect);
}
