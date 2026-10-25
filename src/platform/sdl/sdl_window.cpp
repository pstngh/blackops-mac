// sdl_window.cpp — SDL implementations of the Win32 window/monitor APIs the
// renderer calls (declared in platform/winsdk/windows.h). The Linux build compiles
// this instead of the Win32 window code in src/win32.
//
// Window ownership: the GL backend (src/gfx_gl) creates and owns the single SDL
// window in CreateDevice. So the renderer's window-*manipulation* calls here are
// sentinels / no-ops — the SDL window is authoritative and there is nothing for the
// renderer to position or restyle. The *display* queries (screen metrics, monitor
// rectangle, monitor enumeration) return real SDL data so the renderer's resolution
// and fullscreen-rect logic gets correct numbers. The HWND/HMONITOR values the
// renderer threads around are opaque non-null sentinels: an HMONITOR is the SDL
// display index + 1, which is also the GL backend's adapter index + 1
// (GLD3D9::GetAdapterMonitor), so r_monitor picks the display the game goes
// fullscreen on.
//
// Monitor sizes are in pixels: on a Retina display (macOS) a point is 2x2 pixels,
// and the renderer's resolutions are back-buffer pixels (the GL window is sized in
// points to match). Monitor rectangles start at 0,0: the renderer compares them
// with window-relative positions (ClientToScreen is the identity here).
#include <SDL2/SDL.h>
#include <windows.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <atomic>
#include "sdl_display.h"
#include "sdl_mainthread.h"

namespace {
// Non-null opaque sentinels. The renderer only ever tests these for non-null and
// hands them straight back to APIs below — it never dereferences them.
HWND     kSentinelWindow  = (HWND)(intptr_t)1;
HMODULE  kSentinelModule  = (HMODULE)(intptr_t)1;

void InitVideo(void *) {
#if defined(__APPLE__)
    // Set before video starts, as in glcontext_sdl.cpp (which may start it first):
    // fullscreen covers the display at once rather than animating into its own
    // Space, and the render thread's buffer swap after a window change posts the GL
    // context update to the main thread instead of waiting for it, as the main
    // thread can be waiting for that frame (the swap hung after a fullscreen switch).
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
    SDL_SetHint(SDL_HINT_MAC_OPENGL_ASYNC_DISPATCH, "1");
#endif
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) SDL_InitSubSystem(SDL_INIT_VIDEO);
}

// The size of display `display` in pixels, with a 1080p fallback if SDL video is
// unavailable (e.g. headless) so callers still get a sane rectangle.
void DisplaySize(int display, RECT *out) {
    SDL_Rect r;
    Sys_EnsureSDLVideo();
    out->left = 0; out->top = 0;
    if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_GetDisplayBounds(display, &r) == 0) {
        const int density = Sys_DisplayPixelDensity(display);
        out->right = r.w * density; out->bottom = r.h * density;
    } else {
        out->right = 1920; out->bottom = 1080;
    }
}

int DisplayCount() {
    Sys_EnsureSDLVideo();
    const int n = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetNumVideoDisplays() : 0;
    return n > 0 ? n : 1;
}

HMONITOR DisplayMonitor(int display) { return (HMONITOR)(intptr_t)(display + 1); }

std::atomic<int> g_gameWindowDisplay{0};

int MonitorDisplay(HMONITOR monitor) {
    const int display = (int)(intptr_t)monitor - 1;
    return display >= 0 && display < DisplayCount() ? display : 0;
}
} // namespace

void Sys_EnsureSDLVideo() {
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) Sys_RunOnMainThread(InitVideo, nullptr);  // macOS: main thread only
}

// Pixels per point of a display: 2 on a Retina display. SDL2 reports no pixel
// density; its DPI is the display's content scale times 96 on macOS.
int Sys_DisplayPixelDensity(int display) {
#if defined(__APPLE__)
    float ddpi = 0.0f;
    Sys_EnsureSDLVideo();
    if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_GetDisplayDPI(display, &ddpi, nullptr, nullptr) == 0 && ddpi > 0.0f) {
        const int density = (int)(ddpi / 96.0f + 0.5f);
        return density > 1 ? density : 1;
    }
