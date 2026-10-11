// sys_platform.cpp — Linux implementations of the Sys_*/NET_*/IN_* platform layer
// that the engine calls (declared in src/win32/*.h, whose .cpp impls are Windows-only
// and excluded from the Linux build). Load-critical paths (critical sections,
// filesystem, the event queue, timing, error/exit) are implemented for real (UDP
// networking in sys_net.cpp); the non-essential Windows extras (debug sockets,
// splash/console, hotkeys) are no-ops.
#include <win32/win_main.h>
#include <win32/win_common.h>
#include <universal/dvar.h>   // _Dvar_RegisterBool (sys_SSE registration)
#include <qcommon/cmd.h>      // net_restart
#include <win32/win_shared.h>
#include <win32/win_net.h>
#include <win32/win_input.h>
#include <win32/win_wndproc.h>
#include <client/cl_keys.h>   // Key_StringToKeynum (kb_key)

#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <mach-o/dyld.h>         // _NSGetExecutablePath
#endif
#include <climits>
#include <cstdlib>
#include <dirent.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../sdl/sdl_events.h"   // Sys_PumpSDLEvents
#include <SDL2/SDL.h>            // SDL_GetMouseState / relative-mouse mode (IN_Frame)

// Client mouse entry point (src/client_mp/cl_input_mp.cpp). Declared directly to
// avoid pulling the whole client header into the platform layer; __cdecl is the
// default on x86-32 so this resolves to the same symbol.
int CL_MouseEvent(int x, int y, int dx, int dy);
// The window can be larger than the back buffer (fullscreen), which Present scales
// into it (gl_d3d9.cpp): the engine's cursor positions are back-buffer pixels. SDL's
// are the window's points, which on a Retina display are 2x2 pixels
// (glcontext_sdl.cpp).
void KB_WindowToBackbuffer(int *x, int *y);
void KB_BackbufferToWindow(int *x, int *y);
void KB_GLWindowToPixels(int *x, int *y);
void KB_GLPixelsToWindow(int *x, int *y);

// ---- The window-vars global (normally in win_wndproc.cpp) ------------------
WinVars_t g_wv;

// ---- Critical sections: 75 recursive pthread mutexes -----------------------
namespace {
pthread_mutex_t g_critSects[CRITSECT_COUNT];
bool g_critInit = false;
void EnsureCritInit() {
    if (g_critInit) return; g_critInit = true;
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    for (int i = 0; i < CRITSECT_COUNT; ++i) pthread_mutex_init(&g_critSects[i], &a);
    pthread_mutexattr_destroy(&a);
}
}
void Sys_InitializeCriticalSections() { EnsureCritInit(); }
void Sys_EnterCriticalSection(CriticalSection s) { EnsureCritInit(); if ((int)s < CRITSECT_COUNT) pthread_mutex_lock(&g_critSects[s]); }
void Sys_LeaveCriticalSection(CriticalSection s) { if ((int)s < CRITSECT_COUNT) pthread_mutex_unlock(&g_critSects[s]); }
bool Sys_TryEnterCriticalSection(CriticalSection s) { EnsureCritInit(); return (int)s < CRITSECT_COUNT && pthread_mutex_trylock(&g_critSects[s]) == 0; }

// ---- Timing ----------------------------------------------------------------
unsigned int Sys_MillisecondsRaw() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned int)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull);
}
void Sys_SnapVector(float *v) { if (v) { v[0] = floorf(v[0] + 0.5f); v[1] = floorf(v[1] + 0.5f); v[2] = floorf(v[2] + 0.5f); } }

// ---- Filesystem ------------------------------------------------------------
static char g_cwd[4096];
char *Sys_Cwd() { if (!getcwd(g_cwd, sizeof(g_cwd))) g_cwd[0] = '\0'; return g_cwd; }
void  Sys_Mkdir(const char *path) { if (path) mkdir(path, 0755); }
const char *Sys_DefaultCDPath() { return ""; }
// The game folder: the executable's directory when the game data is there (a
// portable install keeps the binary and its libraries in the game folder, and
// Finder starts a program in the home directory), else the current directory, as
// the original's base path (the dev builds run from the game folder). Everything
// the game writes goes under it (fs_h defaults to the base path).
char *Sys_DefaultInstallPath() {
    static char installPath[PATH_MAX];
    if (!installPath[0]) {
        char exe[PATH_MAX], dir[PATH_MAX];
        bool found = false;
#if defined(__APPLE__)
        uint32_t size = sizeof(exe);
        found = _NSGetExecutablePath(exe, &size) == 0;
#else
        ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        found = len > 0;
        if (found) exe[len] = '\0';
#endif
        if (found && realpath(exe, dir)) {
            if (char *slash = strrchr(dir, '/')) *slash = '\0';
            struct stat st;
            char sub[PATH_MAX];
            snprintf(sub, sizeof(sub), "%s/main", dir);
            found = !stat(sub, &st) && S_ISDIR(st.st_mode);
            snprintf(sub, sizeof(sub), "%s/zone", dir);
            found = found && !stat(sub, &st) && S_ISDIR(st.st_mode);
        } else {
            found = false;
        }
        snprintf(installPath, sizeof(installPath), "%s", found ? dir : Sys_Cwd());
    }
    return installPath;
}
int Sys_DirectoryHasContents(const char *dir) {
    if (!dir) return 0;
    char path[4096]; int i = 0; for (; dir[i] && i < 4095; ++i) path[i] = dir[i] == '\\' ? '/' : dir[i]; path[i] = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    int has = 0; struct dirent *e;
    while ((e = readdir(d))) { if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { has = 1; break; } }
    closedir(d); return has;
}

// ---- Error / exit / print --------------------------------------------------
void Sys_Print(char *msg) { if (msg) { fputs(msg, stdout); fflush(stdout); } }
void Sys_Error(char *error, ...) {
    char buf[4096]; va_list ap; va_start(ap, error); vsnprintf(buf, sizeof(buf), error, ap); va_end(ap);
    fprintf(stderr, "\n********** Sys_Error **********\n%s\n", buf); fflush(stderr);
    _exit(1);
}
void Sys_Quit()        { _exit(0); }
void Sys_NormalExit()  {}
void Sys_QuitAndStartProcess(const char *) { _exit(0); }
void Sys_OutOfMemErrorInternal(const char *file, int line) { fprintf(stderr, "out of memory (%s:%d)\n", file ? file : "?", line); _exit(1); }
void Sys_DirectXFatalError() { fprintf(stderr, "graphics init failed\n"); _exit(1); }

// ---- System info -----------------------------------------------------------
// win_main.cpp fills sys_info in Sys_FindInfo and registers the info dvars in
// Sys_RegisterInfoDvars; neither is compiled here. The renderer caps texture detail
// from sys_sysMB (0 MB forced picmip 2), and autoconfigure picks its configure_mp.csv
// row from configureGHz and sysMB. The engine dereferences sys_SSE without a null
// check (e.g. R_SkinXModelCmd's SSE-skinning gate).
const dvar_t *sys_configureGHz;
const dvar_t *sys_sysMB;
const dvar_t *sys_gpu;
const dvar_t *sys_configSum;
const dvar_t *sys_SSE = nullptr;
static SysInfo sys_info;
static bool sys_infoFound;

void Sys_FindInfo() {
    sys_infoFound = true;
    memset(&sys_info, 0, sizeof(sys_info));
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    sys_info.logicalCpuCount = cpus > 0 ? (int)cpus : 1;
    sys_info.physicalCpuCount = sys_info.logicalCpuCount;
    unsigned long long memBytes = (unsigned long long)sysconf(_SC_PHYS_PAGES) * (unsigned long long)sysconf(_SC_PAGE_SIZE);
    double ghz = 0.0;
#if defined(__APPLE__)
    int physical = 0;
    size_t len = sizeof(physical);
    if (sysctlbyname("hw.physicalcpu", &physical, &len, nullptr, 0) == 0 && physical > 0)
        sys_info.physicalCpuCount = physical;
    unsigned long long value = 0;
    len = sizeof(value);
    if (sysctlbyname("hw.memsize", &value, &len, nullptr, 0) == 0 && value)
        memBytes = value;
    value = 0;
    len = sizeof(value);
    if (sysctlbyname("hw.cpufrequency", &value, &len, nullptr, 0) == 0 && value)   // absent on Apple Silicon
        ghz = (double)value / 1.0e9;
    len = sizeof(sys_info.cpuName);
    if (sysctlbyname("machdep.cpu.brand_string", sys_info.cpuName, &len, nullptr, 0) != 0)
        sys_info.cpuName[0] = 0;
    sys_info.cpuName[sizeof(sys_info.cpuName) - 1] = 0;
#else
    if (FILE *f = fopen("/proc/cpuinfo", "r")) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            const char *colon = strchr(line, ':');
            if (!colon) continue;
            const char *v = colon + 1;
            while (*v == ' ' || *v == '\t') ++v;
            if (!sys_info.cpuName[0] && !strncmp(line, "model name", 10))
                snprintf(sys_info.cpuName, sizeof(sys_info.cpuName), "%.*s", (int)strcspn(v, "\n"), v);
            else if (!sys_info.cpuVendor[0] && !strncmp(line, "vendor_id", 9))
                snprintf(sys_info.cpuVendor, sizeof(sys_info.cpuVendor), "%.*s", (int)strcspn(v, "\n"), v);
            else if (ghz == 0.0 && !strncmp(line, "cpu MHz", 7))
                ghz = atof(v) / 1000.0;
        }
        fclose(f);
    }