#endif
    (void)display;
    return 1;
}

// ---- Display metrics / monitors --------------------------------------------
int GetSystemMetrics(int nIndex) {
    RECT r; DisplaySize(0, &r);
    if (nIndex == 0) return r.right;    // SM_CXSCREEN
    if (nIndex == 1) return r.bottom;   // SM_CYSCREEN
    return 0;
}

// vid_xpos/vid_ypos pick the monitor of a window (R_ChooseMonitor): the display
// that holds the point, else the primary one.
HMONITOR MonitorFromPoint(POINT pt, DWORD) {
    const int count = DisplayCount();
    for (int display = 0; display < count; ++display) {
        SDL_Rect r;
        if (SDL_GetDisplayBounds(display, &r) == 0 && pt.x >= r.x && pt.x < r.x + r.w && pt.y >= r.y && pt.y < r.y + r.h)
            return DisplayMonitor(display);
    }
    return DisplayMonitor(0);
}

void Sys_NoteGameWindowDisplay(int display) { g_gameWindowDisplay = display > 0 ? display : 0; }

static SDL_Window *g_gameWindow;
void Sys_NoteGameWindow(SDL_Window *win) { g_gameWindow = win; }
void Sys_HideGameWindow() {
    Sys_RunOnMainThread([](void *) {
        if (!g_gameWindow) return;
        SDL_HideWindow(g_gameWindow);
#ifdef __APPLE__
        // macOS puts the change on screen as the main run loop turns (Core Animation commits
        // then), which the shutdown wouldn't let it do.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
#endif
    }, nullptr);
}

// The game window's (the screenshot code sizes its capture by this monitor).
HMONITOR MonitorFromWindow(HWND, DWORD)  { return DisplayMonitor(g_gameWindowDisplay); }

BOOL GetMonitorInfoA(HMONITOR hMonitor, LPMONITORINFO lpmi) {
    if (!lpmi) return FALSE;
    const int display = MonitorDisplay(hMonitor);
    DisplaySize(display, &lpmi->rcMonitor);
    lpmi->rcWork = lpmi->rcMonitor;             // no taskbar inset modeled
    lpmi->dwFlags = display == 0 ? 1 : 0;       // MONITORINFOF_PRIMARY
    return TRUE;
}

BOOL EnumDisplayMonitors(HDC, LPRECT, MONITORENUMPROC lpfnEnum, LPARAM dwData) {
    if (!lpfnEnum) return FALSE;
    const int count = DisplayCount();
    for (int display = 0; display < count; ++display) {
        RECT r; DisplaySize(display, &r);
        if (!lpfnEnum(DisplayMonitor(display), (HDC)0, &r, dwData))
            break;
    }
    return TRUE;
}

BOOL ClientToScreen(HWND, LPPOINT) { return TRUE; }  // SDL window client == screen origin here

// ---- Window manipulation (SDL window is authoritative -> no-ops) ------------
BOOL AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD) { return TRUE; }  // SDL owns decorations
HWND CreateWindowExA(DWORD, const char *, const char *, DWORD, int, int, int, int,
                     HWND, HMENU, HINSTANCE, void *) { return kSentinelWindow; }
BOOL DestroyWindow(HWND)              { return TRUE; }
BOOL IsWindow(HWND hWnd)             { return hWnd == kSentinelWindow; }
BOOL ShowWindow(HWND, int)            { return TRUE; }
BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return TRUE; }
LONG SetWindowLongA(HWND, int, LONG)  { return 0; }
HWND SetFocus(HWND)                   { return kSentinelWindow; }
HMODULE GetModuleHandleA(const char *) { return kSentinelModule; }