#endif
    sys_info.sysMB = (int)(memBytes >> 20);
    // Sys_BenchmarkGHz is a stub on Windows too ("swag", 2.4); scale it the same way
    // Sys_SetAutoConfigureGHz does.
    if (ghz == 0.0) ghz = 2.4;
    sys_info.cpuGHz = ghz;
    double multiCpuFactor = sys_info.physicalCpuCount == 1 ? 1.0 : sys_info.physicalCpuCount == 2 ? 1.75 : 2.0;
    sys_info.configureGHz = ghz * multiCpuFactor;
    sys_info.SSE = true;
}

// Windows calls Sys_FindInfo from WinMain; here autoconfigure (Com_SetRecommended)
// reads the info before Sys_Init runs.
static void Sys_EnsureInfo() { if (!sys_infoFound) Sys_FindInfo(); }

void Sys_RegisterInfoDvars() {
    Sys_EnsureInfo();
    sys_configureGHz = _Dvar_RegisterFloat("sys_configureGHz", 0.0f, -3.4028235e38f, 3.4028235e38f, 0x11u,
                                           "Normalized total CPU power, based on cpu type, count, and speed; used in autoconfigure");
    sys_sysMB = _Dvar_RegisterInt("sys_sysMB", 0, 0x80000000, 0x7FFFFFFF, 0x11u, "Physical memory in the system");
    sys_gpu = _Dvar_RegisterString("sys_gpu", (char *)"", 0x11u, "GPU description");
    sys_configSum = _Dvar_RegisterInt("sys_configSum", 0, 0x80000000, 0x7FFFFFFF, 0x11u, "Configuration checksum");
    sys_SSE = _Dvar_RegisterBool("sys_SSE", sys_info.SSE, 0x40u, "Operating system allows Streaming SIMD Extensions");
    _Dvar_RegisterFloat("sys_cpuGHz", (float)sys_info.cpuGHz, -3.4028235e38f, 3.4028235e38f, 0x40u, "Measured CPU speed");
    _Dvar_RegisterString("sys_cpuName", sys_info.cpuName, 0x40u, "CPU name description");
}

static cmd_function_s Sys_Net_Restart_f_VAR;
static void Sys_Net_Restart_f() { NET_Restart(); }

void Sys_Init() {
    EnsureCritInit();
    Cmd_AddCommandInternal("net_restart", Sys_Net_Restart_f, &Sys_Net_Restart_f_VAR);
    Sys_RegisterInfoDvars();   // Com_InitDvars() has already run
}
void Sys_GetInfo(SysInfo *info) { Sys_EnsureInfo(); if (info) memcpy(info, &sys_info, sizeof(SysInfo)); }
// Windows asks with a message box before re-running autoconfigure for a changed
// configure_mp.csv or changed hardware; there is no dialog here, so it re-runs.
bool Sys_HasConfigureChecksumChanged(int checksum) {
    Sys_RegisterInfoDvars();
    bool changed = sys_configSum->current.integer && sys_configSum->current.integer != checksum;
    if (sys_configSum->current.integer != checksum) Dvar_SetInt((dvar_s *)sys_configSum, checksum);
    return changed;
}
bool Sys_HasInfoChanged() {
    Sys_RegisterInfoDvars();
    return sys_configureGHz->current.value > sys_info.configureGHz * 1.100000023841858
        || sys_info.configureGHz * 0.8999999761581421 > sys_configureGHz->current.value
        || sys_sysMB->current.integer > sys_info.sysMB + 32
        || sys_sysMB->current.integer < sys_info.sysMB - 32
        || strcmp(sys_gpu->current.string, sys_info.gpuDescription);
}
void Sys_ArchiveInfo(int checksum) {
    Sys_RegisterInfoDvars();
    Dvar_SetFloat((dvar_s *)sys_configureGHz, (float)sys_info.configureGHz);
    Dvar_SetInt((dvar_s *)sys_sysMB, sys_info.sysMB);
    Dvar_SetString((dvar_s *)sys_gpu, sys_info.gpuDescription);
    Dvar_SetInt((dvar_s *)sys_configSum, checksum);
}
bool Sys_IsMiniDumpStarted() { return false; }

// ---- Event queue: SDL-fed ring buffer --------------------------------------
namespace {
sysEvent_t g_eventQue[256];
int g_eventHead = 0, g_eventTail = 0;
}
void Sys_QueEvent(unsigned int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr) {
    sysEvent_t *ev = &g_eventQue[g_eventHead & 255];
    if (g_eventHead - g_eventTail >= 256) { if (ev->evPtr) free(ev->evPtr); ++g_eventTail; }
    ++g_eventHead;
    ev->evTime = time ? time : Sys_Milliseconds();
    ev->evType = type; ev->evValue = value; ev->evValue2 = value2;
    ev->evPtrLength = ptrLength; ev->evPtr = ptr;
}
sysEvent_t *Sys_GetEvent(sysEvent_t *result) {
    if (g_eventHead == g_eventTail) Sys_PumpSDLEvents(Sys_Milliseconds());   // pull from SDL
    if (g_eventHead > g_eventTail) { *result = g_eventQue[g_eventTail & 255]; ++g_eventTail; return result; }
    memset(result, 0, sizeof(*result)); result->evTime = Sys_Milliseconds(); return result;
}
void Sys_LoadingKeepAlive() { Sys_PumpSDLEvents(Sys_Milliseconds()); }

// ---- Input / window (driven by the SDL layer / GL backend) -----------------
// Test tools for the menus, run from KB_MENU_CMDS or KB_CMDS (CLAUDE.md): `kb_mouse <x> <y>` puts the menu
// cursor at that back-buffer pixel in place of the mouse's position (no arguments: the mouse again), and
// `kb_key <key>` presses and releases a key by its bind name (MOUSE1 clicks there, ESCAPE, ENTER, ...).
static bool kbMouseOverride;
static int kbMouseX, kbMouseY;
static cmd_function_s KB_Mouse_f_VAR;
static cmd_function_s KB_Key_f_VAR;

static void KB_Mouse_f() {
    kbMouseOverride = Cmd_Argc() >= 3;
    if (kbMouseOverride) {
        kbMouseX = atoi(Cmd_Argv(1));
        kbMouseY = atoi(Cmd_Argv(2));
    }
}
static void KB_Key_f() {
    int key = Cmd_Argc() >= 2 ? Key_StringToKeynum(Cmd_Argv(1)) : -1;
    if (key < 0) {
        Com_Printf(0, "usage: kb_key <key>, a bind name such as MOUSE1, ESCAPE, ENTER or a letter\n");
        return;
    }
    Sys_QueEvent(Sys_Milliseconds(), SE_KEY, key, 1, 0, nullptr);
    Sys_QueEvent(Sys_Milliseconds(), SE_KEY, key, 0, 0, nullptr);
}

// Poll the mouse each frame and feed it to the client, mirroring win_input.cpp's
// IN_Frame -> IN_MouseMove -> CL_MouseEvent. SDL events are pumped every frame by
// Com_EventLoop (Sys_GetEvent -> Sys_PumpSDLEvents), so SDL_GetMouseState is
// current. CL_MouseEvent routes the window-relative position to the menu cursor
// (UI_MouseEvent) when a menu is open, or accumulates the delta for game look
// otherwise; its return value (recenterMouse) requests relative/captured mode,
// which maps cleanly to SDL's relative mouse mode.
void IN_Frame() {
    // In-game look uses relative-mouse mode: SDL captures and recenters the cursor,
    // so absolute SDL_GetMouseState positions jump around and differencing them gives
    // garbage deltas (the view "teleports" while looking). Use SDL_GetRelativeMouseState
    // for motion when captured; only the menu path needs the absolute cursor position.
    static bool relative = false;
    static bool commandsAdded = false;
    int x = 0, y = 0, dx = 0, dy = 0;
    if (!commandsAdded) {
        commandsAdded = true;
        Cmd_AddCommandInternal("kb_mouse", KB_Mouse_f, &KB_Mouse_f_VAR);
        Cmd_AddCommandInternal("kb_key", KB_Key_f, &KB_Key_f_VAR);
    }
    if (relative) {
        SDL_GetRelativeMouseState(&dx, &dy);   // accumulated look deltas since last call
        SDL_GetMouseState(&x, &y);             // position (unused in-game, but harmless)
    } else {
        SDL_GetMouseState(&x, &y);             // absolute window-relative cursor (menu)
        KB_GLWindowToPixels(&x, &y);
        KB_WindowToBackbuffer(&x, &y);
        if (kbMouseOverride) { x = kbMouseX; y = kbMouseY; }
        static int oldX = 0, oldY = 0;
        static bool primed = false;
        if (!primed) { oldX = x; oldY = y; primed = true; }
        dx = x - oldX; dy = y - oldY;
        oldX = x; oldY = y;
    }
    int recenter = CL_MouseEvent(x, y, dx, dy);
    if ((bool)recenter != relative) {
        relative = recenter != 0;
        SDL_SetRelativeMouseMode(relative ? SDL_TRUE : SDL_FALSE);
        if (relative) SDL_GetRelativeMouseState(nullptr, nullptr);  // drop the entry-frame jump
    }
}
void IN_SetCursorPos(unsigned int x, unsigned int y) {
    int wx = (int)x, wy = (int)y;
    KB_BackbufferToWindow(&wx, &wy);
    KB_GLPixelsToWindow(&wx, &wy);
    SDL_WarpMouseInWindow(nullptr, wx, wy);
}
void IN_ShowSystemCursor(bool show) { SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE); }

// ---- Misc Windows extras: no-ops on Linux ----------------------------------
char *Sys_GetClipboardData() { return nullptr; }
void  Sys_OpenURL(const char *, int) {}
void Sys_ShowConsole() {}
void  Sys_DestroySplashWindow() {}
void  Sys_HideSplashWindow() {}
void  Sys_UpdateHotkeyBlock() {}

// ---- Networking: UDP is in sys_net.cpp; no SOCKS proxy or client socket pool --
void NET_RestartDebug() {}
void NET_ShutdownDebug() {}
int  Sys_SocketPool_GetPacket(netadr_t *, msg_t *) { return 0; }
void Sys_CheckForNATOverflow() {}

// ---- Remote script-debug sockets: not supported (no-ops) -------------------
char Sys_StartRemoteDebugServer() { return 0; }
int  Sys_IsRemoteDebugServer() { return 0; }
void Sys_AckDebugSocket() {}
bool Sys_DebugSocketReady(int) { return false; }
int  Sys_UpdateDebugSocket() { return 0; }
void Sys_FlushDebugSocketData() {}
void Sys_EndWriteDebugSocket() {}
int  Sys_ReadDebugSocketInt() { return 0; }
char *Sys_ReadDebugSocketString() { return nullptr; }
void Sys_WriteDebugSocketData(unsigned char *, int) {}
void Sys_WriteDebugSocketInt(int) {}
void Sys_WriteDebugSocketMessageType(unsigned char) {}
void Sys_WriteDebugSocketString(char *) {}

// ---- Localization (English) ------------------------------------------------
char *Win_GetLanguage() { return (char *)"english"; }  // language NAME, not index (read from registry on Windows)
const char *Win_LocalizeRef(const char *ref) { return ref ? ref : ""; }

// ---- Aligned allocation ----------------------------------------------------
extern "C" void *_aligned_malloc(size_t size, size_t align) {
    void *p = nullptr; if (align < sizeof(void *)) align = sizeof(void *);
    if (posix_memalign(&p, align, size) != 0) return nullptr; return p;
}
extern "C" void _aligned_free(void *p) { free(p); }

// MSVC spells unlink _unlink; one decompiled TU calls it by that name.
extern "C" int _unlink(const char *path) { return unlink(path); }
